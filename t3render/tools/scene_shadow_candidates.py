#!/usr/bin/env python3
"""Extract SDK shadow volumes for revalidated same-frame static SceneIR casters.

The output is local candidate geometry for a future renderer, not an observed
runtime shadow list. No process addresses or graphics API objects are emitted.
"""
import argparse
from contextlib import ExitStack
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

import import_t3_scene as importer
import scene_direct_candidates as direct
import scene_ir_to_d3d12_packet as preview
import scene_legacy_states as states
from ibt import IBT
from scene_ir import Geometry, SceneSnapshot
from t3bsp import find_render_blocks
from t3common import game_dir, resolve_map
from t3mesh import TYPE_STATICMESH, parse_static_mesh
from upkg import Package

MAX_VERTICES = 100000
MAX_TRIANGLES = 200000
MAX_CASTERS = 8192


def volume(render):
    """Validate the asset's local-space face normals and volume indices."""
    positions, normals, indices = render.shadow_positions, render.shadow_normals, render.shadow_indices
    if not positions and not normals and not indices:
        return None
    if (not positions or len(positions) != len(normals) or len(positions) > MAX_VERTICES or
            not indices or len(indices) % 3 or len(indices) // 3 > MAX_TRIANGLES or
            any(not isinstance(v, (list, tuple)) or len(v) != 3 or any(
                isinstance(x, bool) or not isinstance(x, (int, float)) or abs(x) > 1000000 or not math.isfinite(x)
                for x in v) for v in positions) or
            any(not isinstance(v, (list, tuple)) or len(v) != 3 or any(
                isinstance(x, bool) or not isinstance(x, (int, float)) or abs(x) > 2 or not math.isfinite(x)
                for x in v) for v in normals) or
            any(type(index) is not int or not 0 <= index < len(positions) for index in indices)):
        raise ValueError("invalid or oversized offline shadow volume")
    return {"positions": positions, "face_normals": normals, "indices": indices}


def affine(matrix):
    """Reject transforms which cannot map caster positions and face normals."""
    if len(matrix) != 16 or any(abs(matrix[i]) > 1e-6 for i in (3, 7, 11)) or abs(matrix[15] - 1) > 1e-6:
        raise ValueError("shadow caster requires an affine runtime transform")
    a, b, c, d, e, f, g, h, i = (matrix[n] for n in (0, 1, 2, 4, 5, 6, 8, 9, 10))
    det = a*(e*i-f*h) - b*(d*i-f*g) + c*(d*h-e*g)
    if not math.isfinite(det) or abs(det) < 1e-12:
        raise ValueError("shadow caster transform is singular")
    return det < 0


