#!/usr/bin/env python3
"""Verify the local frame-3377 oracle against its committed manifests.

This only reads local captures. It does not copy or publish game data and is
not a Scene IR fixture or a claim about GPU inputs or frame parity.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys

import trace_summary

ROOT = Path(__file__).resolve().parents[2]
FRAME = 3377
MATCHED_DRAWS = (5, 6, 8, 9, 20, 21, 45, 46)


def sha256(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def frame_evidence(records):
    rows = [r for r in records if r.get("frame") == FRAME]
    draws = {r.get("op_id"): r for r in rows if r.get("event") == "draw"}
    results = {r.get("op_id"): r for r in rows if r.get("event") == "draw_result"}
    images = [r for r in rows if r.get("event") == "frame_image"]
    ends = [r for r in rows if r.get("event") == "frame_end"]
    if (len(images) != 1 or images[0].get("status") != "captured" or
            images[0].get("stage") != "complete" or images[0].get("hr") != 0 or
            (images[0].get("width"), images[0].get("height")) != (1024, 768) or
            len(ends) != 1 or ends[0].get("present_hr") != 0 or
            ends[0].get("had_reset") is not False or ends[0].get("concurrent_operations") is not False):
        raise ValueError("golden frame image or frame outcome is incomplete")
    if any(op not in draws or results.get(op, {}).get("hr") != 0 or
           results.get(op, {}).get("buffer_stable") is not True for op in MATCHED_DRAWS):
        raise ValueError("a correlated draw is absent, failed or unstable")
    camera = draws[8].get("transforms") or {}
    reference = (camera.get("view"), camera.get("projection"), draws[8].get("viewport"))
    if any(part is None for part in reference):
        raise ValueError("camera observation is unavailable")
    for op in MATCHED_DRAWS:
        draw = draws[op]
        transforms = draw.get("transforms") or {}
        if (transforms.get("view"), transforms.get("projection"), draw.get("viewport")) != reference:
            raise ValueError("correlated draw camera observations differ")
        buffers = draw.get("buffer_data") or {}
        if buffers.get("source") != "application_lock_shadow" or any(
                (buffers.get(kind) or {}).get("status") != "captured" for kind in ("vertex", "index")):
            raise ValueError("correlated draw buffer shadows are missing")
    return {"frame": FRAME, "camera_reference_op": 8, "correlated_draws": list(MATCHED_DRAWS),
            "image_timing": "before_present", "present_hr": 0}


def verify(root):
    runtime = json.loads((root / "t3render/benchmarks/runtime/2026-11-14-tds-provenance-screen/result.json").read_text())
    replay = json.loads((root / "t3render/benchmarks/d3d12/2026-11-14-tds-bedroom-reconstruction/result.json").read_text())
    capture = runtime["capture"]
    if (replay["frame"] != FRAME or FRAME not in capture["frames"] or
            capture["raw_trace_sha256"] != replay["trace_sha256"] or
            capture["frame_3377_backbuffer"]["sha256"] != replay["d3d8_pre_present_ppm_sha256"]):
        raise ValueError("golden manifests disagree")
    entries = (
        ("trace", "build/t3render-captures/bedroom-provenance-20261114-001/bedroom.jsonl", replay["trace_sha256"]),
        ("d3d8_image", "build/t3render-captures/bedroom-provenance-20261114-001/bedroom.jsonl.frame-3377.ppm",
         replay["d3d8_pre_present_ppm_sha256"]),
        ("dx12_packet", "build/t3render-reconstruction/bedroom-3377-cull.txt", replay["packet_sha256"]),
        ("dx12_image", "build/t3render-reconstruction/bedroom-3377-cull.ppm", replay["dx12_ppm_sha256"]),
        ("comparison", "build/t3render-reconstruction/comparison-3377-cull.png", replay["comparison_png_sha256"]),
    )
    for name, relative, expected in entries:
        path = root / relative
        if not path.is_file():
            raise ValueError(f"golden {name} is missing: {relative}")
        if sha256(path) != expected:
            raise ValueError(f"golden {name} hash differs: {relative}")
    trace = root / entries[0][1]
    trace_summary.summarize(trace)
    with trace.open(encoding="utf-8") as source:
        evidence = frame_evidence(json.loads(line) for line in source)
    if sha256(trace) != replay["trace_sha256"]:
        raise ValueError("golden trace changed during validation")
    return {"status": "PASS", "artifacts_verified": [entry[0] for entry in entries], **evidence,
            "limitations": ["Local game-derived files remain outside Git.",
                            "Matching camera state and lock shadows do not establish GPU inputs or runtime actor identity."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help="t3sdk root")
    args = parser.parse_args(argv)
    try:
        print(json.dumps(verify(args.root), indent=2))
    except (OSError, ValueError, KeyError, TypeError, trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
