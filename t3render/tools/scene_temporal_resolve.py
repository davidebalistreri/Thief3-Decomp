#!/usr/bin/env python3
"""Resolve static-geometry temporal history from D3D12 HDR and position buffers.

The runtime path verifies the SceneIR and packet manifests. Synthetic inputs
are explicitly labeled and do not claim to be runtime frames.
"""
import argparse
from array import array
from collections import Counter
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import sys

import scene_ir_to_d3d12_packet as preview
from scene_ir import SceneSnapshot

HEADER = struct.Struct("<8sII32s32f7fI")
MOTION_HEADER = struct.Struct("<8sII32s")
MAX_BYTES = HEADER.size + 2048*2048*16


def parse(raw, packet):
    if not isinstance(raw, bytes) or len(raw) < HEADER.size or len(raw) > MAX_BYTES:
        raise ValueError("invalid temporal buffer size")
    header = HEADER.unpack_from(raw)
    magic, width, height, digest = header[:4]
    matrix, camera, scale, exposure, jitter, flags = header[4:36], header[36:39], header[39], header[40], header[41:43], header[43]
    if (magic != b"T3TEMP01" or not 1 <= width <= 2048 or not 1 <= height <= 2048 or
            len(raw) != HEADER.size+width*height*16 or flags > 15 or
            any(not math.isfinite(x) or abs(x) > 1000000 for x in (*matrix, *camera)) or
            not math.isfinite(scale) or not .00001 <= scale <= 1 or
            not math.isfinite(exposure) or not .01 <= exposure <= 32 or
            any(not math.isfinite(x) or abs(x) > .5 for x in jitter)):
        raise ValueError("invalid temporal header")
    if not isinstance(packet, bytes) or len(packet) < 196 or hashlib.sha256(packet).digest() != digest:
        raise ValueError("temporal buffer does not match packet SHA-256")
    packet_header = struct.unpack_from("<8sIIff32f4f4fIII", packet)
    if (packet_header[0] not in (b"T3PBR01\0", b"T3PBR02\0", b"T3PBR03\0", b"T3PBR04\0") or
            packet_header[1:3] != (width, height) or packet_header[5:37] != matrix or
            packet_header[37:40] != camera or packet_header[40] != scale or packet_header[44] != exposure):
        raise ValueError("temporal camera does not match packet")
    return {"raw": raw, "width": width, "height": height, "digest": digest.hex(), "view": matrix[:16],
            "projection": matrix[16:], "camera": camera, "scale": scale, "exposure": exposure,
            "jitter": jitter, "flags": flags, "packet_version": int(chr(packet_header[0][6]))}


def validate_runtime(temporal, scene_raw, manifest_raw):
    scene = SceneSnapshot.from_json(scene_raw)
    manifest = json.loads(manifest_raw, object_pairs_hook=preview._pairs, parse_constant=preview._invalid_number)
    if (not isinstance(manifest, dict) or manifest.get("schema") != "t3.scene.pbr-packet" or
            type(manifest.get("version")) is not int or manifest["version"] != temporal["packet_version"] or
            manifest.get("packet_sha256") != temporal["digest"] or
            manifest.get("scene_sha256") != hashlib.sha256(scene_raw).hexdigest() or
            manifest.get("source_frame") != scene.source.frame_id or
            manifest.get("source_fingerprints") != scene.source.fingerprints or
            scene.source.kind != "runtime" or scene.source.camera_synchronized is not True or scene.camera is None or
            type(manifest.get("render_pass")) is not int or type(manifest.get("stage")) is not int or
            not any((s.render_pass, s.stage) == (manifest["render_pass"], manifest["stage"]) for s in scene.submissions) or
            not isinstance(manifest.get("textures"), list) or any(not isinstance(t, dict) or
                not isinstance(t.get("name"), str) or not isinstance(t.get("bundle"), str) for t in manifest["textures"]) or
            (manifest.get("shadow_maps") is not None and not isinstance(manifest["shadow_maps"], dict)) or
            (manifest.get("candle_flames") is not None and not isinstance(manifest["candle_flames"], dict)) or
            not isinstance(manifest.get("lights"), list) or any(not isinstance(light, dict) or
                not isinstance(light.get("id"), str) or not isinstance(light.get("position"), list) or
                len(light["position"]) != 3 or not isinstance(light.get("color_linear"), list) or
                len(light["color_linear"]) != 3 for light in manifest["lights"]) or
            not isinstance(manifest.get("calibration_sha256"), str) or
            not re.fullmatch(r"[0-9a-f]{64}", manifest["calibration_sha256"]) or
            (scene.camera.viewport.width, scene.camera.viewport.height) != (temporal["width"], temporal["height"])):
        raise ValueError("temporal runtime provenance does not match the synchronized SceneIR")
    floats = lambda values: struct.unpack(f"<{len(values)}f", struct.pack(f"<{len(values)}f", *values))
    if (floats(scene.camera.view) != temporal["view"] or floats(scene.camera.projection) != temporal["projection"] or
            scene.conventions.meters_per_unit is None or floats((scene.conventions.meters_per_unit,))[0] != temporal["scale"]):
        raise ValueError("temporal camera or units differ from SceneIR")
    return scene, manifest


