#!/usr/bin/env python3
"""Import a T3SDK map export into a local, offline SceneIR v0 fixture.

The output is the entire offline map, not a captured frame or visibility list.
The game files are read only. The BSP parser discovers blocks heuristically.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "assets"))
sys.path.insert(0, str(ROOT / "t3render" / "ir"))

from ibt import IBT  # noqa: E402
from scene_ir import SceneSnapshot  # noqa: E402
from t3bsp import find_render_blocks  # noqa: E402
from t3common import game_dir, resolve_map, rotator_matrix  # noqa: E402
from t3mesh import TYPE_STATICMESH, kernel_bundle, parse_static_mesh  # noqa: E402
from upkg import Package  # noqa: E402


def file_digest(path, algorithm="sha256"):
    digest = hashlib.new(algorithm)
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _valid_number(value):
    return not isinstance(value, bool) and isinstance(value, (int, float)) and math.isfinite(value)


def _vector(value, size):
    return (isinstance(value, (tuple, list)) and len(value) == size and
            all(_valid_number(v) for v in value))


def actor_transform(actor):
    """T3SDK Unreal rotation convention, with native units and row vectors."""
    location, rotation, scale = actor.get("location"), actor.get("rotation"), actor.get("draw_scale")
    # The JSON record's scalar DrawScale does not encode DrawScale3D. Do not
    # silently interpret a nonuniform scale as a world transform.
    properties = actor.get("properties") or {}
    if not isinstance(properties, dict) or properties.get("DrawScale3D") is not None:
        raise ValueError("actor DrawScale3D is not resolved")
    if not _vector(location, 3) or not isinstance(rotation, (tuple, list)) or len(rotation) != 3 or any(
            type(angle) is not int for angle in rotation) or not _valid_number(scale):
        raise ValueError("actor location, rotation or scale is unknown")
    rows = rotator_matrix(*rotation)
    return [rows[0][0] * scale, rows[0][1] * scale, rows[0][2] * scale, 0,
            rows[1][0] * scale, rows[1][1] * scale, rows[1][2] * scale, 0,
            rows[2][0] * scale, rows[2][1] * scale, rows[2][2] * scale, 0,
            *location, 1]


def light_position(actor, mesh):
    """Resolve a light in native T3 coordinates; never apply actor DrawScale to its hardpoint."""
    location = actor.get("location")
    light = actor.get("light")
    if not _vector(location, 3) or not isinstance(light, dict):
        return None
    offset = light.get("hardpoint_offset")
    if offset is None:
        offset = (0, 0, 0)
    if not _vector(offset, 3):
        return None
    if not actor.get("mesh"):
        return list(location) if not any(offset) else None
    if mesh is None:
        return None
    hardpoints = getattr(mesh, "hardpoints", None)
    if not isinstance(hardpoints, (list, tuple)):
        return None
    matches = [hp for hp in hardpoints if isinstance(getattr(hp, "name", None), str)
               and hp.name.lower() == "hp_lighttarget"]
    rotation = actor.get("rotation")
    if (len(matches) != 1 or not _vector(getattr(matches[0], "position", None), 3) or
            not isinstance(rotation, (list, tuple)) or len(rotation) != 3 or
            any(type(angle) is not int for angle in rotation)):
        return None
    local = [matches[0].position[i] + offset[i] for i in range(3)]
    matrix = rotator_matrix(*rotation)
    position = [location[j] + sum(local[i] * matrix[i][j] for i in range(3)) for j in range(3)]
    return position if all(_valid_number(x) for x in position) else None


def geometry(render, material_ids):
    if len(material_ids) != len(render.sections):
        raise ValueError("section material count does not match geometry")
    return {
        "positions": render.positions,
        "normals": render.normals if len(render.normals) == len(render.positions) else None,
        "uv0": render.uv0 if len(render.uv0) == len(render.positions) else None,
        "tangents": render.tangents if len(render.tangents) == len(render.positions) else None,
        "indices": render.indices,
        "sections": [{"name": section.name, "first_index": section.first_index,
                      "index_count": section.index_count, "material_id": material_id}
                     for section, material_id in zip(render.sections, material_ids)],
    }


def validate_actor_export(export, map_path):
    if not isinstance(export, dict) or not isinstance(export.get("actors"), list):
        raise ValueError("actor export must contain an actors array")
    if (not isinstance(export.get("level"), str) or
            export["level"].lower() != Path(map_path).stem.lower()):
        raise ValueError("actor export and GMP map names do not match")
    if export.get("source_size") != Path(map_path).stat().st_size or export.get("source_sha1") != file_digest(map_path, "sha1"):
        raise ValueError("actor export provenance does not match the GMP bytes")
    if export.get("units") != "unreal (Z up)":
        raise ValueError("actor export has unsupported or missing coordinate units")
    if export.get("actor_count") != len(export["actors"]):
        raise ValueError("actor export count does not match its contents")
    names = [a.get("name") if isinstance(a, dict) else None for a in export["actors"]]
    if any(not isinstance(n, str) or not n for n in names) or len(names) != len(set(names)):
        raise ValueError("actor names must be present and unique")


def make_scene(export, blocks, bundles, fingerprints):
    """Construct the semantic fixture from t3map records and t3mesh/t3bsp data.

    Bundles are ordered (local before kernel), as in t3mesh.ResourceSet. There
    is no visibility filter, camera, game state, implicit skin fallback, or
    attempt to construct transforms for actor attachments.
    """
    level = export["level"]
    prefix = f"{level}:{fingerprints['map']}"
    limitations = [
        "Offline map export: no runtime visibility, save-game changes, frame or camera are represented.",
        "BSP blocks are discovered by the t3bsp heuristic; zone/leaf membership is unknown.",
        "Actor attachments are omitted because parent/bone transforms are not resolved.",
        "Material names and skins are offline references; runtime material bindings are not established.",
        "BF is a legacy hidden material; its visibility is not encoded in SceneIR v0.",
        "Light colors are the approximate t3map conversion; radius and intensity are not physically calibrated.",
        "Mesh-actor light positions use the offline hp_lighttarget and offset, rotated without DrawScale; unresolved positions are null.",
        "The SDK scale of 0.01905 meters per T3 unit is inferred from 16 units per foot in a 4x8 asset; it is not runtime calibrated.",
    ]
    scene = {
        "schema": "t3.scene", "version": 0,
        "source": {"kind": "offline", "map": level, "frame_id": None,
                   "camera_synchronized": None, "fingerprints": fingerprints, "limitations": limitations},
        "conventions": {"length_unit": "t3_unit", "meters_per_unit": 0.01905, "axes": "t3_xyz", "up_axis": "z",
                        "handedness": "left", "winding": "unknown", "uv_origin": "unknown",
                        "tangent_space": "unknown", "matrix_layout": "row_major", "matrix_vectors": "row",
                        "clip_depth": "unknown"},
        "camera": None, "static_mesh_resources": [], "mesh_instances": [], "bsp_blocks": [],
        "materials": [], "lights": [],
    }
    materials = {}
    meshes = {}

    def material_id(name):
        if not isinstance(name, str) or not name:
            return None
        # Match IBT.find's lower(), not Unicode casefold(): the latter can
        # merge distinct Latin-1 resource names such as Straße and Strasse.
        key = name.lower()
        if key not in materials:
            materials[key] = {"id": f"material:{prefix}:{key}", "name": name}
        return materials[key]["id"]

    def lookup(name):
        key = name.lower()
        if key in meshes:
            return meshes[key]
        for bundle in bundles:
            if bundle is not None:
                resource = bundle.find(name, TYPE_STATICMESH)
                if resource:
                    parsed = parse_static_mesh(resource, bundle.split(resource))
                    # The fingerprint describes the actual containing bundle, not only the map bundle.
                    role = "bundle" if bundle is bundles[0] else "kernel"
                    identity = f"mesh:{prefix}:{fingerprints[role]}:{resource.name.lower()}"
                    mesh = {"id": identity, "name": parsed.name,
                            "geometry": geometry(parsed.render, [None] * len(parsed.render.sections))}
                    scene["static_mesh_resources"].append(mesh)
                    meshes[key] = (parsed, mesh)
                    return meshes[key]
        meshes[key] = None
        return None

    counts = Counter()
    for actor in export["actors"]:
        name = actor["name"]
        mesh_name = actor.get("mesh")
        attached = bool(actor.get("attached_to"))
        resolved = None
        if isinstance(mesh_name, str) and mesh_name:
            if attached:
                limitations.append(f"Mesh actor omitted (unresolved attachment): {name} -> {actor['attached_to']}")
                counts["attached_mesh_actors"] += 1
            else:
                resolved = lookup(mesh_name)
                if resolved is None:
                    limitations.append(f"Mesh actor omitted (resource missing): {name} -> {mesh_name}")
                    counts["missing_mesh_actors"] += 1
                else:
                    parsed, resource = resolved
                    try:
                        transform = actor_transform(actor)
                    except ValueError:
                        limitations.append(f"Mesh actor omitted (invalid transform): {name}")
                        counts["invalid_mesh_actors"] += 1
                    else:
                        skin_name = actor.get("skin") or "Default"
                        if not isinstance(skin_name, str):
                            skin_name = None
                        skins = [s for s in parsed.skins if skin_name and s.name.lower() == skin_name.lower()]
                        chosen = skins[0] if len(skins) == 1 else None
                        if not chosen or len(chosen.materials) != len(parsed.render.sections):
                            limitations.append(f"Section materials incomplete (skin missing or mismatched): {name} -> {mesh_name}/{skin_name}")
                            counts["unresolved_skins"] += 1
                        mat_ids = [material_id(chosen.materials[i]) if chosen and i < len(chosen.materials) else None
                                   for i in range(len(parsed.render.sections))]
                        scene["mesh_instances"].append({"id": f"actor:{prefix}:{name}", "resource_id": resource["id"],
                                                        "transform": transform, "skin": skin_name, "material_ids": mat_ids})
        light = actor.get("light")
        if isinstance(light, dict):
            if attached:
                limitations.append(f"Light omitted (unresolved attachment): {name} -> {actor['attached_to']}")
                counts["attached_lights"] += 1
            else:
                color = light.get("color") if _vector(light.get("color"), 3) else None
                enabled = light.get("on") if type(light.get("on")) is bool else None
                kind = light.get("kind") if isinstance(light.get("kind"), str) and light.get("kind") else None
                position = light_position(actor, resolved[0] if resolved else None)
                if position is None:
                    limitations.append(f"Light position unresolved (location, offset or mesh hardpoint): {name}")
                    counts["unresolved_light_positions"] += 1
                scene["lights"].append({"id": f"light:{prefix}:{name}", "kind": kind, "position": position,
                                        "color": color, "radius": None, "intensity": None, "enabled": enabled})

    for index, block in enumerate(blocks):
        scene["bsp_blocks"].append({"id": f"bsp:{prefix}:{index}:{block.offset}",
                                    "block_id": block.block_id, "source_index": index,
                                    "source_offset": block.offset,
                                    "geometry": geometry(block.mesh, [material_id(section.name)
                                                                     for section in block.mesh.sections])})
    scene["materials"] = sorted(materials.values(), key=lambda value: value["id"])
    scene["static_mesh_resources"].sort(key=lambda value: value["id"])
    scene["mesh_instances"].sort(key=lambda value: value["id"])
    scene["lights"].sort(key=lambda value: value["id"])
    return scene, counts


def import_scene(level, actors_path, game):
    gmp = resolve_map(game, level, ".gmp")
    ibt_path = resolve_map(game, level, ".ibt")
    with Path(actors_path).open("r", encoding="utf-8") as f:
        export = json.load(f, parse_constant=lambda value: (_ for _ in ()).throw(ValueError(f"invalid number {value}")))
    validate_actor_export(export, gmp)
    fingerprints = {"map": file_digest(gmp), "bundle": file_digest(ibt_path), "actors": file_digest(actors_path)}
    blocks = find_render_blocks(Package(gmp))
    with IBT(ibt_path) as local:
        kernel = kernel_bundle(ibt_path)
        try:
            if kernel:
                fingerprints["kernel"] = file_digest(kernel.path)
            scene, counts = make_scene(export, blocks, [local, kernel], fingerprints)
        finally:
            if kernel:
                kernel.close()
    return SceneSnapshot.from_dict(scene), counts


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("level", help="map name, for example SouthQuarter1_int1")
    parser.add_argument("--game-dir", help="read-only game installation path or T3_GAME_DIR")
    parser.add_argument("--actors", required=True, help="matching offline t3map.py actors.json")
    parser.add_argument("--output", required=True, help="new local SceneIR JSON output file")
    args = parser.parse_args(argv)
    try:
        snapshot, excluded = import_scene(args.level, args.actors, game_dir(args.game_dir))
        destination = Path(args.output)
        serialized = snapshot.to_json(separators=(",", ":"))
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open("x", encoding="utf-8") as output:
            output.write(serialized)
            output.write("\n")
        print(json.dumps({"output": str(destination), "source": "offline", "map": snapshot.source.map,
                          "mesh_resources": len(snapshot.static_mesh_resources), "mesh_instances": len(snapshot.mesh_instances),
                          "bsp_blocks": len(snapshot.bsp_blocks), "materials": len(snapshot.materials),
                          "lights": len(snapshot.lights), "exclusions": dict(excluded)}, indent=2))
        return 0
    except (OSError, ValueError, KeyError, TypeError, IndexError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
