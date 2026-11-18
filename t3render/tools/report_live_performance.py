#!/usr/bin/env python3
"""Summarize D3D8 Present cadence from two diagnostic snapshots, offline."""
import argparse
import json
import math
from pathlib import Path


COUNTERS = ("successful", "failed", "overlaps", "resets", "interruptions",
            "clock_errors", "intervals", "total_ticks", "max_ticks")


def _integer(value):
    return type(value) is int and value >= 0


def _timing(event):
    timing = event.get("present_timing")
    if not isinstance(timing, dict):
        raise ValueError("D3D8 Present timing is unavailable in this log")
    bounds, histogram = timing.get("bounds_ms"), timing.get("histogram")
    if (not _integer(timing.get("clock_hz")) or not timing["clock_hz"] or
            any(not _integer(timing.get(name)) for name in COUNTERS) or
            not isinstance(bounds, list) or not bounds or
            any(not _integer(x) or x == 0 for x in bounds) or
            bounds != sorted(set(bounds)) or not isinstance(histogram, list) or
            len(histogram) != len(bounds) + 1 or any(not _integer(x) for x in histogram) or
            sum(histogram) != timing["intervals"]):
        raise ValueError("invalid D3D8 Present timing snapshot")
    return timing


def _bucket_interval(histogram, bounds, rank):
    count = 0
    for index, value in enumerate(histogram):
        count += value
        if count >= rank:
            return [bounds[index - 1] if index else 0,
                    bounds[index] if index < len(bounds) else None]
    raise ValueError("invalid histogram rank")


def report(events, start_ms=None, end_ms=None):
    if start_ms is not None and end_ms is not None and start_ms > end_ms:
        raise ValueError("start must not exceed end")
    sessions = [e.get("session_id") for e in events if e.get("event") == "viewer_started"]
    if len(sessions) != 1 or not isinstance(sessions[0], str) or not sessions[0]:
        raise ValueError("expected one viewer session")
    drivers = [e for e in events if e.get("event") == "driver"]
    if any(not _integer(e.get("elapsed_ms")) for e in drivers):
        raise ValueError("invalid driver snapshot time")
    selected = [e for e in drivers if
                (start_ms is None or e["elapsed_ms"] >= start_ms) and
                (end_ms is None or e["elapsed_ms"] <= end_ms)]
    if len(selected) < 2:
        raise ValueError("at least two driver snapshots are required in the selected window")
    if any(b["elapsed_ms"] <= a["elapsed_ms"] for a, b in zip(selected, selected[1:])):
        raise ValueError("driver snapshots are not ordered")
    snapshots = [_timing(e) for e in selected]
    first, last = snapshots[0], snapshots[-1]
    for a, b in zip(snapshots, snapshots[1:]):
        if (a["clock_hz"] != b["clock_hz"] or a["bounds_ms"] != b["bounds_ms"] or
                any(a[name] > b[name] for name in COUNTERS) or
                any(x > y for x, y in zip(a["histogram"], b["histogram"]))):
            raise ValueError("D3D8 Present timing counters changed or went backwards")
    delta = {name: last[name] - first[name] for name in COUNTERS if name != "max_ticks"}
    histogram = [b - a for a, b in zip(first["histogram"], last["histogram"])]
    count, ticks, hz = delta["intervals"], delta["total_ticks"], last["clock_hz"]
    if sum(histogram) != count or count > delta["successful"] or (count and not ticks) or (not count and ticks):
        raise ValueError("inconsistent D3D8 Present timing counters")
    result = {"session_id": sessions[0], "window_start_ms": selected[0]["elapsed_ms"],
              "window_end_ms": selected[-1]["elapsed_ms"], **delta,
              "measured_interval_seconds": ticks / hz,
              "telemetry_window_seconds": (selected[-1]["elapsed_ms"] - selected[0]["elapsed_ms"]) / 1000,
              "fps": count * hz / ticks if count else None,
              "mean_frametime_ms": ticks * 1000 / (hz * count) if count else None,
              "median_frametime_ms_bucket": None, "p95_frametime_ms_bucket": None,
              "max_frametime_ms_bucket": None,
              "limitations": ["Present entry intervals include proxy and diagnostic overhead; they are not GPU timings.",
                              "Intervals across failures, overlapping calls, resets, clock errors or thread changes are excluded.",
                              "Telemetry snapshots and Present entry times are not sampled at exactly the same instant.",
                              "Bucket ranges are lower-exclusive and upper-inclusive; null upper bound means over the last bound.",
                              "The window does not establish the map, player movement or D3D8/D3D12 synchronization."]}
    if count:
        bounds = last["bounds_ms"]
        result["median_frametime_ms_bucket"] = _bucket_interval(histogram, bounds, (count + 1) // 2)
        result["p95_frametime_ms_bucket"] = _bucket_interval(histogram, bounds, math.ceil(count * 0.95))
        result["max_frametime_ms_bucket"] = _bucket_interval(histogram, bounds, count)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--start-ms", type=int)
    parser.add_argument("--end-ms", type=int)
    args = parser.parse_args()
    try:
        if ((args.start_ms is not None and args.start_ms < 0) or
                (args.end_ms is not None and args.end_ms < 0)):
            raise ValueError("window times must be non-negative")
        events = []
        with args.log.open(encoding="utf-8") as stream:
            for line in stream:
                if not line.strip():
                    continue
                event = json.loads(line)
                if not isinstance(event, dict):
                    raise ValueError("diagnostic event must be an object")
                if event.get("event") in ("viewer_started", "driver"):
                    events.append(event)
        print(json.dumps(report(events, args.start_ms, args.end_ms), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(2, f"error: {error}\n")


if __name__ == "__main__":
    main()