def compatible(previous, current, previous_runtime=None, current_runtime=None):
    if (previous["width"], previous["height"], previous["scale"], previous["flags"]) != (
            current["width"], current["height"], current["scale"], current["flags"]):
        raise ValueError("temporal frames have incompatible dimensions, units or effects")
    if (previous_runtime is None) != (current_runtime is None):
        raise ValueError("cannot combine runtime and synthetic temporal sources")
    if current_runtime is not None:
        old, old_manifest = previous_runtime
        new, new_manifest = current_runtime
        if (old.source.map != new.source.map or old.source.frame_id >= new.source.frame_id or any(
                old.source.fingerprints.get(k) != new.source.fingerprints.get(k)
                for k in ("map", "bundle", "kernel", "offline_scene")) or
                old_manifest.get("calibration_sha256") != new_manifest.get("calibration_sha256") or
                (old_manifest["render_pass"], old_manifest["stage"]) != (new_manifest["render_pass"], new_manifest["stage"])):
            raise ValueError("runtime temporal frames have incompatible map, order or light calibration")
        def material_signature(manifest):
            return sorted((t.get("bundle"), t.get("name"), t.get("source_sha256")) for t in manifest.get("textures", []))
        def shadow_signature(manifest):
            value = manifest.get("shadow_maps")
            return None if value is None else tuple(value.get(k) for k in ("size", "near_m", "far_m", "bias_m", "softness", "casters"))
        def flame_signature(manifest):
            value = manifest.get("candle_flames")
            return None if value is None else (value.get("source_sha256"), value.get("radiance_linear"))
        def light_signature(manifest):
            return sorted((light["id"], tuple(light["position"]), tuple(light["color_linear"]),
                           light.get("intensity_candela"), light.get("radius_m")) for light in manifest["lights"])
        if (material_signature(old_manifest) != material_signature(new_manifest) or
                shadow_signature(old_manifest) != shadow_signature(new_manifest) or
                flame_signature(old_manifest) != flame_signature(new_manifest) or
                light_signature(old_manifest) != light_signature(new_manifest)):
            raise ValueError("runtime temporal frames have incompatible material or lighting settings")


def _decode(raw, offset, count, position=False):
    result = array("f")
    for values in struct.iter_unpack("<4e", memoryview(raw)[offset:offset+count*8]):
        if (any(not math.isfinite(x) for x in values) or
                (position and (any(abs(x) > 60000 for x in values[:3]) or values[3] not in (0, 1))) or
                (not position and (any(not 0 <= x <= 65504 for x in values[:3]) or values[3] not in (0, 1)))):
            raise ValueError("temporal pixels contain invalid HDR or position values")
        result.extend(values)
    return result


def _view_projection(view, projection):
    return tuple(sum(view[i*4+k]*projection[k*4+j] for k in range(4)) for i in range(4) for j in range(4))


