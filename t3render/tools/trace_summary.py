#!/usr/bin/env python3
"""Validate and summarize a T3Render diagnostic JSONL trace.

This format records diagnostics, not a replayable capture. Unknown events are
counted but deliberately not interpreted.
"""

import argparse
from collections import Counter
import json
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
                    current = {"frame": frame, "draws": 0, "calls": Counter()}
                elif event in ("draw", "frame_end"):
                    if current is None:
                        raise TraceError(f"line {lines}: {event} outside a frame")
                    frame = item.get("frame")
                    if not _is_int(frame):
                        raise TraceError(f"line {lines}: frame must be an integer")
                    if frame != current["frame"]:
                        raise TraceError(
                            f"line {lines}: frame {frame} does not match active frame {current['frame']}"
                        )
                    if event == "draw":
                        call = item.get("call")
                        if not isinstance(call, str) or not call:
                            raise TraceError(f"line {lines}: draw call must be a non-empty string")
                        current["draws"] += 1
                        current["calls"][call] += 1
                        calls[call] += 1
                    else:
                        hr = item.get("present_hr")
                        if not _is_int(hr):
                            raise TraceError(f"line {lines}: present_hr must be an integer")
                        current["present_hr"] = hr
                        current["calls"] = dict(sorted(current["calls"].items()))
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
    if not calls:
        raise TraceError("trace has no draw events")

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
