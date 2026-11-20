#!/usr/bin/env python3
"""Request one existing M1 capture after continuous observed bedroom movement.

This does not launch or control TDS. Watch mode creates the runner's existing
capture.request marker only for a fresh, unambiguous viewer stream; replay
mode reads a saved log and never creates a marker.
"""
import argparse
from collections import deque
import json
import math
from pathlib import Path
import re
import sys
import time

import compare_live_light_diagnostics as diagnostics

MAP = "SouthQuarter1_int1"
MAP_SHA = "ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba"
SESSION_RE = re.compile(r"live:[0-9a-f]{32}\Z")
MIN_DURATION_MS = 30000
MAX_GAP_MS = 1000
MAX_LINE = 1024 * 1024
FRESH_SECONDS = 3.0


def forward(view):
    if not isinstance(view, list) or len(view) != 16:
        return None
    values = [view[i] for i in (2, 6, 10)]
    if any(type(x) not in (int, float) or not math.isfinite(x) for x in values):
        return None
    length = math.sqrt(sum(x * x for x in values))
    return [x / length for x in values] if length > 1e-9 else None


def angle(a, b):
    return math.degrees(math.acos(max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))))


def bed_count(row):
    meshes = row.get("mesh_instances")
    if type(meshes) is not int or not 0 <= meshes <= 8192:
        return None
    if "bed_source_instances" not in row:
        return meshes if meshes in (0, 1) else None
    bed = row["bed_source_instances"]
    return bed if type(bed) is int and bed in (0, 1) and bed <= meshes else None


def sample(row, session):
    if (not isinstance(row, dict) or row.get("event") != "frame" or row.get("source") != "live_process" or
            row.get("session_id") != session or row.get("map") != MAP or row.get("map_sha256") != MAP_SHA or
            type(row.get("render_pass")) is not int or row["render_pass"] != 0 or
            type(row.get("stage")) is not int or row["stage"] != 1 or
            row.get("identity_scope") not in ("visible_list", "full_object_scan") or
            bed_count(row) is None or
            type(row.get("bsp_blocks")) is not int or row["bsp_blocks"] <= 0 or
            not isinstance(row.get("viewport"), dict) or row["viewport"].get("source") != "scoped_draw"):
        return None
    for name in ("frame", "elapsed_ms", "map_epoch", "device_epoch", "continuity"):
        if type(row.get(name)) is not int or row[name] < 0:
            return None
    if not row["map_epoch"] or not row["device_epoch"] or not row["continuity"]:
        return None
    try:
        position = diagnostics.camera_position(row.get("view"))
        direction = forward(row.get("view"))
    except (ValueError, TypeError, OverflowError):
        return None
    if position is None or direction is None:
        return None
    return {"frame": row["frame"], "elapsed_ms": row["elapsed_ms"], "position": position,
            "forward": direction, "bed": bed_count(row),
            "epoch": (row["map_epoch"], row["device_epoch"], row["continuity"])}


class MovementWindow:
    def __init__(self, session):
        if not isinstance(session, str) or not SESSION_RE.fullmatch(session):
            raise ValueError("invalid viewer session")
        self.session = session
        self.reset()

    def reset(self):
        self.previous = self.baseline = self.start = None
        self.maximum_distance = self.maximum_angle = 0.0
        self.exits = self.entries = 0
        self.recent = deque()
        self.frames = 0

    def feed(self, row):
        current = sample(row, self.session)
        if current is None:
            self.reset()
            return None
        if self.previous is not None and (
                current["frame"] != self.previous["frame"] + 1 or current["epoch"] != self.previous["epoch"] or
                not 0 < current["elapsed_ms"] - self.previous["elapsed_ms"] <= MAX_GAP_MS):
            self.reset()
        if self.baseline is None:
            self.baseline = current
        if self.start is None:
            distance = math.dist(self.baseline["position"], current["position"])
            rotation = angle(self.baseline["forward"], current["forward"])
            if distance >= 8.0 or rotation >= 3.0:
                self.start = current
                self.frames = 1
                self.recent.append(current)
        else:
            self.frames += 1
            self.maximum_distance = max(self.maximum_distance, math.dist(self.start["position"], current["position"]))
            self.maximum_angle = max(self.maximum_angle, angle(self.start["forward"], current["forward"]))
            if self.previous["bed"] == 1 and current["bed"] == 0:
                self.exits += 1
            elif self.previous["bed"] == 0 and current["bed"] == 1:
                self.entries += 1
            self.recent.append(current)
            # Retain one sample immediately before the two-second boundary.
            while len(self.recent) > 1 and self.recent[1]["elapsed_ms"] <= current["elapsed_ms"] - 2000:
                self.recent.popleft()
            if len(self.recent) > 4096:
                self.reset()
                return None
            recent = self.recent[0]
            recent_enough = current["elapsed_ms"] - recent["elapsed_ms"] >= 2000
            recent_distance = math.dist(recent["position"], current["position"])
            recent_angle = angle(recent["forward"], current["forward"])
            if (current["elapsed_ms"] - self.start["elapsed_ms"] >= MIN_DURATION_MS and
                    self.maximum_distance >= 30.0 and self.maximum_angle >= 15.0 and
                    self.exits >= 1 and self.entries >= 1 and recent_enough and
                    (recent_distance >= 2.0 or recent_angle >= 1.0)):
                result = {"session_id": self.session, "first_frame": self.start["frame"],
                          "last_frame": current["frame"], "frames": self.frames,
                          "first_elapsed_ms": self.start["elapsed_ms"], "last_elapsed_ms": current["elapsed_ms"],
                          "duration_ms": current["elapsed_ms"] - self.start["elapsed_ms"],
                          "maximum_distance_t3": self.maximum_distance, "maximum_rotation_degrees": self.maximum_angle,
                          "bed_exits": self.exits, "bed_entries": self.entries,
                          "recent_distance_t3": recent_distance, "recent_rotation_degrees": recent_angle,
                          "map_epoch": current["epoch"][0], "device_epoch": current["epoch"][1],
                          "continuity": current["epoch"][2],
                          "limitations": ["Viewer diagnostics do not independently prove the map, game input, or GPU pixels.",
                                          "The capture request is made after this observation; a later Present may differ."]}
                self.previous = current
                return result
        self.previous = current
        return None