def resolve(previous, current, weight=.8):
    compatible(previous, current)
    if isinstance(weight, bool) or not isinstance(weight, (int, float)) or not math.isfinite(weight) or not 0 <= weight <= .95:
        raise ValueError("temporal history weight must be between 0 and 0.95")
    width, height, scale = current["width"], current["height"], current["scale"]
    count = width*height
    color = _decode(current["raw"], HEADER.size, count)
    position = _decode(current["raw"], HEADER.size+count*8, count, position=True)
    old_color = _decode(previous["raw"], HEADER.size, count)
    old_position = _decode(previous["raw"], HEADER.size+count*8, count, position=True)
    vp = _view_projection(previous["view"], previous["projection"])
    camera = current["camera"]
    coeff = [[vp[k*4+j]/scale for k in range(3)] for j in range(4)]
    origin = [sum(camera[k]*vp[k*4+j] for k in range(3))+vp[12+j] for j in range(4)]
    camera_delta = tuple((camera[k]-previous["camera"][k])*scale for k in range(3))
    jx, jy = previous["jitter"]
    out = bytearray(current["raw"])
    motion = bytearray(MOTION_HEADER.pack(b"T3MOTN01", width, height, bytes.fromhex(current["digest"])) + bytes(count*12))
    counts = Counter()
    max_motion = 0.0
    for index in range(count):
        at = index*4
        if position[at+3] < .5:
            counts["background"] += 1
            continue
        px, py, pz = position[at:at+3]
        clip = [origin[j]+px*coeff[j][0]+py*coeff[j][1]+pz*coeff[j][2] for j in range(4)]
        if clip[3] <= 1e-8 or not 0 <= clip[2] <= clip[3]:
            counts["outside"] += 1
            struct.pack_into("<ffI", motion, MOTION_HEADER.size+index*12, 0, 0, 1)
            continue
        sx = (clip[0]/clip[3]*.5+.5)*width+.5+jx
        sy = (.5-clip[1]/clip[3]*.5)*height+.5+jy
        if not 0 <= sx < width or not 0 <= sy < height:
            counts["outside"] += 1
            struct.pack_into("<ffI", motion, MOTION_HEADER.size+index*12, 0, 0, 1)
            continue
        x, y = index % width, index // width
        motion_x, motion_y = sx-(x+.5), sy-(y+.5)
        qx, qy = int(sx), int(sy)
        old_at = (qy*width+qx)*4
        dx, dy, dz = (old_position[old_at+k] - (position[at+k]+camera_delta[k]) for k in range(3))
        if old_position[old_at+3] < .5 or dx*dx+dy*dy+dz*dz > .05**2:
            counts["disoccluded"] += 1
            struct.pack_into("<ffI", motion, MOTION_HEADER.size+index*12, motion_x, motion_y, 2)
            continue
        max_motion = max(max_motion, math.hypot(motion_x, motion_y))
        struct.pack_into("<ffI", motion, MOTION_HEADER.size+index*12, motion_x, motion_y, 3)
        rgb = []
        for channel in range(3):
            values = [color[(ny*width+nx)*4+channel] for ny in range(max(0,y-1), min(height,y+2))
                      for nx in range(max(0,x-1), min(width,x+2))]
            history = min(max(old_color[old_at+channel], min(values)), max(values))
            rgb.append(color[at+channel]*(1-weight)+history*weight)
        struct.pack_into("<4e", out, HEADER.size+index*8, *rgb, color[at+3])
        counts["reprojected"] += 1
    return bytes(out), bytes(motion), {"history_weight": weight, "counts": dict(sorted(counts.items())),
                                      "max_reprojection_pixels": max_motion}


def image(raw, upscale=1):
    if type(upscale) is not int or upscale not in (1, 2):
        raise ValueError("temporal preview upscale must be 1 or 2")
    width, height = struct.unpack_from("<II", raw, 8)
    exposure = HEADER.unpack_from(raw)[40]
    pixels = _decode(raw, HEADER.size, width*height)
    positions = _decode(raw, HEADER.size+width*height*8, width*height, position=True) if upscale == 2 else None
    out_width, out_height = width*upscale, height*upscale
    result = bytearray(f"P6\n{out_width} {out_height}\n255\n".encode())

    def display(rgb):
        for value in rgb:
            value = max(value*exposure, 0)
            value = value/(1+value)
            value = 12.92*value if value <= .0031308 else 1.055*value**(1/2.4)-.055
            result.append(max(0, min(255, round(value*255))))
    if upscale == 1:
        for index in range(width*height):
            display(pixels[index*4:index*4+3])
        return bytes(result)

    for y in range(out_height):
        sy = (y+.5)/2-.5
        y0 = math.floor(sy)
        for x in range(out_width):
            sx = (x+.5)/2-.5
            x0 = math.floor(sx)
            ax, ay = min(width-1, max(0, math.floor(sx+.5))), min(height-1, max(0, math.floor(sy+.5)))
            anchor = (ay*width+ax)*4
            rgba = [0., 0., 0.]
            total = 0.
            for ny in (y0, y0+1):
                for nx in (x0, x0+1):
                    cx, cy = min(width-1, max(0, nx)), min(height-1, max(0, ny))
                    at = (cy*width+cx)*4
                    weight = (1-abs(sx-nx))*(1-abs(sy-ny))
                    if positions[at+3] != positions[anchor+3]:
                        continue
                    if positions[anchor+3] and sum((positions[at+k]-positions[anchor+k])**2 for k in range(3)) > .15**2:
                        continue
                    for k in range(3): rgba[k] += pixels[at+k]*weight
                    total += weight
            display([value/total for value in rgba] if total > 0 else pixels[anchor:anchor+3])
    return bytes(result)


