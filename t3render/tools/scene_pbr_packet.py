#!/usr/bin/env python3
"""Prepare an independent, bounded PBR preview packet from scoped SceneIR.

The light calibration is explicit preview input, not a measurement of Flesh.
The packet contains semantic geometry and textures, never legacy draw state.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

import compare_bridge_fixture as comparison
import scene_direct_candidates as direct
import scene_ir_to_d3d12_packet as preview
import scene_ir_to_textured_packet as legacy
import scene_legacy_states as states
import scene_material_overrides as overrides
import scene_pbr_offline_casters as offline_casters
import scene_pbr_flames as flames
from ibt import IBT
from scene_ir import SceneSnapshot, transform_point
from t3common import game_dir, resolve_map

MAGIC = b"T3PBR01\0"
SHADOW_MAGIC = b"T3PBR02\0"
OFFLINE_CASTER_MAGIC = b"T3PBR03\0"
EMISSION_MAGIC = b"T3PBR04\0"
MAX_LIGHTS = 16
MAX_TRIANGLES = 100000
MAX_DRAWS = 8192
MAX_TEXTURES = 64
MAX_BYTES = 64 * 1024 * 1024
MAX_SHADOW_DRAWS = 65536
MAX_SHADOW_TRIANGLES = 6000000


def _bit_count(mask):
    # macOS ships Python 3.9, which predates int.bit_count().
    return bin(mask).count("1")


def _number(value, low, high, name):
    if (isinstance(value, bool) or not isinstance(value, (int, float)) or
            not math.isfinite(value) or not low <= value <= high):
        raise ValueError(f"invalid {name}")
    return float(value)


def _rgb(value, high, name):
    if not isinstance(value, list) or len(value) != 3:
        raise ValueError(f"invalid {name}")
    return tuple(_number(x, 0, high, name) for x in value)


def load_calibration(raw, light_scene):
    data = json.loads(raw, object_pairs_hook=preview._pairs, parse_constant=preview._invalid_number)
    if (not isinstance(data, dict) or set(data) != {"schema", "version", "ambient_linear", "exposure", "lights"} or
            data["schema"] != "t3.scene.pbr-preview" or type(data["version"]) is not int or data["version"] != 1 or
            not isinstance(data["lights"], list) or not 1 <= len(data["lights"]) <= MAX_LIGHTS):
        raise ValueError("invalid PBR preview calibration")
    ambient = _rgb(data["ambient_linear"], 4, "ambient color")
    exposure = _number(data["exposure"], .01, 32, "exposure")
    available = {light.id: light for light in light_scene.lights}
    result, seen = [], set()
    for item in data["lights"]:
        if (not isinstance(item, dict) or set(item) != {"id", "color_linear", "intensity_candela", "radius_m"} or
                not isinstance(item["id"], str) or item["id"] in seen or item["id"] not in available):
            raise ValueError("invalid or duplicate PBR light identity")
        light = available[item["id"]]
        if light.kind not in ("point", "omni"):
            raise ValueError("PBR preview currently supports point lights only")
        if light.position is None or any(abs(x) > 1000000 for x in light.position):
            raise ValueError("PBR light position is unavailable or out of range")
        seen.add(item["id"])
        result.append({"id": item["id"], "position": light.position,
                       "color_linear": _rgb(item["color_linear"], 1, "light color"),
                       "intensity_candela": _number(item["intensity_candela"], 0, 10000, "light intensity"),
                       "radius_m": _number(item["radius_m"], .01, 20, "light radius")})
    return ambient, exposure, result


def _constant(name, rgba):
    return {"name": name, "bundle": "t3render_defaults", "format": "RGBA8", "width": 1, "height": 1,
            "mips": [bytes(rgba)]}


def shadow_parameters(value):
    if not isinstance(value, dict) or set(value) != {"size", "near_m", "far_m", "bias_m", "softness"}:
        raise ValueError("invalid shadow map parameters")
    size = value["size"]
    if type(size) is not int or size < 64 or size > 512 or size & (size-1):
        raise ValueError("shadow map size must be a power of two from 64 to 512")
    def f32(value, low, high, name):
        result = struct.unpack("<f", struct.pack("<f", _number(value, low, high, name)))[0]
        bounds = [struct.unpack("<f", struct.pack("<f", bound))[0] for bound in (low, high)]
        return _number(result, *bounds, name)
    result = {"size": size, "near_m": f32(value["near_m"], .01, 1, "shadow near"),
              "far_m": f32(value["far_m"], .01, 100, "shadow far"),
              "bias_m": f32(value["bias_m"], 0, .25, "shadow bias"),
              "softness": f32(value["softness"], 0, 2, "shadow softness")}
    if result["far_m"] <= result["near_m"]:
        raise ValueError("shadow far must be greater than near")
    return result


def shadow_owners(scene, lights, actors):
    """Validate semantic light ownership using the fingerprinted actor export."""
    if not isinstance(actors, dict):
        raise ValueError("shadow maps require the fingerprinted offline actors")
    prefix = f"candidate:light:{scene.source.map}:{scene.source.fingerprints.get('map')}:"
    owners = []
    for light in lights:
        identity = light["id"]
        if not identity.startswith(prefix) or ":" in identity[len(prefix):]:
            raise ValueError("shadow light requires a qualified candidate identity")
        name = identity[len(prefix):]
        actor = actors.get(name)
        if not isinstance(actor, dict):
            raise ValueError("shadow light lacks an offline actor")
        parent = actor.get("attached_to")
        if parent is not None and (not isinstance(parent, str) or parent not in actors):
            raise ValueError("shadow light has an unresolved offline parent")
        owners.append({name, parent})
    return owners


def create_packet(scene, light_scene, calibration, resolver, remaster, render_pass=None, stage=None,
                  shadows=None, actors=None, offline=None, offline_raw=None, caster_resolve=None,
                  flame_glb=None, flame_radiance=8):
    render_pass, stage = legacy.select_scope(scene, render_pass, stage)
    legacy.select_scope(light_scene)
    if (light_scene.source.kind != "runtime" or scene.source.frame_id != light_scene.source.frame_id or
            scene.source.map != light_scene.source.map or scene.source.fingerprints != light_scene.source.fingerprints or
            scene.camera != light_scene.camera or scene.conventions != light_scene.conventions):
        raise ValueError("PBR light candidates must share the synchronized scene frame")
    if remaster is not None:
        remaster.validate_scene(scene)
    scale = scene.conventions.meters_per_unit
    if scale is None or not .00001 <= scale <= 1:
        raise ValueError("PBR preview requires an explicit scene unit scale")
    camera = comparison.camera_position(scene.camera.view)
    if camera is None or any(abs(x) > 1000000 for x in camera):
        raise ValueError("PBR preview requires an invertible camera view")
    ambient, exposure, lights = load_calibration(calibration, light_scene)
    shadow = shadow_parameters(shadows) if shadows is not None else None
    if offline is not None and (shadow is None or offline_raw is None or caster_resolve is None):
        raise ValueError("offline shadow casters require shadows, fixture bytes and a material resolver")
    if offline is None and (offline_raw is not None or caster_resolve is not None):
        raise ValueError("offline caster inputs require an offline scene")
    if flame_glb is not None and shadow is None:
        raise ValueError("candle flames require shadow maps and fingerprinted offline actors")
    owners = shadow_owners(scene, lights, actors) if shadow is not None else None
    resources = {m.id: m for m in scene.static_mesh_resources}
    instances = {m.id: m for m in scene.mesh_instances}
    blocks = {b.id: b for b in scene.bsp_blocks}
    materials = {m.id: m.name for m in scene.materials}
    textures, texture_ids, draws, records, skipped = [], {}, [], [], []

    def texture_index(tex):
        key = (tex["bundle"], tex["name"].lower())
        if key not in texture_ids:
            if len(textures) >= MAX_TEXTURES:
                raise ValueError("PBR texture count exceeds bounds")
            texture_ids[key] = len(textures)
            textures.append(tex)
        return texture_ids[key]

    flat_normal = _constant("flat_normal", (128, 128, 255, 255))
    default_roughness = _constant("roughness_0_8", (204, 204, 204, 255))
    default_ao = _constant("ambient_occlusion_1", (255, 255, 255, 255))
    seen = set()
    triangles = 0
    for submission in scene.submissions:
        if (submission.render_pass, submission.stage) != (render_pass, stage):
            continue
        identity = (submission.target_kind, submission.target_id, submission.section)
        if identity in seen:
            raise ValueError("PBR scene contains duplicate submitted sections")
        seen.add(identity)
        if submission.target_kind == "mesh_instance":
            instance = instances[submission.target_id]
            resource = resources[instance.resource_id]
            geometry, matrix = resource.geometry, instance.transform
            section = geometry.sections[submission.section]
            material_id = (instance.material_ids[submission.section] if instance.material_ids else section.material_id)
        else:
            geometry, matrix = blocks[submission.target_id].geometry, preview.IDENTITY
            section = geometry.sections[submission.section]
            material_id = section.material_id
        material = materials.get(material_id)
        if not material or geometry.uv0 is None or geometry.normals is None:
            raise ValueError("PBR section requires a material, UV0 and normals")
        if any(abs(matrix[i]) > 1e-6 for i in (3, 7, 11)) or abs(matrix[15] - 1) > 1e-6:
            raise ValueError("PBR preview requires affine instance transforms")
        a, b, c, d, e, f, g, h, i = (matrix[n] for n in (0, 1, 2, 4, 5, 6, 8, 9, 10))
        mirrored = a*(e*i-f*h) - b*(d*i-f*g) + c*(d*h-e*g) < 0
        shadow_mask = 0
        if shadow is not None:
            if submission.target_kind == "bsp_block":
                shadow_mask = (1 << len(lights)) - 1
            else:
                actor_name = instance.id.rsplit(":", 1)[-1]
                actor = actors.get(actor_name)
                if (not isinstance(actor, dict) or type(actor.get("casts_shadows")) is not bool or
                        not isinstance(actor.get("mesh"), str) or actor["mesh"].lower() != resource.name.lower()):
                    raise ValueError("shadow caster lacks matching offline mesh and cast flag")
                if actor["casts_shadows"]:
                    shadow_mask = sum(1 << n for n, ignored in enumerate(owners) if actor_name not in ignored)
        diffuse = resolver.resolve(material)
        if any(mip[3::4].count(255) != len(mip)//4 for mip in diffuse["mips"]):
            skipped.append({"target_kind": identity[0], "target_id": identity[1], "section": identity[2],
                            "material": material, "reason": "nonopaque_diffuse_texture"})
            continue
        normal_xform = legacy.normal_matrix(matrix)
        indices = geometry.indices[section.first_index:section.first_index + section.index_count]
        vertices = []
        for index in indices:
            position = transform_point(geometry.positions[index], matrix)
            uv = geometry.uv0[index]
            if (abs(position[3] - 1) > 1e-6 or any(not math.isfinite(x) or abs(x) > 1000000 for x in position[:3] + uv)):
                raise ValueError("PBR vertex exceeds bounds")
            vertices.append((*position[:3], *uv, *legacy.transform_normal(geometry.normals[index], normal_xform)))
        if not vertices:
            raise ValueError("PBR submission has no triangles")
        if geometry.tangents is None:
            frames = [(*v, 0, 0, 0, 0) for v in vertices]
        else:
            frames, _ = legacy.tangent_frames(vertices, indices, geometry.tangents, matrix)
        ranges = (remaster.partition(submission.target_kind, material, vertices) if remaster is not None
                  else [(0, len(vertices), None)])
        for first, end, replacement in ranges:
            if replacement:
                albedo, normal, roughness, ao = (remaster.resolve(replacement, channel)
                                                  for channel in ("albedo", "normal", "roughness", "ao"))
                if any(mip[3::4].count(255) != len(mip)//4 for mip in albedo["mips"]):
                    raise ValueError("PBR replacement albedo must be opaque")
            else:
                albedo, normal, roughness, ao = diffuse, flat_normal, default_roughness, default_ao
            tex = [texture_index(x) for x in (albedo, normal, roughness, ao)]
            part = frames[first:end] if replacement else [(*v[:8], 0, 0, 0, 0) for v in frames[first:end]]
            if mirrored:
                # World-space expansion changes winding under a reflection.
                part = [part[n+j] for n in range(0, len(part), 3) for j in (0, 2, 1)]
            triangles += len(part)//3
            if triangles > MAX_TRIANGLES or len(draws) >= MAX_DRAWS:
                raise ValueError("PBR geometry exceeds bounds")
            draws.append((tex, part, shadow_mask, (0, 0, 0)))
            records.append({"target_kind": identity[0], "target_id": identity[1], "section": identity[2],
                            "material": material, "triangle_range": [first//3, end//3], "reversed_winding": mirrored,
                            "replacement": replacement, "albedo": albedo["name"], "normal": normal["name"],
                            "roughness": roughness["name"], "ao": ao["name"],
                            "flat_fallback_triangles": sum(part[n][11] == 0 for n in range(0, len(part), 3))})
            if shadow is not None:
                records[-1]["shadow_caster_lights"] = [lights[n]["id"] for n in range(len(lights)) if shadow_mask & (1 << n)]
    if not draws:
        raise ValueError("PBR preview has no opaque submitted geometry")
    flame_report = None
    if flame_glb is not None:
        strength = _number(flame_radiance, 0, 1000, "flame radiance")
        geometry, digest = flames.select(scene, lights, actors, flame_glb, (strength, strength*.4, strength*.08),
                                         render_pass, stage)
        black = _constant("emissive_black", (0, 0, 0, 255))
        tex = [texture_index(x) for x in (black, flat_normal, default_roughness, default_ao)] if geometry else []
        for item in geometry:
            part = item["vertices"]
            triangles += len(part)//3
            if triangles > MAX_TRIANGLES or len(draws) >= MAX_DRAWS:
                raise ValueError("PBR geometry exceeds bounds")
            draws.append((tex, part, 0, item["emission"]))
            records.append({"target_kind": "mesh_instance", "target_id": item["target_id"],
                            "section": "generated_candle_flame", "material": "CandleFlame", "replacement": "candle_flame",
                            "light_id": item["light_id"], "triangles": len(part)//3, "emission_linear": item["emission"],
                            "source": item["source"], "shadow_caster_lights": []})
        flame_report = {"source_sha256": digest, "source": "generated_godot_flame_geometry",
                        "light_state": "fingerprinted_offline_actor", "radiance_linear": strength,
                        "instances": len(geometry), "triangles": sum(len(item["vertices"])//3 for item in geometry)}
    casters, caster_stats = [], None
    if offline is not None:
        casters, caster_stats = offline_casters.select(scene, offline, offline_raw, actors, caster_resolve,
                                                        lights, shadow, owners, render_pass, stage)
    if shadow is not None:
        calls = sum(_bit_count(mask)*6 for _, _, mask, _ in draws)
        work = sum(_bit_count(mask)*6*(len(vertices)//3) for _, vertices, mask, _ in draws)
        calls += sum(_bit_count(caster["shadow_mask"])*6 for caster in casters)
        work += sum(_bit_count(caster["shadow_mask"])*6*(len(caster["vertices"])//3) for caster in casters)
        if calls > MAX_SHADOW_DRAWS or work > MAX_SHADOW_TRIANGLES:
            raise ValueError("shadow maps exceed renderer work bounds")
    viewport = scene.camera.viewport
    magic = EMISSION_MAGIC if flame_glb is not None else OFFLINE_CASTER_MAGIC if offline is not None else SHADOW_MAGIC if shadow else MAGIC
    body = bytearray(struct.pack("<8sIIff32f4f4fIII", magic, viewport.width, viewport.height,
                                 viewport.min_depth, viewport.max_depth, *scene.camera.view, *scene.camera.projection,
                                 *camera, scale, *ambient, exposure, len(textures), len(draws), len(lights)))
    if shadow is not None:
        body.extend(struct.pack("<I4f", shadow["size"], shadow["near_m"], shadow["far_m"],
                                shadow["bias_m"], shadow["softness"]))
        if offline is not None or flame_glb is not None:
            body.extend(struct.pack("<I", len(casters)))
    for light in lights:
        body.extend(struct.pack("<8f", *light["position"], light["radius_m"], *light["color_linear"],
                                light["intensity_candela"]))
    for tex in textures:
        body.extend(struct.pack("<III", tex["width"], tex["height"], len(tex["mips"])))
        for mip in tex["mips"]:
            body.extend(mip)
    for tex, vertices, shadow_mask, emission in draws:
        body.extend(struct.pack("<5I", *tex, len(vertices)))
        if shadow is not None:
            body.extend(struct.pack("<I", shadow_mask))
        if flame_glb is not None:
            body.extend(struct.pack("<3f", *emission))
        for vertex in vertices:
            body.extend(struct.pack("<12f", *vertex))
    for caster in casters:
        body.extend(struct.pack("<II", caster["shadow_mask"], len(caster["vertices"])))
        for vertex in caster["vertices"]:
            body.extend(struct.pack("<3f", *vertex))
    if len(body) > MAX_BYTES:
        raise ValueError("PBR packet exceeds bounds")
    stats = {"render_pass": render_pass, "stage": stage, "triangles": triangles, "draws": records,
             "skipped": skipped, "lights": lights,
             "textures": [{k: tex[k] for k in ("name", "bundle", "format", "width", "height")}
                          | {"mips": len(tex["mips"])}
                          | ({"source_sha256": tex["source_sha256"]} if "source_sha256" in tex else {})
                          for tex in textures]}
    if shadow is not None:
        stats["shadow_maps"] = {**shadow, "casters": "scoped_opaque_geometry_with_offline_cast_flags",
                                "draw_calls": calls, "triangle_submissions": work}
        if caster_stats is not None:
            stats["shadow_maps"]["casters"] = "scoped_opaque_geometry_and_offline_candidates"
            stats["shadow_maps"]["offline_candidates"] = {
                **caster_stats,
                "sections": [{k: value for k, value in caster.items() if k != "vertices"} | {
                    "triangles": len(caster["vertices"])//3,
                    "lights": [lights[n]["id"] for n in range(len(lights)) if caster["shadow_mask"] & (1 << n)]}
                    for caster in casters]}
    if flame_report is not None:
        stats["candle_flames"] = flame_report
    return bytes(body), stats


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene", type=Path, help="synchronized scoped geometry snapshot")
    parser.add_argument("--light-scene", type=Path, required=True, help="same-frame candidate-light snapshot")
    parser.add_argument("--calibration", type=Path, required=True, help="explicit artistic light and exposure values")
    parser.add_argument("--state-trace", type=Path, required=True)
    parser.add_argument("--offline-scene", type=Path, required=True)
    parser.add_argument("--offline-actors", type=Path, required=True)
    parser.add_argument("--game-dir")
    parser.add_argument("--pass", dest="render_pass", type=int)
    parser.add_argument("--stage", type=int)
    parser.add_argument("--bedroom-remaster-materials", type=Path)
    parser.add_argument("--shadows", action="store_true", help="render point-light shadow maps from selected opaque geometry")
    parser.add_argument("--offline-shadow-casters", action="store_true",
                        help="add bounded static caster candidates from the offline fixture")
    parser.add_argument("--candle-flames", type=Path, help="generated Godot candlestick GLB for semantic emissive flames")
    parser.add_argument("--flame-radiance", type=float, help="artistic linear flame radiance (default: 8)")
    parser.add_argument("--shadow-map-size", type=int)
    parser.add_argument("--shadow-near-m", type=float)
    parser.add_argument("--shadow-far-m", type=float)
    parser.add_argument("--shadow-bias-m", type=float)
    parser.add_argument("--shadow-softness", type=float)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.flame_radiance is not None and args.candle_flames is None:
            raise ValueError("--flame-radiance requires --candle-flames")
        if not args.shadows and (args.offline_shadow_casters or args.candle_flames is not None or any(x is not None for x in (args.shadow_map_size,
                args.shadow_near_m, args.shadow_far_m, args.shadow_bias_m, args.shadow_softness))):
            raise ValueError("shadow parameters require --shadows")
        scene_raw = preview.read_limited(args.scene, preview.MAX_INPUT)
        light_raw = preview.read_limited(args.light_scene, preview.MAX_INPUT)
        offline_raw = preview.read_limited(args.offline_scene, preview.MAX_INPUT)
        calibration = preview.read_limited(args.calibration, 1024 * 1024)
        scene, light_scene, offline = (SceneSnapshot.from_json(raw) for raw in (scene_raw, light_raw, offline_raw))
        game = game_dir(args.game_dir)
        for current, scope in ((scene, legacy.select_scope(scene, args.render_pass, args.stage)),
                               (light_scene, legacy.select_scope(light_scene))):
            states.from_trace(current, offline, offline_raw, args.state_trace, *scope,
                              self_attached_actors=args.offline_actors, self_attached_game=game,
                              light_actors=args.offline_actors, light_game=game)
        local = resolve_map(game, scene.source.map, ".ibt")
        paths = [("bundle", local)]
        if "kernel" in scene.source.fingerprints:
            paths.append(("kernel", local.parent / "Kernel_GFXALL.ibt"))
        for role, path in paths:
            if scene.source.fingerprints.get(role) != legacy.digest_file(path):
                raise ValueError(f"{role} bundle fingerprint does not match SceneIR")
        bundles = []
        try:
            for _, path in paths:
                bundles.append(IBT(path))
            remaster = (overrides.BedroomOverrides(scene, args.bedroom_remaster_materials,
                                                   texture_size=512, linear_albedo_mips=True)
                        if args.bedroom_remaster_materials is not None else None)
            shadow = ({"size": 512 if args.shadow_map_size is None else args.shadow_map_size,
                       "near_m": .05 if args.shadow_near_m is None else args.shadow_near_m,
                       "far_m": 15 if args.shadow_far_m is None else args.shadow_far_m,
                       "bias_m": .015 if args.shadow_bias_m is None else args.shadow_bias_m,
                       "softness": .5 if args.shadow_softness is None else args.shadow_softness}
                      if args.shadows else None)
            actors = direct.offline_actors(scene, offline, offline_raw, args.offline_actors) if shadow is not None else None
            body, stats = create_packet(scene, light_scene, calibration, legacy.TextureResolver(bundles), remaster,
                                        args.render_pass, args.stage, shadows=shadow, actors=actors,
                                        offline=offline if args.offline_shadow_casters else None,
                                        offline_raw=offline_raw if args.offline_shadow_casters else None,
                                        caster_resolve=(lambda name: legacy.TextureResolver(bundles).resolve(name))
                                        if args.offline_shadow_casters else None,
                                        flame_glb=preview.read_limited(args.candle_flames, flames.MAX_GLB_BYTES)
                                        if args.candle_flames is not None else None,
                                        flame_radiance=8 if args.flame_radiance is None else args.flame_radiance)
        finally:
            for bundle in bundles:
                bundle.close()
        manifest = {"schema": "t3.scene.pbr-packet", "version": 4 if args.candle_flames is not None else 3 if args.offline_shadow_casters else 2 if shadow is not None else 1,
                    "source_frame": scene.source.frame_id,
                    "scene_sha256": hashlib.sha256(scene_raw).hexdigest(),
                    "light_scene_sha256": hashlib.sha256(light_raw).hexdigest(),
                    "calibration_sha256": hashlib.sha256(calibration).hexdigest(),
                    "packet_sha256": hashlib.sha256(body).hexdigest(), "source_fingerprints": scene.source.fingerprints,
                    "source_limitations": list(scene.source.limitations),
                    "light_source_limitations": list(light_scene.source.limitations),
                    "limitations": ["PBR light positions are scoped candidates; colors, strengths and exposure are artistic preview values, not runtime measurements.",
                                    "Legacy geometry/material bindings are asset-backed candidates; the selected frame is incomplete.",
                                    "Sections with nonopaque offline diffuse alpha are omitted. Other unreplaced materials use vertex normals and roughness 0.8.",
                                    "Override AO, roughness and normals are generated offline; tangent space and the SDK unit scale are assumptions."],
                    **stats}
        if shadow is not None:
            manifest["limitations"].append("Shadow maps use selected opaque runtime-scope geometry and offline cast flags; runtime shadow flags are unknown.")
            if args.offline_shadow_casters:
                manifest["limitations"].append("Additional static casters are offline candidates, not frame observations; save-game changes, attachments and dynamic state can make them incorrect or incomplete.")
            else:
                manifest["limitations"].append("Off-screen casters are unavailable in this preview.")
        if args.candle_flames is not None:
            manifest["limitations"].append("Candle flames use generated Godot geometry, artistic emissive radiance and offline on/off state; runtime flame state is not observed.")
        legacy.write_pair(args.output, body, manifest)
        print(json.dumps(manifest, indent=2, allow_nan=False))
    except (OSError, ValueError, TypeError, KeyError, IndexError, EOFError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
