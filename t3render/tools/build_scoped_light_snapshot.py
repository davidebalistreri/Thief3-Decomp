#!/usr/bin/env python3
"""Add explicitly uncertain light candidates to a revalidated scoped SceneIR v1."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

import scene_ir_to_d3d12_packet as preview
import scene_legacy_states as states
import scene_light_candidates as lights
from scene_ir import SceneSnapshot
from t3common import game_dir


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--scene", required=True, type=Path)
    parser.add_argument("--offline-scene", required=True, type=Path)
    parser.add_argument("--offline-actors", required=True, type=Path)
    parser.add_argument("--game-dir")
    parser.add_argument("--pass", dest="render_pass", required=True, type=int)
    parser.add_argument("--stage", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        raw = preview.read_limited(args.scene, preview.MAX_INPUT)
        scene = SceneSnapshot.from_json(raw)
        offline_bytes = preview.read_limited(args.offline_scene, preview.MAX_INPUT)
        offline = SceneSnapshot.from_json(offline_bytes)
        game = game_dir(args.game_dir)
        states.from_trace(scene, offline, offline_bytes, args.trace, args.render_pass, args.stage,
                          self_attached_actors=args.offline_actors, self_attached_game=game)
        snapshot, evidence = lights.from_trace(scene, offline, offline_bytes, args.trace, args.offline_actors,
                                               game, args.render_pass, args.stage)
        body = snapshot.to_json(indent=2) + "\n"
        manifest = {"schema": "t3.scene.light-candidate-evidence", "version": 1,
                    "frame": snapshot.source.frame_id, "pass": args.render_pass, "stage": args.stage,
                    "trace_sha256": snapshot.source.fingerprints["trace"],
                    "base_scene_sha256": hashlib.sha256(raw).hexdigest(),
                    "snapshot_sha256": hashlib.sha256(body.encode()).hexdigest(),
                    "candidates": evidence, "limitations": [lights.LIMITATION]}
        preview.write_pair(args.output, body, manifest)
        print(json.dumps(manifest, indent=2, allow_nan=False))
    except (OSError, ValueError, TypeError, KeyError, IndexError, EOFError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