def replay(path, start, end):
    detector = None
    with path.open("rb") as stream:
        for raw in stream:
            if len(raw) > MAX_LINE or not raw.endswith(b"\n"):
                raise ValueError("incomplete or oversized log line")
            row = diagnostics.strict_json(raw)
            if not isinstance(row, dict):
                raise ValueError("invalid viewer log event")
            if row.get("event") == "viewer_started":
                if detector is not None:
                    raise ValueError("multiple viewer sessions")
                detector = MovementWindow(row.get("session_id"))
            elif row.get("event") == "frame" and (start is None or row.get("frame", -1) >= start) and (
                    end is None or row.get("frame", -1) <= end):
                if detector is None:
                    raise ValueError("frame precedes viewer start")
                result = detector.feed(row)
                if result is not None:
                    return result
    if detector is None:
        raise ValueError("viewer start missing")
    return None


class Tail:
    def __init__(self, path):
        self.path = path
        with path.open("rb") as stream:
            first = stream.readline(MAX_LINE + 1)
            if not first.endswith(b"\n") or len(first) > MAX_LINE:
                raise ValueError("viewer start is incomplete")
            row = diagnostics.strict_json(first)
            if not isinstance(row, dict):
                raise ValueError("invalid viewer start")
            session = row.get("session_id") if row.get("event") == "viewer_started" else None
            self.detector = MovementWindow(session)
            if path.name != "scene-live-" + session.replace(":", "-") + ".jsonl":
                raise ValueError("viewer filename and session disagree")
            self.identity = (path.stat().st_dev, path.stat().st_ino)
            stream.seek(0, 2)
            self.offset = stream.tell()
            self.discard_partial = False
            if self.offset:
                stream.seek(self.offset - 1)
                self.discard_partial = stream.read(1) != b"\n"
        self.last_seen = None
        self.motion_started = None

    def reset(self):
        self.detector.reset()
        self.motion_started = None

    def read_new(self, now):
        info = self.path.stat()
        if (info.st_dev, info.st_ino) != self.identity or info.st_size < self.offset:
            raise ValueError("viewer log replaced or truncated")
        events = []
        with self.path.open("rb") as stream:
            stream.seek(self.offset)
            while True:
                start = stream.tell()
                raw = stream.readline(MAX_LINE + 1)
                if not raw:
                    break
                if len(raw) > MAX_LINE:
                    raise ValueError("viewer log line too long")
                if not raw.endswith(b"\n"):
                    stream.seek(start)
                    break
                self.offset = stream.tell()
                if self.discard_partial:
                    self.discard_partial = False
                    continue
                row = diagnostics.strict_json(raw)
                if not isinstance(row, dict):
                    raise ValueError("invalid viewer log event")
                if row.get("event") == "frame":
                    self.last_seen = now
                    events.append(row)
        return events


