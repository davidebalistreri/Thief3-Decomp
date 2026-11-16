#!/usr/bin/env python3
"""Build independently verified SceneIR snapshots from one finalized capture.

All frames must have the bed and BSP proof; this is an offline processor, not
a live bridge. The output directory must not already exist.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

import analyze_scene_bridge as bridge
import build_bridge_runtime_snapshot as reader
import build_scoped_scene_snapshot as scoped
import scene_ir_to_d3d12_packet as packet
import scene_trace_session as session
import scene_self_attached_candidates as self_attached
from scene_ir import SceneSnapshot


def build(trace, scene_path, render_pass, stage, *, include_asset_candidates=False,
          allow_quantized_rotators=False, offline_actors=None, game_dir=None):
    if not bridge.uint(render_pass) or not bridge.uint(stage):
        raise ValueError("pass and stage must be unsigned")
    if offline_actors is not None and not include_asset_candidates:
        raise ValueError("self-attached actors require offline asset candidates")
    if game_dir is not None and offline_actors is None:
        raise ValueError("game directory requires self-attached candidates")
    trace = Path(trace)
    before = trace.stat()
    summary = bridge.trace_summary.summarize(trace)
    if summary["frame_count"] not in (2, 3):
        raise ValueError("sequence requires a finalized two- or three-frame capture")
    digest = hashlib.sha256()
    with trace.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
        finished = os.fstat(stream.fileno())
    trace_digest = digest.hexdigest()
    raw = packet.read_limited(Path(scene_path), packet.MAX_INPUT)
    scene = SceneSnapshot.from_json(raw)
    scene_digest = hashlib.sha256(raw).hexdigest()
    output = []
    for item in summary["frames"]:
        frame = item["frame"]
        records = reader.read_frame(trace, frame, trace_digest)
        assets = (self_attached.load(scene, records, offline_actors, self_attached.importer.game_dir(game_dir))
                  if offline_actors is not None else None)
        snapshot, manifest = scoped.build(scene, records, frame, render_pass, stage, None, trace_digest, scene_digest,
                                          include_asset_candidates=include_asset_candidates,
                                          allow_quantized_rotators=allow_quantized_rotators,
                                          self_attached_assets=assets)
        body = snapshot.to_json(indent=2) + "\n"
        manifest["snapshot_sha256"] = hashlib.sha256(body.encode()).hexdigest()
        output.append((snapshot, manifest, body))
    for left, right in zip(output, output[1:]):
        session.validate(trace, left[0], right[0], render_pass, stage)
    after = trace.stat()
    identity = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns)
    if len({identity(value) for value in (before, opened, finished, after)}) != 1:
        raise ValueError("trace changed while building sequence")
    manifest = {"schema": "t3.scene.scoped-sequence", "version": 1, "trace_sha256": trace_digest,
                "offline_scene_sha256": scene_digest, "pass": render_pass, "stage": stage,
                "frames": [{"frame": snapshot.source.frame_id, "snapshot_sha256": info["snapshot_sha256"]}
                           for snapshot, info, _ in output],
                "limitations": ["Each frame is a partial scoped selection with asset-backed geometry.",
                                "This is an offline capture processor, not an integrated game loop.",
                                "Runtime light properties and GPU payload bytes are not established."]}
    return output, manifest


def write(output_dir, frames, manifest):
    output_dir = Path(output_dir)
    files = []
    for snapshot, info, body in frames:
        path = output_dir / f"frame-{snapshot.source.frame_id}.scene.json"
        files.extend(((path, body), (Path(str(path) + ".manifest.json"), json.dumps(info, indent=2, allow_nan=False) + "\n")))
    files.append((output_dir / "sequence.manifest.json", json.dumps(manifest, indent=2, allow_nan=False) + "\n"))
    output_dir.mkdir()
    directory = output_dir.stat()
    created = []
    try:
        for path, body in files:
            with path.open("x", encoding="utf-8", newline="\n") as stream:
                stat = os.fstat(stream.fileno())
                created.append((path, stat.st_dev, stat.st_ino))
                stream.write(body)
    except Exception:
        for path, device, inode in reversed(created):
            try:
                stat = path.lstat()
                if (stat.st_dev, stat.st_ino) == (device, inode):
                    path.unlink()
            except OSError:
                pass
        try:
            stat = output_dir.lstat()
            if (stat.st_dev, stat.st_ino) == (directory.st_dev, directory.st_ino):
                output_dir.rmdir()
        except OSError:
            pass
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--scene", required=True, type=Path)
    parser.add_argument("--pass", dest="render_pass", required=True, type=int)
    parser.add_argument("--stage", required=True, type=int)
    parser.add_argument("--include-asset-candidates", action="store_true")
    parser.add_argument("--allow-quantized-rotators", action="store_true")
    parser.add_argument("--offline-actors", type=Path, help="include fingerprinted self-attached asset candidates")
    parser.add_argument("--game-dir")
    parser.add_argument("--output-dir", required=True, type=Path, help="new directory for the verified sequence")
    args = parser.parse_args(argv)
    try:
        frames, manifest = build(args.trace, args.scene, args.render_pass, args.stage,
                                 include_asset_candidates=args.include_asset_candidates,
                                 allow_quantized_rotators=args.allow_quantized_rotators,
                                 offline_actors=args.offline_actors, game_dir=args.game_dir)
        write(args.output_dir, frames, manifest)
        print(json.dumps(manifest, indent=2, allow_nan=False))
    except (OSError, ValueError, TypeError, KeyError, IndexError, StopIteration) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
