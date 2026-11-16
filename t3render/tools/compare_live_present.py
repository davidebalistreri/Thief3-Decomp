#!/usr/bin/env python3
"""Compare a finalized trace with one live viewer log using their shared Present link."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys

import trace_summary


def _fingerprint(stat):
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns


def _number_bits(value):
    try:
        if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value):
            return None
        raw = struct.unpack("<I", struct.pack("<f", value))[0]
        if (raw & 0x7f800000) == 0x7f800000:
            return None
        return 0 if raw == 0x80000000 else raw
    except (OverflowError, struct.error):
        return None


def _float_bits(values):
    if not isinstance(values, list) or len(values) != 16:
        return None
    result = [_number_bits(value) for value in values]
    if any(value is None for value in result):
        return None
    return result


def _camera_matches(viewer, observation):
    camera = observation.get("current_camera")
    if (not isinstance(camera, dict) or not camera.get("address") or camera.get("stable") is not True or
            not observation.get("self") or
            observation.get("known_caller") is not True or observation.get("caller") != 0x109271ae or
            observation.get("pass_stable") is not True):
        return False
    for displayed, captured in (("view", "derived_matrix_bits"), ("projection", "projection_bits")):
        raw = camera.get(captured)
        if (not isinstance(raw, list) or len(raw) != 16 or
                any((v & 0x7f800000) == 0x7f800000 for v in raw)):
            return False
        raw = [0 if v == 0x80000000 else v for v in raw]
        if _float_bits(viewer.get(displayed)) != raw:
            return False
    return True


def _viewport_matches(viewer, draw):
    left, right = viewer.get("viewport"), draw.get("viewport")
    transforms = draw.get("transforms")
    if not isinstance(left, dict) or not isinstance(right, dict) or not isinstance(transforms, dict):
        return False
    for key in ("x", "y", "width", "height"):
        if not trace_summary._uint(left.get(key)) or not trace_summary._uint(right.get(key)) or left[key] != right[key]:
            return False
    if not left["width"] or not left["height"]:
        return False
    for left_key, right_key in (("min_depth", "min_z"), ("max_depth", "max_z")):
        a, b = _number_bits(left.get(left_key)), _number_bits(right.get(right_key))
        if a is None or a != b:
            return False
    if not 0 <= left["min_depth"] <= left["max_depth"] <= 1:
        return False
    for key in ("view", "projection"):
        values = _float_bits(viewer.get(key))
        if values is None or values != _float_bits(transforms.get(key)):
            return False
    return True


def compare(trace, viewer_log):
    trace, viewer_log = Path(trace), Path(viewer_log)
    before = trace.stat()
    summary = trace_summary.summarize(trace)
    session = summary.get("live_session_id")
    if not session:
        raise ValueError("trace has no live session identity")
    bridge, observations, draws, results, digest = {}, {}, {}, {}, hashlib.sha256()
    with trace.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        for raw in stream:
            digest.update(raw)
            item = json.loads(raw, object_pairs_hook=trace_summary._object, parse_constant=trace_summary._constant)
            if item.get("event") == "scene_bridge_frame":
                bridge[item["frame"]] = item
            elif item.get("event") == "scene_bridge_observation":
                observations.setdefault(item["frame"], []).append(item)
            elif item.get("event") == "draw":
                draws.setdefault(item["frame"], []).append({k: item.get(k) for k in
                             ("op_id", "thread_id", "viewport", "transforms")})
            elif item.get("event") == "draw_result":
                results[(item["frame"], item["op_id"])] = item["hr"]
        finished = os.fstat(stream.fileno())
    if len({_fingerprint(s) for s in (before, opened, finished, trace.stat())}) != 1:
        raise ValueError("trace changed while reading")

    log_before = viewer_log.stat()
    if log_before.st_size > 8 * 1024 * 1024:
        raise ValueError("viewer log exceeds the diagnostic limit")
    with viewer_log.open("rb") as stream:
        log_opened = os.fstat(stream.fileno())
        raw_log = stream.read(8 * 1024 * 1024 + 1)
        log_finished = os.fstat(stream.fileno())
    if len({_fingerprint(s) for s in (log_before, log_opened, log_finished, viewer_log.stat())}) != 1:
        raise ValueError("viewer log changed while reading")
    lines = raw_log.splitlines()
    if not lines or len(lines) > 4096 or not raw_log.endswith(b"\n"):
        raise ValueError("viewer log is empty, incomplete or too long")
    records = [json.loads(line, object_pairs_hook=trace_summary._object,
                          parse_constant=trace_summary._constant) for line in lines]
    if any(not isinstance(record, dict) for record in records):
        raise ValueError("invalid viewer event")
    starts = [record for record in records if record.get("event") == "viewer_started"]
    if len(starts) != 1 or starts[0].get("session_id") != session:
        raise ValueError("trace and viewer session identities differ or are ambiguous")
    frames = {}
    for record in records:
        if record.get("event") != "frame":
            continue
        frame_id = record.get("frame")
        if not trace_summary._uint(frame_id, 64) or frame_id in frames:
            raise ValueError("invalid or duplicate viewer frame")
        frames[frame_id] = record

    linked_ids = [f["scene_live_frame"] for f in summary["frames"] if "scene_live_frame" in f]
    if len(set(linked_ids)) != len(linked_ids):
        raise ValueError("ambiguous live Present link")
    matches = []
    for frame in summary["frames"]:
        live_id = frame.get("scene_live_frame")
        displayed = frames.get(live_id)
        window = bridge.get(frame["frame"], {})
        if (live_id is None or displayed is None or displayed.get("source") != "live_process" or
                type(displayed.get("render_pass")) is not int or displayed["render_pass"] != 0 or
                type(displayed.get("stage")) is not int or displayed["stage"] != 1 or
                window.get("status") != "synchronized"):
            continue
        relevant = [o for o in observations.get(frame["frame"], [])
                    if o.get("pass") == 0 and o.get("thread_id") == window.get("present_thread")]
        if not relevant or not all(_camera_matches(displayed, o) for o in relevant):
            continue
        observed_draws = draws.get(frame["frame"], [])
        if not any(draw.get("thread_id") == window.get("present_thread") and
                   (frame["frame"], draw["op_id"]) in results and
                   results[(frame["frame"], draw["op_id"])] & 0x80000000 == 0 and
                   _viewport_matches(displayed, draw) for draw in observed_draws):
            continue
        matches.append({"trace_frame": frame["frame"], "live_frame": live_id,
                        "render_pass": 0, "stage": 1, "camera": "matched_float32_zero_normalized",
                        "viewport": "matched_same_frame_draw"})
    return {"status": "matched_present_and_camera" if matches else "no_verified_match",
            "trace_sha256": digest.hexdigest(), "session_id": session, "matches": matches,
            "limitations": ["This checks recorded Present linkage and camera, not GPU inputs,"
                            " complete hook coverage, semantic binding or pixel parity."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("viewer_log", type=Path)
    args = parser.parse_args(argv)
    try:
        print(json.dumps(compare(args.trace, args.viewer_log), indent=2, allow_nan=False))
    except (OSError, ValueError, TypeError, trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