def watch(directory, timeout, start_motion=False):
    deadline = time.monotonic() + timeout
    tails = {}
    while time.monotonic() < deadline:
        if directory.is_dir():
            if any((directory / name).exists() for name in ("capture.request", "capture.ready", "batch.json")):
                raise ValueError("capture was already requested or the runner finished")
            now, updates = time.monotonic(), {}
            paths = sorted(directory.glob("scene-live-live-*.jsonl"))
            if len(paths) > 16:
                raise ValueError("too many viewer logs")
            for path in paths:
                if path not in tails:
                    try:
                        tails[path] = Tail(path)
                    except (OSError, ValueError):
                        continue  # The viewer may still be writing its first line.
                    continue  # Existing bytes never trigger a live request.
                updates[path] = tails[path].read_new(now)
            active = [path for path in paths if path in tails and tails[path].last_seen is not None and
                      now - tails[path].last_seen <= FRESH_SECONDS and
                      0 <= time.time() - path.stat().st_mtime <= FRESH_SECONDS]
            if len(active) != 1:
                for item in tails.values():
                    item.reset()
            else:
                active_path = active[0]
                for path, item in tails.items():
                    if path != active_path:
                        item.reset()
                active_tail = tails[active_path]
                result = None
                for row in updates.get(active_path, []):
                    # A trigger from an earlier buffered frame cannot survive a
                    # later reset or stopped-motion frame in the same batch.
                    previous_start = active_tail.detector.start
                    result = active_tail.detector.feed(row)
                    if active_tail.detector.start is not previous_start:
                        active_tail.motion_started = time.monotonic() if active_tail.detector.start is not None else None
                host_duration = (time.monotonic() - active_tail.motion_started
                                 if active_tail.motion_started is not None else 0)
                if start_motion and active_tail.detector.start and active_tail.detector.previous and active_tail.detector.recent:
                    detector = active_tail.detector
                    first, current, recent = detector.start, detector.previous, detector.recent[0]
                    recent_distance = math.dist(recent["position"], current["position"])
                    recent_angle = angle(recent["forward"], current["forward"])
                    if (current["elapsed_ms"] - first["elapsed_ms"] >= 2000 and
                            current["elapsed_ms"] - recent["elapsed_ms"] >= 2000 and
                            (recent_distance >= 2.0 or recent_angle >= 1.0)):
                        result = {"session_id": detector.session, "first_frame": first["frame"],
                                  "last_frame": current["frame"], "first_elapsed_ms": first["elapsed_ms"],
                                  "last_elapsed_ms": current["elapsed_ms"],
                                  "recent_distance_t3": recent_distance, "recent_rotation_degrees": recent_angle,
                                  "map_epoch": current["epoch"][0], "device_epoch": current["epoch"][1],
                                  "continuity": current["epoch"][2],
                                  "limitations": ["This starts sampling; it does not establish the full M1 movement or pixel alignment."]}
                    else:
                        result = None
                required_host_seconds = 2 if start_motion else MIN_DURATION_MS / 1000
                if (result is not None and host_duration >= required_host_seconds and
                        0 <= time.time() - active_path.stat().st_mtime <= FRESH_SECONDS):
                    if any((directory / name).exists() for name in ("capture.ready", "batch.json")):
                        raise ValueError("runner finished before capture request")
                    # The runner separately validates the current TDS PID
                    # and process start before creating capture.ready.
                    result["observed_host_duration_ms"] = round(host_duration * 1000)
                    with (directory / "capture.request").open("x", encoding="utf-8") as marker:
                        json.dump(result, marker, indent=2, allow_nan=False)
                        marker.write("\n")
                    return result
        time.sleep(.2)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--watch", type=Path, help="new runner output directory (may not exist yet)")
    mode.add_argument("--replay", type=Path, help="read a saved viewer log without writing a marker")
    parser.add_argument("--timeout", type=float, default=360, help="watch timeout in seconds")
    parser.add_argument("--start-frame", type=int, help="replay only: inclusive frame bound")
    parser.add_argument("--end-frame", type=int, help="replay only: inclusive frame bound")
    parser.add_argument("--start-motion", action="store_true", help="watch only: request sampling near the start of movement")
    args = parser.parse_args()
    try:
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            raise ValueError("invalid timeout")
        if args.watch is not None and (args.start_frame is not None or args.end_frame is not None):
            raise ValueError("frame bounds apply only to replay")
        if args.replay is not None and args.start_motion:
            raise ValueError("start-motion applies only to watch")
        if any(x is not None and x < 0 for x in (args.start_frame, args.end_frame)) or (
                args.start_frame is not None and args.end_frame is not None and args.start_frame > args.end_frame):
            raise ValueError("invalid replay frame bounds")
        result = watch(args.watch, args.timeout, args.start_motion) if args.watch is not None else replay(args.replay, args.start_frame, args.end_frame)
        print(json.dumps({"status": "requested" if args.watch is not None and result else "would_request" if result else "not_requested",
                          "observation": result}, indent=2, allow_nan=False))
    except (OSError, ValueError, TypeError, KeyError, OverflowError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0 if result is not None else 2


if __name__ == "__main__":
    raise SystemExit(main())
