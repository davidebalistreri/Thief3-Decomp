#!/usr/bin/env python3
"""Validate and summarize a T3Render diagnostic JSONL trace.

This format records diagnostics, not a replayable capture. Unknown events are
counted but deliberately not interpreted.
"""

import argparse
from collections import Counter
import json
import math
from pathlib import Path
import sys


class TraceError(ValueError):
    """The input is not a complete, well-formed diagnostic trace."""


def _is_int(value):
    # bool is a subclass of int in Python but is not a JSON integer.
    return isinstance(value, int) and not isinstance(value, bool)


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON member {key!r}")
        result[key] = value
    return result


def _constant(value):
    raise ValueError(f"invalid JSON constant {value}")


def summarize(path):
    """Read a trace incrementally; raise TraceError on invalid input."""
    events = Counter()
    calls = Counter()
    frames = []
    current = None
    header_seen = False
    capture_ended = False
    operation_results = False
    clear_rect_limit = None
    lines = 0

    try:
        with Path(path).open(encoding="utf-8") as stream:
            for lines, raw in enumerate(stream, 1):
                try:
                    item = json.loads(raw, object_pairs_hook=_object, parse_constant=_constant)
                except ValueError as exc:
                    raise TraceError(f"line {lines}: invalid JSON: {exc}") from exc
                if not isinstance(item, dict):
                    raise TraceError(f"line {lines}: expected a JSON object")
                event = item.get("event")
                if not isinstance(event, str) or not event:
                    raise TraceError(f"line {lines}: event must be a non-empty string")
                if capture_ended:
                    raise TraceError(f"line {lines}: event after capture_end")

                if not header_seen:
                    if event != "header":
                        raise TraceError(f"line {lines}: first event must be header")
                    if item.get("format") != "t3render-d3d8-diagnostic":
                        raise TraceError(f"line {lines}: unsupported trace format")
                    if not _is_int(item.get("version")) or item["version"] != 1:
                        raise TraceError(f"line {lines}: unsupported trace version")
                    if "operation_results" in item:
                        if not _is_int(item["operation_results"]) or item["operation_results"] != 1:
                            raise TraceError(f"line {lines}: unsupported operation result capability")
                        clear_rect_limit = item.get("clear_rect_limit")
                        if not _is_int(clear_rect_limit) or clear_rect_limit <= 0:
                            raise TraceError(f"line {lines}: clear_rect_limit must be a positive integer")
                        operation_results = True
                    header_seen = True
                elif event == "header":
                    raise TraceError(f"line {lines}: duplicate header")
                elif event == "capture_end":
                    if current is not None:
                        raise TraceError(f"line {lines}: capture_end inside a frame")
                    frame_count = item.get("frames")
                    if not _is_int(frame_count):
                        raise TraceError(f"line {lines}: capture_end frames must be an integer")
                    if frame_count != len(frames):
                        raise TraceError(
                            f"line {lines}: capture_end frames {frame_count} does not match {len(frames)} complete frames"
                        )
                    capture_ended = True
                elif event == "frame_begin":
                    frame = item.get("frame")
                    if not _is_int(frame):
                        raise TraceError(f"line {lines}: frame must be an integer")
                    if current is not None:
                        raise TraceError(f"line {lines}: frame_begin before previous frame_end")
                    current = {"frame": frame, "draws": 0, "calls": Counter(), "pending": {},
                               "last_op": 0, "overlap": False}
                elif event in ("draw", "draw_result", "clear", "clear_result", "frame_end"):
                    if current is None:
                        raise TraceError(f"line {lines}: {event} outside a frame")
                    frame = item.get("frame")
                    if not _is_int(frame):
                        raise TraceError(f"line {lines}: frame must be an integer")
                    if frame != current["frame"]:
                        raise TraceError(
                            f"line {lines}: frame {frame} does not match active frame {current['frame']}"
                        )
                    if event in ("draw", "clear"):
                        has_buffer_data = event == "draw" and "buffer_data" in item
                        if has_buffer_data and not isinstance(item["buffer_data"], dict):
                            raise TraceError(f"line {lines}: buffer_data must be an object")
                        if has_buffer_data and not operation_results:
                            raise TraceError(f"line {lines}: buffer_data requires operation result capability")
                        if event == "clear":
                            if not operation_results:
                                raise TraceError(f"line {lines}: clear requires operation result capability")
                            count = item.get("rect_count")
                            status = item.get("rects_status")
                            rects = item.get("rects")
                            if not _is_int(count) or not 0 <= count <= 0xffffffff:
                                raise TraceError(f"line {lines}: invalid clear rect_count")
                            if status == "none":
                                if count != 0 or rects != []:
                                    raise TraceError(f"line {lines}: invalid empty clear rectangles")
                            elif status == "captured":
                                if not 0 < count <= clear_rect_limit or not isinstance(rects, list) or len(rects) != count:
                                    raise TraceError(f"line {lines}: invalid captured clear rectangles")
                                for rect in rects:
                                    if not isinstance(rect, dict) or set(rect) != {"x1", "y1", "x2", "y2"} or any(
                                        not _is_int(value) or not -(1 << 31) <= value < (1 << 31)
                                        for value in rect.values()
                                    ):
                                        raise TraceError(f"line {lines}: invalid clear rectangle")
                            elif status in ("inconsistent", "null_pointer", "count_limit", "unreadable", "address_overflow"):
                                if rects is not None:
                                    raise TraceError(f"line {lines}: unknown clear rectangles must be null")
                                if ((status == "inconsistent" and count != 0) or
                                    (status == "null_pointer" and count == 0) or
                                    (status == "count_limit" and count <= clear_rect_limit) or
                                    (status in ("unreadable", "address_overflow") and not 0 < count <= clear_rect_limit)):
                                    raise TraceError(f"line {lines}: clear rectangle status does not match count")
                            else:
                                raise TraceError(f"line {lines}: invalid clear rectangle status")
                            for field in ("flags", "color", "depth_bits", "stencil"):
                                value = item.get(field)
                                if not _is_int(value) or not 0 <= value <= 0xffffffff:
                                    raise TraceError(f"line {lines}: invalid clear {field}")
                            depth = item.get("depth")
                            if depth is not None and (isinstance(depth, bool) or not isinstance(depth, (int, float)) or
                                                      (isinstance(depth, float) and not math.isfinite(depth))):
                                raise TraceError(f"line {lines}: invalid clear depth")
                        if operation_results or event == "clear" or "op_id" in item:
                            op_id = item.get("op_id")
                            if not _is_int(op_id) or not current["last_op"] < op_id < (1 << 64) - 1:
                                raise TraceError(f"line {lines}: operation id must increase within the frame")
                            if current["pending"]:
                                current["overlap"] = True
                            current["last_op"] = op_id
                            current["pending"][op_id] = (event, has_buffer_data)
                    if event == "draw":
                        call = item.get("call")
                        if not isinstance(call, str) or not call:
                            raise TraceError(f"line {lines}: draw call must be a non-empty string")
                        current["draws"] += 1
                        current["calls"][call] += 1
                        calls[call] += 1
                    elif event in ("draw_result", "clear_result"):
                        op_id = item.get("op_id")
                        pending = current["pending"].get(op_id) if _is_int(op_id) else None
                        if pending is None or pending[0] != event.removesuffix("_result"):
                            raise TraceError(f"line {lines}: result has no matching operation")
                        if not _is_int(item.get("hr")) or not -(1 << 31) <= item["hr"] < (1 << 32):
                            raise TraceError(f"line {lines}: operation hr must be an integer")
                        if pending[1]:
                            if not isinstance(item.get("buffer_stable"), bool):
                                raise TraceError(f"line {lines}: buffered draw result requires boolean buffer_stable")
                        elif "buffer_stable" in item:
                            raise TraceError(f"line {lines}: buffer_stable has no matching buffer_data")
                        del current["pending"][op_id]
                    elif event == "frame_end":
                        hr = item.get("present_hr")
                        if not _is_int(hr):
                            raise TraceError(f"line {lines}: present_hr must be an integer")
                        if current["pending"]:
                            raise TraceError(f"line {lines}: frame ends with pending operations")
                        if operation_results and not isinstance(item.get("concurrent_operations"), bool):
                            raise TraceError(f"line {lines}: concurrent_operations must be a boolean")
                        if current["overlap"] and item.get("concurrent_operations") is not True:
                            raise TraceError(f"line {lines}: overlapping operations were not flagged")
                        if "concurrent_operations" in item:
                            current["concurrent_operations"] = item["concurrent_operations"]
                        current["present_hr"] = hr
                        current["calls"] = dict(sorted(current["calls"].items()))
                        del current["pending"]
                        del current["last_op"]
                        del current["overlap"]
                        frames.append(current)
                        current = None
                events[event] += 1
    except (OSError, UnicodeError) as exc:
        raise TraceError(f"cannot read trace: {exc}") from exc

    if not header_seen:
        raise TraceError("empty trace: missing header")
    if current is not None:
        raise TraceError(f"unexpected EOF: frame {current['frame']} has no frame_end")
    if not capture_ended:
        raise TraceError("unexpected EOF: missing capture_end")
    if not frames:
        raise TraceError("trace has no complete frames")
    if not calls and not events["clear"]:
        raise TraceError("trace has no draw events or clear events")

    return {
        "format": "t3render-d3d8-diagnostic",
        "version": 1,
        "lines": lines,
        "frame_count": len(frames),
        "draw_count": sum(calls.values()),
        "calls": dict(sorted(calls.items())),
        "events": dict(sorted(events.items())),
        "frames": frames,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", help="diagnostic JSONL file")
    parser.add_argument("--json", action="store_true", help="write machine-readable summary")
    args = parser.parse_args(argv)
    try:
        summary = summarize(args.trace)
    except TraceError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        print(f"Valid diagnostic trace: {summary['frame_count']} frames, {summary['draw_count']} draws")
        for call, count in summary["calls"].items():
            print(f"  {call}: {count}")
        for frame in summary["frames"]:
            detail = ", ".join(f"{name}={count}" for name, count in frame["calls"].items()) or "no draws"
            print(f"  frame {frame['frame']}: {frame['draws']} draws ({detail}), present_hr={frame['present_hr']}")
        print("Diagnostic data only; replay and visual parity are not verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