def _write_outputs(path, body, picture, motion, manifest):
    outputs = ((path, body), (Path(str(path)+".ppm"), picture), (Path(str(path)+".motion.bin"), motion),
               (Path(str(path)+".manifest.json"), (json.dumps(manifest, indent=2, allow_nan=False)+"\n").encode()))
    created = []
    try:
        for destination, content in outputs:
            with destination.open("xb") as stream:
                info = os.fstat(stream.fileno())
                created.append((destination, info.st_dev, info.st_ino))
                stream.write(content)
    except Exception:
        for destination, device, inode in created:
            try:
                info = destination.lstat()
                if (info.st_dev, info.st_ino) == (device, inode): destination.unlink()
            except OSError:
                pass
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("previous", type=Path)
    parser.add_argument("current", type=Path)
    parser.add_argument("--previous-packet", type=Path, required=True)
    parser.add_argument("--current-packet", type=Path, required=True)
    parser.add_argument("--previous-scene", type=Path)
    parser.add_argument("--current-scene", type=Path)
    parser.add_argument("--previous-manifest", type=Path)
    parser.add_argument("--current-manifest", type=Path)
    parser.add_argument("--synthetic", action="store_true")
    parser.add_argument("--weight", type=float, default=.8)
    parser.add_argument("--upscale", type=int, choices=(1, 2), default=1,
                        help="optional 2x depth-aware spatial reconstruction of the preview")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        paths = (args.previous_scene, args.current_scene, args.previous_manifest, args.current_manifest)
        if (args.synthetic and any(p is not None for p in paths)) or (not args.synthetic and any(p is None for p in paths)):
            raise ValueError("supply both runtime scenes and manifests, or select --synthetic")
        previous_raw = preview.read_limited(args.previous, MAX_BYTES)
        current_raw = preview.read_limited(args.current, MAX_BYTES)
        previous = parse(previous_raw, preview.read_limited(args.previous_packet, 64*1024*1024))
        current = parse(current_raw, preview.read_limited(args.current_packet, 64*1024*1024))
        old_runtime = new_runtime = None
        if not args.synthetic:
            old_runtime = validate_runtime(previous, preview.read_limited(args.previous_scene, preview.MAX_INPUT),
                                           preview.read_limited(args.previous_manifest, preview.MAX_INPUT))
            new_runtime = validate_runtime(current, preview.read_limited(args.current_scene, preview.MAX_INPUT),
                                           preview.read_limited(args.current_manifest, preview.MAX_INPUT))
        compatible(previous, current, old_runtime, new_runtime)
        result, motion, stats = resolve(previous, current, args.weight)
        manifest = {"schema": "t3.scene.temporal-history", "version": 1,
                    "source": "synthetic" if args.synthetic else "runtime",
                    "previous_frame": None if args.synthetic else old_runtime[0].source.frame_id,
                    "current_frame": None if args.synthetic else new_runtime[0].source.frame_id,
                    "source_map": None if args.synthetic else new_runtime[0].source.map,
                    "source_fingerprints": {} if args.synthetic else new_runtime[0].source.fingerprints,
                    "submission_scope": None if args.synthetic else {"render_pass": new_runtime[1]["render_pass"],
                                                                        "stage": new_runtime[1]["stage"]},
                    "previous_sha256": hashlib.sha256(previous_raw).hexdigest(),
                    "current_sha256": hashlib.sha256(current_raw).hexdigest(),
                    "packet_sha256": current["digest"], "history_sha256": hashlib.sha256(result).hexdigest(),
                    "motion_sha256": hashlib.sha256(motion).hexdigest(),
                    "limitations": ["Reprojection uses static world positions and camera motion; object motion vectors are not available.",
                                    "History is nearest sampled, position rejected and neighborhood clamped; temporal stability is not guaranteed.",
                                    "Upscaling is a depth-aware spatial preview; history and motion remain at native resolution."],
                    "upscale": args.upscale, "image_size": [current["width"]*args.upscale, current["height"]*args.upscale],
                    **stats}
        picture = image(result, args.upscale)
        manifest["image_sha256"] = hashlib.sha256(picture).hexdigest()
        _write_outputs(args.output, result, picture, motion, manifest)
        print(json.dumps(manifest, indent=2, allow_nan=False))
    except (OSError, ValueError, TypeError, KeyError, IndexError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
