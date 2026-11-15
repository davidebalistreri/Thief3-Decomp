#!/usr/bin/env python3
"""Extract one observed draw camera for an offline diagnostic SceneIR view.

This does not attach the camera to the offline scene or establish that its
objects belong to the captured runtime frame.
"""
import argparse
import json
from pathlib import Path
import sys

import analyze_scene_bridge as trace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ir"))
from scene_ir import Camera  # noqa: E402


def extract(path, frame, draw_id):
    records, digest = trace.read_selected(Path(path), frame, draw_id)
    draw = trace.one(records, "draw", draw_id)
    result = trace.one(records, "draw_result", draw_id)
    end = trace.one(records, "frame_end")
    if (not isinstance(draw, dict) or not isinstance(result, dict) or not isinstance(end, dict) or
            trace.hresult_succeeded(result.get("hr")) is not True or
            trace.hresult_succeeded(end.get("present_hr")) is not True or
            end.get("had_reset") is not False or end.get("concurrent_operations") is not False):
        raise ValueError("draw and Present must succeed in a frame without recorded reset or overlap")
    transform = draw.get("transforms")
    viewport = draw.get("viewport")
    if not isinstance(transform, dict) or not isinstance(viewport, dict):
        raise ValueError("draw camera or viewport is missing")
    camera = {"source": {"kind": "runtime", "frame_id": frame, "fingerprint": digest},
              "view": transform.get("view"), "projection": transform.get("projection"),
              "viewport": {"x": viewport.get("x"), "y": viewport.get("y"),
                           "width": viewport.get("width"), "height": viewport.get("height"),
                           "min_depth": viewport.get("min_z"), "max_depth": viewport.get("max_z")},
              "near": None, "far": None}
    Camera.parse(camera, "camera")
    if camera["view"] is None or camera["projection"] is None:
        raise ValueError("draw view and projection must be available")
    return {"schema": "t3.camera.observation", "version": 1,
            "source": {"kind": "captured_draw", "frame_id": frame, "operation_id": draw_id,
                       "trace_sha256": digest}, "camera": camera}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--frame", required=True, type=int)
    parser.add_argument("--draw", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if not trace.uint(args.frame, 64) or not trace.uint(args.draw, 64, nonzero=True):
            raise ValueError("frame and draw IDs must be unsigned; draw must be nonzero")
        observation = extract(args.trace, args.frame, args.draw)
        with args.output.open("x", encoding="utf-8") as output:
            json.dump(observation, output, indent=2, allow_nan=False)
            output.write("\n")
        print(json.dumps(observation["source"], allow_nan=False))
    except (OSError, ValueError, TypeError, trace.trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