def assemble(scene, actors, blocks, bundles):
    """Map revalidated scene geometry to its fingerprinted SDK volumes."""
    meshes = {m.id: m for m in scene.static_mesh_resources}
    resources, casters, omitted = {}, [], []
    prefix = f"{scene.source.map}:{scene.source.fingerprints['map']}"
    for instance in scene.mesh_instances:
        actor_name = instance.id.rsplit(":", 1)[-1]
        actor = actors.get(actor_name)
        mesh = meshes[instance.resource_id]
        if (not isinstance(actor, dict) or type(actor.get("casts_shadows")) is not bool or
                not isinstance(actor.get("mesh"), str) or actor["mesh"].lower() != mesh.name.lower()):
            raise ValueError("shadow candidate actor or offline mesh binding is inconsistent")
        if not actor["casts_shadows"]:
            omitted.append({"target_id": instance.id, "reason": "offline_actor_disabled"})
            continue
        reflected = affine(instance.transform)
        shadow_id = "shadow:" + mesh.id
        if shadow_id not in resources:
            selected = None
            for role, bundle in bundles:
                matches = [r for r in bundle.of_type(TYPE_STATICMESH) if r.name.lower() == mesh.name.lower()]
                if len(matches) > 1:
                    raise ValueError("ambiguous offline mesh for shadow candidate")
                if matches:
                    selected = role, bundle, matches[0]
                    break
            if selected is None:
                raise ValueError("offline shadow candidate mesh is missing")
            role, bundle, resource = selected
            expected_id = f"mesh:{prefix}:{scene.source.fingerprints[role]}:{resource.name.lower()}"
            if mesh.id != expected_id:
                raise ValueError("shadow candidate mesh resource identity differs")
            parsed = parse_static_mesh(resource, bundle.split(resource))
            geometry = Geometry.parse(importer.geometry(parsed.render, [None] * len(parsed.render.sections)), "shadow.mesh")
            if parsed.name.lower() != mesh.name.lower() or geometry != mesh.geometry:
                raise ValueError("shadow candidate mesh geometry differs from the scene")
            found = volume(parsed.render)
            if found is not None:
                resources[shadow_id] = {"id": shadow_id, "source_kind": "static_mesh", **found}
        if shadow_id not in resources:
            omitted.append({"target_id": instance.id, "reason": "offline_volume_missing"})
            continue
        casters.append({"target_kind": "mesh_instance", "target_id": instance.id, "resource_id": shadow_id,
                        "transform": list(instance.transform), "reflected": reflected})
    for block in scene.bsp_blocks:
        if block.source_index >= len(blocks):
            raise ValueError("shadow candidate BSP index is outside the source map")
        source = blocks[block.source_index]
        if source.offset != block.source_offset or source.block_id != block.block_id:
            raise ValueError("shadow candidate BSP identity differs from the scene")
        geometry = Geometry.parse(importer.geometry(source.mesh, [s.material_id for s in block.geometry.sections]), "shadow.bsp")
        if geometry != block.geometry:
            raise ValueError("shadow candidate BSP geometry differs from the scene")
        found = volume(source.mesh)
        if found is None:
            omitted.append({"target_id": block.id, "reason": "offline_volume_missing"})
            continue
        shadow_id = "shadow:" + block.id
        resources[shadow_id] = {"id": shadow_id, "source_kind": "bsp", **found}
        casters.append({"target_kind": "bsp_block", "target_id": block.id, "resource_id": shadow_id,
                        "transform": list(preview.IDENTITY), "reflected": False})
    if not casters:
        raise ValueError("no selected SDK shadow-volume candidates")
    if (len(casters) > MAX_CASTERS or sum(len(resources[c["resource_id"]]["indices"]) // 3 for c in casters)
            > MAX_TRIANGLES):
        raise ValueError("expanded shadow candidates exceed renderer bounds")
    return [resources[k] for k in sorted(resources)], casters, omitted


def extract(scene, scene_bytes, offline, offline_bytes, trace, actor_path, game, render_pass, stage):
    """Revalidate the scoped scene and load only bytes matching its fingerprints."""
    if (SceneSnapshot.from_json(scene_bytes) != scene or SceneSnapshot.from_json(offline_bytes) != offline):
        raise ValueError("shadow candidate scene bytes differ from the parsed scenes")
    states.from_trace(scene, offline, offline_bytes, trace, render_pass, stage,
                      self_attached_actors=actor_path, self_attached_game=game,
                      light_actors=actor_path, light_game=game)
    actors = direct.offline_actors(scene, offline, offline_bytes, actor_path)
    path = resolve_map(Path(game), scene.source.map, ".gmp")
    raw = preview.read_limited(path, preview.MAX_INPUT)
    if hashlib.sha256(raw).hexdigest() != scene.source.fingerprints.get("map"):
        raise ValueError("shadow candidate map fingerprint mismatch")
    blocks = find_render_blocks(Package(path, data=raw))
    local = resolve_map(Path(game), scene.source.map, ".ibt")
    paths = [("bundle", local)]
    if "kernel" in scene.source.fingerprints:
        paths.append(("kernel", local.parent / "Kernel_GFXALL.ibt"))
    with ExitStack() as stack:
        bundles = []
        for role, path in paths:
            raw = preview.read_limited(path, preview.MAX_INPUT)
            if hashlib.sha256(raw).hexdigest() != scene.source.fingerprints.get(role):
                raise ValueError(f"shadow candidate {role} fingerprint mismatch")
            bundles.append((role, stack.enter_context(IBT(path, data=raw))))
        resources, casters, omitted = assemble(scene, actors, blocks, bundles)
    return {"schema": "t3.scene.shadow-candidates", "version": 1, "frame": scene.source.frame_id,
            "pass": render_pass, "stage": stage, "scene_sha256": hashlib.sha256(scene_bytes).hexdigest(),
            "fingerprints": scene.source.fingerprints, "conventions": scene.to_dict()["conventions"],
            "resources": resources, "casters": casters, "omitted": omitted,
            "limitations": ["Caster list is limited to selected scoped scene objects; off-screen casters may be missing.",
                            "Shadow volumes, cast flags and mesh bindings are fingerprinted offline candidates, not observed runtime shadow submissions.",
                            "BSP blocks are selected by the heuristic SDK scanner; its source indices do not prove runtime payload identity.",
                            "Mesh transforms are from the synchronized runtime snapshot; BSP data is in world space."]}


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
        result = extract(scene, raw, offline, offline_bytes, args.trace, args.offline_actors,
                         game_dir(args.game_dir), args.render_pass, args.stage)
        body = json.dumps(result, indent=2, allow_nan=False) + "\n"
        if len(body.encode()) > preview.MAX_INPUT:
            raise ValueError("shadow candidate output exceeds bounds")
        summary = {"schema": "t3.scene.shadow-candidate-evidence", "version": 1,
                   "frame": result["frame"], "resources": len(result["resources"]),
                   "casters": len(result["casters"]), "omitted": len(result["omitted"]),
                   "sha256": hashlib.sha256(body.encode()).hexdigest()}
        preview.write_pair(args.output, body, summary)
        print(json.dumps(summary))
    except (OSError, ValueError, TypeError, KeyError, IndexError, EOFError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
