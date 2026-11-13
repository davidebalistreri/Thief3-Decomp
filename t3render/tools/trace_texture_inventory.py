#!/usr/bin/env python3
"""Inventory observed D3D8 texture types in a validated diagnostic trace.

Address counts are process-local observations, not stable asset identities.
This does not capture pixels, prove GPU inputs, or establish COM alias coverage.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
import sys

import trace_summary
import trace_to_ir

KINDS = {3: "texture_2d", 4: "volume_texture", 5: "cube_texture"}
ADDRESS = re.compile(r"0x[0-9a-fA-F]{1,16}\Z")


def inventory(path):
    ir = trace_to_ir.convert(path)  # validates and parses the same immutable trace bytes
    frames = []
    for frame in ir["frames"]:
        events = frame["compatibility"]["d3d8"]["events"]
        results = {item["event"]["op_id"]: item["event"]["hr"] for item in events
                   if item["event"].get("event") == "draw_result"}
        kinds = Counter()
        addresses = defaultdict(set)
        draws_with_kind = defaultdict(set)
        stages = defaultdict(Counter)
        statuses = Counter()
        outcome = Counter()
        malformed_draws = 0
        end = frame["compatibility"]["d3d8"]["frame_end"]
        for command in frame["commands"]:
            draw = command["compatibility"]["d3d8"]["draw"]
            success = trace_to_ir.hresult_succeeded(results.get(draw.get("op_id")))
            if success is not True:
                outcome["failed" if success is False else "unknown"] += 1
                continue
            outcome["succeeded"] += 1
            entries = draw.get("stages")
            if not isinstance(entries, list) or not entries:
                malformed_draws += 1
                continue
            indexes = [entry.get("stage") if isinstance(entry, dict) else None for entry in entries]
            if (len(indexes) > 8 or any(type(index) is not int or not 0 <= index < 8 for index in indexes) or
                    len(set(indexes)) != len(indexes)):
                malformed_draws += 1
                continue
            for stage in entries:
                index = stage["stage"]
                if "texture_hr" in stage and trace_to_ir.hresult_succeeded(stage["texture_hr"]) is not True:
                    statuses["getter_failed_or_invalid"] += 1
                    continue
                if "texture" not in stage:
                    statuses["unknown"] += 1
                    continue
                texture = stage["texture"]
                if texture is None:
                    statuses["unbound"] += 1
                    continue
                if not isinstance(texture, dict):
                    statuses["unknown"] += 1
                    continue
                kind = KINDS.get(texture.get("type")) if type(texture.get("type")) is int else None
                address = texture.get("id")
                if not kind or not isinstance(address, str) or not ADDRESS.fullmatch(address) or not int(address, 16):
                    statuses["unknown"] += 1
                    continue
                address = address.lower()
                statuses["bound"] += 1
                kinds[kind] += 1
                addresses[kind].add(address)
                draws_with_kind[kind].add(command["id"])
                stages[kind][index] += 1
        frames.append({"frame": frame["frame"],
                       "frame_outcome": {"present_succeeded": trace_to_ir.hresult_succeeded(end.get("present_hr")),
                                         "concurrent_operations": end.get("concurrent_operations")
                                         if type(end.get("concurrent_operations")) is bool else None,
                                         "had_reset": end.get("had_reset") if type(end.get("had_reset")) is bool else None},
                       "draw_outcomes": dict(sorted(outcome.items())),
                       "draws_with_missing_or_invalid_stages": malformed_draws,
                       "stage_observations": dict(sorted(statuses.items())),
                       "texture_types": {kind: {"bound_stage_observations": kinds[kind],
                                                "draws_with_type": len(draws_with_kind[kind]),
                                                "distinct_observed_addresses": len(addresses[kind]),
                                                "by_stage": {str(n): count for n, count in sorted(stages[kind].items())}}
                                         for kind in sorted(kinds)}})
    return {"schema": "t3render.texture-inventory", "version": 1, "source_sha256": ir["source"]["sha256"],
            "frames": frames, "limitations": [
                "Observations follow successful draw results; snapshots are not proof of GPU input.",
                "Addresses are process-local, may be reused or aliased, and are not asset identities.",
                "Texture bytes and asset names are absent; alias paths and unobserved draws remain unknown."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", help="complete D3D8 diagnostic JSONL trace")
    parser.add_argument("-o", "--output", help="new JSON file; default is standard output")
    args = parser.parse_args(argv)
    try:
        encoded = json.dumps(inventory(args.trace), ensure_ascii=False, indent=2, allow_nan=False) + "\n"
        if args.output:
            with Path(args.output).open("x", encoding="utf-8") as output:
                output.write(encoded)
        else:
            sys.stdout.write(encoded)
    except (OSError, UnicodeError, ValueError, trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
