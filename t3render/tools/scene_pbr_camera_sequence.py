#!/usr/bin/env python3
"""Render a verified captured camera sequence with one resident bed switch.

This consumes finalized captures, not a live TDS connection. Frames whose
geometry, materials, lights or viewport differ are rejected before launch.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys

import scene_ir_to_d3d12_packet as preview
import scene_pbr_variants as variants
import scene_temporal_resolve as temporal
import scene_trace_session as session

HEADER = struct.Struct("<8sIIff32f4f4fIII")
MAX_BUNDLE = 2 * variants.MAX_PACKET + variants.MAX_KEY + 20


def _digest(raw):
    return hashlib.sha256(raw).hexdigest()


def _frame(scene_path, packet_path, manifest_path):
    scene_raw = preview.read_limited(scene_path, preview.MAX_INPUT)
    packet = preview.read_limited(packet_path, variants.MAX_PACKET)
    manifest_raw = preview.read_limited(manifest_path, preview.MAX_INPUT)
    if len(packet) < HEADER.size:
        raise ValueError("PBR packet is truncated")
    header = HEADER.unpack_from(packet)
    if header[0] not in (b"T3PBR01\0", b"T3PBR02\0", b"T3PBR03\0", b"T3PBR04\0", b"T3PBR05\0"):
        raise ValueError("unsupported PBR packet")
    data = {"packet_version": header[0][6]-ord("0"), "digest": _digest(packet),
            "width": header[1], "height": header[2], "min_depth": header[3], "max_depth": header[4],
            "view": header[5:21], "projection": header[21:37], "camera": header[37:40], "scale": header[40], "flags": 0}
    scene, manifest = temporal.validate_runtime(data, scene_raw, manifest_raw)
    return {"scene": scene, "manifest": manifest, "packet": packet, "metadata": data,
            "paths": (scene_path, packet_path, manifest_path),
            "hashes": (_digest(scene_raw), _digest(packet), _digest(manifest_raw))}


def prepare(trace, bundle_path, bundle_manifest_path, paths):
    if len(paths) not in (2, 3):
        raise ValueError("camera sequence requires two or three captured frames")
    frames = [_frame(*map(Path, item)) for item in paths]
    first = frames[0]
    base = first["packet"]
    scope = (first["manifest"]["render_pass"], first["manifest"]["stage"])
    for previous, current in zip(frames, frames[1:]):
        if (len(current["packet"]) != len(base) or current["packet"][:24] != base[:24] or
                current["packet"][164:] != base[164:]):
            raise ValueError("camera sequence changes resident geometry, materials, lights or viewport")
        if (current["manifest"]["render_pass"], current["manifest"]["stage"]) != scope:
            raise ValueError("camera sequence changes submission scope")
        if (current["scene"].source.fingerprints != first["scene"].source.fingerprints or
                temporal._light_source(current["scene"], current["manifest"]) !=
                temporal._light_source(first["scene"], first["manifest"])):
            raise ValueError("camera sequence changes capture or light provenance")
        temporal.compatible(previous["metadata"], current["metadata"],
                            (previous["scene"], previous["manifest"]), (current["scene"], current["manifest"]))
        session.validate(trace, previous["scene"], current["scene"], *scope)
    bundle_path, bundle_manifest_path = Path(bundle_path), Path(bundle_manifest_path)
    raw = preview.read_limited(bundle_path, MAX_BUNDLE)
    manifest_raw = preview.read_limited(bundle_manifest_path, preview.MAX_INPUT)
    manifest = json.loads(manifest_raw, object_pairs_hook=preview._pairs, parse_constant=preview._invalid_number)
    if len(raw) < 20 or not isinstance(manifest, dict):
        raise ValueError("variant bundle or manifest is invalid")
    magic, key_len, original_len, replacement_len = struct.unpack_from("<8sIII", raw)
    if (magic != b"T3VAR01\0" or not 1 <= key_len <= variants.MAX_KEY or
            not 1 <= original_len <= variants.MAX_PACKET or not 1 <= replacement_len <= variants.MAX_PACKET or
            len(raw) != 20+key_len+original_len+replacement_len):
        raise ValueError("variant bundle is invalid")
    key = raw[20:20+key_len].decode("ascii")
    original = raw[20+key_len:20+key_len+original_len]
    replacement = raw[20+key_len+original_len:]
    if (original != base or manifest.get("schema") != "t3.scene.pbr-variants" or
            type(manifest.get("version")) is not int or manifest["version"] != 1 or
            manifest.get("packet_sha256") != _digest(raw) or
            manifest.get("scene_sha256") != first["hashes"][0] or
            manifest.get("source_frame") != first["scene"].source.frame_id or
            manifest.get("source_fingerprints") != first["scene"].source.fingerprints or
            manifest.get("calibration_sha256") != first["manifest"].get("calibration_sha256") or
            manifest.get("light_scene_sha256") != first["manifest"].get("light_scene_sha256") or
            temporal._light_source(first["scene"], manifest) != temporal._light_source(first["scene"], first["manifest"])):
        raise ValueError("variant bundle does not match the first captured frame")
    if not isinstance(manifest.get("original"), dict):
        raise ValueError("variant bundle lacks original draw evidence")
    rebuilt, report = variants.build(first["scene"], original, manifest["original"], replacement, manifest)
    if rebuilt != raw or report != manifest.get("variant") or key != report["resource_key"]:
        raise ValueError("semantic bed variant evidence does not match its bundle")
    return {"frames": frames, "key": key, "trace": Path(trace),
            "trace_sha256": first["scene"].source.fingerprints["trace"], "scope": scope,
            "bundle": bundle_path, "bundle_manifest": bundle_manifest_path,
            "bundle_sha256": _digest(raw), "bundle_manifest_sha256": _digest(manifest_raw)}


def _path(path, wine):
    value = str(Path(path).resolve())
    if any(c in value for c in "\t\r\n"):
        raise ValueError("stream paths cannot contain tabs or newlines")
    return "Z:" + value.replace("/", "\\") if wine else value


def highball_environment(engine, bottle):
    """Configure only the child process; do not alter the saved bottle."""
    engine, bottle = Path(engine).resolve(), Path(bottle).resolve()
    wine, wineserver, frameworks = engine/"engine/bin/wine", engine/"engine/bin/wineserver", engine/"frameworks"
    drive = bottle/"dosdevices/z:"
    if (not wine.is_file() or not wineserver.is_file() or not frameworks.is_dir() or
            not drive.is_symlink() or os.readlink(drive) != "/"):
        raise ValueError("invalid Highball engine, bottle or Z: drive")
    env = {key: value for key, value in os.environ.items() if not key.startswith("T3RENDER_") and
           key not in ("WINEDLLPATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH")}
    env.update(WINEPREFIX=str(bottle), WINESERVER=str(wineserver),
               WINEDLLPATH_PREPEND=f"{bottle}/.highball-dlss/wine:{engine}/renderers/d3dmetal-tsshim/wine:{engine}/renderers/d3dmetal/wine",
               DYLD_FALLBACK_LIBRARY_PATH=f"{frameworks}:{frameworks}/GStreamer.framework/Versions/1.0/lib",
               DYLD_FALLBACK_FRAMEWORK_PATH=str(frameworks),
               WINEDLLOVERRIDES="nvapi64,nvngx=n,b;winemenubuilder.exe=d", WINEDEBUG="-all")
    return wine, env


def run(plan, output_dir, renderer, *, wine=None, effects=(), timeout=300, environment=None):
    if not math.isfinite(timeout) or not 0 < timeout <= 900:
        raise ValueError("invalid renderer timeout")
    output_dir = Path(output_dir).resolve()
    renderer = Path(renderer).resolve()
    renderer_digest = _digest(preview.read_limited(renderer, preview.MAX_INPUT))
    frames = plan["frames"]
    first_id = frames[0]["scene"].source.frame_id
    names = [(first_id, "original"), (first_id, "replacement")]
    commands = ["replacement " + _path(output_dir/f"frame-{first_id}.replacement.ppm", wine)]
    for index, frame in enumerate(frames[1:], 1):
        number = frame["scene"].source.frame_id
        names.extend(((number, "replacement"), (number, "original")))
        commands.append("frame " + _path(frame["paths"][1], wine) + "\t" +
                        _path(output_dir/f"frame-{number}.replacement.ppm", wine))
        commands.append("original " + _path(output_dir/f"frame-{number}.original.ppm", wine))
        if index+1 < len(frames):
            names.append((number, "replacement-repeat"))
            commands.append("replacement " + _path(output_dir/f"frame-{number}.replacement-repeat.ppm", wine))
    commands.append("quit")
    args = ([str(Path(wine).resolve())] if wine else []) + [str(renderer), _path(plan["bundle"], wine),
            "--output", _path(output_dir/f"frame-{first_id}.original.ppm", wine),
            *["--" + value for value in effects], "--variant-stream", "--resource-key", plan["key"]]
    output_dir.mkdir()
    with (output_dir/"renderer.log").open("xb") as log:
        result = subprocess.run(args, input="\n".join(commands)+"\n", text=True, stdout=log,
                                stderr=subprocess.STDOUT, timeout=timeout, env=environment)
    if result.returncode:
        raise ValueError(f"renderer failed ({result.returncode}); inspect {output_dir / 'renderer.log'}")
    log = preview.read_limited(output_dir/"renderer.log", 16*1024*1024).decode("utf-8", errors="replace")
    if (log.count("sceneir_pbr_dx12=ok") != len(names) or log.count("variant_camera=updated") != len(frames)-1 or
            log.count("variant_gpu_resources=shared") != 1):
        raise ValueError("renderer did not confirm all requested camera updates and selections")
    images = []
    width, height = frames[0]["scene"].camera.viewport.width, frames[0]["scene"].camera.viewport.height
    header = f"P6\n{width} {height}\n255\n".encode()
    for number, variant in names:
        path = output_dir/f"frame-{number}.{variant}.ppm"
        raw = preview.read_limited(path, len(header)+width*height*3)
        if not raw.startswith(header) or len(raw) != len(header)+width*height*3:
            raise ValueError("renderer produced an invalid frame image")
        images.append({"frame": number, "variant": variant, "sha256": _digest(raw)})
    for frame in frames:
        for path, digest in zip(frame["paths"], frame["hashes"]):
            if _digest(preview.read_limited(path, preview.MAX_INPUT)) != digest:
                raise ValueError("frame input changed during rendering")
    for path, digest, limit in ((plan["bundle"], plan["bundle_sha256"], MAX_BUNDLE),
                                (plan["bundle_manifest"], plan["bundle_manifest_sha256"], preview.MAX_INPUT)):
        if _digest(preview.read_limited(path, limit)) != digest:
            raise ValueError("variant input changed during rendering")
    # The trace can be large; hash it incrementally without storing its data.
    digest = hashlib.sha256()
    with plan["trace"].open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b""):
            digest.update(chunk)
    if digest.hexdigest() != plan["trace_sha256"]:
        raise ValueError("trace changed during rendering")
    if _digest(preview.read_limited(renderer, preview.MAX_INPUT)) != renderer_digest:
        raise ValueError("renderer changed during execution")
    report = {"schema": "t3.scene.pbr-camera-sequence", "version": 1, "trace_sha256": plan["trace_sha256"],
              "bundle_sha256": plan["bundle_sha256"], "renderer_sha256": renderer_digest, "effects": list(effects),
              "resource_key": plan["key"], "render_pass": plan["scope"][0], "stage": plan["scope"][1],
              "light_source": temporal._light_source(frames[0]["scene"], frames[0]["manifest"]),
              "frames": [{"frame": f["scene"].source.frame_id, "scene_sha256": f["hashes"][0],
                          "packet_sha256": f["hashes"][1]} for f in frames], "images": images,
              "limitations": ["Finalized capture processing; not a live TDS connection.",
                              "Only camera updates are supported; other scene changes are rejected.",
                              "Temporal history is reset; per-object motion vectors are unavailable.",
                              "Repeated reads cannot exclude ABA or unobserved writes."]}
    with (output_dir/"sequence.manifest.json").open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write("\n")
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--bundle-manifest", type=Path, required=True)
    parser.add_argument("--frame", nargs=3, metavar=("SCENE", "PACKET", "MANIFEST"), action="append", required=True)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--wine", type=Path, help="Wine executable with Z: mapped to the host root")
    parser.add_argument("--highball-engine", type=Path, help="Highball engine root")
    parser.add_argument("--highball-bottle", type=Path, help="Highball bottle with Z: mapped to the host root")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--effect", action="append", choices=("indirect", "gtao", "volumetric", "reflections", "filmic"), default=[])
    parser.add_argument("--timeout", type=float, default=300)
    args = parser.parse_args(argv)
    try:
        if len(set(args.effect)) != len(args.effect):
            raise ValueError("effects must be distinct")
        if (args.highball_engine is None) != (args.highball_bottle is None) or (
                args.wine is not None and args.highball_engine is not None):
            raise ValueError("supply both Highball paths or a separate Wine executable")
        wine, environment = ((args.wine, None) if args.highball_engine is None else
                             highball_environment(args.highball_engine, args.highball_bottle))
        plan = prepare(args.trace, args.bundle, args.bundle_manifest, args.frame)
        result = run(plan, args.output_dir, args.renderer, wine=wine, effects=args.effect, timeout=args.timeout,
                     environment=environment)
        print(json.dumps(result, indent=2, allow_nan=False))
    except (OSError, ValueError, TypeError, KeyError, IndexError, UnicodeError, struct.error, subprocess.SubprocessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
