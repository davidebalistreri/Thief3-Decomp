#!/usr/bin/env python3
"""Add an independently observed camera to an explicitly offline SceneIR.

The resulting camera retains its own provenance. It does not make the map
export a runtime frame or establish synchronization with its objects.
"""
import argparse
import json
from pathlib import Path
import sys

import scene_ir_to_d3d12_packet as preview


def attach(scene, camera_bytes):
    if scene.source.kind != "offline":
        raise ValueError("camera attachment accepts offline scenes only")
    preview.load_camera(camera_bytes)
    observation = json.loads(camera_bytes, object_pairs_hook=preview._pairs,
                             parse_constant=preview._invalid_number)
    document = scene.to_dict()
    document["camera"] = observation["camera"]
    document["source"]["camera_synchronized"] = False
    before = "Offline map export: no runtime visibility, save-game changes, frame or camera are represented."
    after = "Offline map export: no runtime visibility, save-game changes or runtime frame are represented."
    document["source"]["limitations"] = [after if item == before else item
                                          for item in document["source"]["limitations"]]
    source = observation["source"]
    provenance = (f"Captured camera: frame {source['frame_id']}, draw operation {source['operation_id']}. "
                  if source["kind"] == "captured_draw" else "Synthetic camera. ")
    document["source"]["limitations"].append(
        provenance + "Camera has independent provenance; it is not synchronized to the offline objects.")
    return preview.SceneSnapshot.from_dict(document)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene", type=Path)
    parser.add_argument("--camera", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        scene = preview.SceneSnapshot.from_json(preview.read_limited(args.scene, preview.MAX_INPUT))
        result = attach(scene, preview.read_limited(args.camera, 1024 * 1024))
        with args.output.open("x", encoding="utf-8") as output:
            output.write(result.to_json(indent=2))
            output.write("\n")
        print("source=offline camera_synchronized=false")
    except (OSError, ValueError, TypeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
