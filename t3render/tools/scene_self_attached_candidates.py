"""Resolve self-attached actor assets offline without inventing a world transform.

The runtime bridge must separately prove ownership and supply the transform.
Only fingerprinted actor and bundle data are used; resource bindings remain
candidates and the original offline SceneIR is not changed.
"""
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path

import analyze_scene_bridge as bridge
import import_t3_scene as importer
import scene_ir_to_d3d12_packet as preview
from ibt import IBT
from scene_ir import StaticMeshResource
from t3common import resolve_map
from t3mesh import TYPE_STATICMESH, parse_static_mesh


def load(scene, records, actors_path, game):
    if scene.source.kind != "offline" or actors_path is None or game is None:
        raise ValueError("self-attached candidates require an offline scene, actor export and game directory")
    raw = preview.read_limited(actors_path, preview.MAX_INPUT)
    if hashlib.sha256(raw).hexdigest() != scene.source.fingerprints.get("actors"):
        raise ValueError("self-attached candidate actor fingerprint mismatch")
    export = json.loads(raw, object_pairs_hook=preview._pairs, parse_constant=preview._invalid_number)
    actors = export.get("actors") if isinstance(export, dict) else None
    if (not isinstance(actors, list) or export.get("actor_count") != len(actors) or
            not isinstance(export.get("level"), str) or export["level"].lower() != scene.source.map.lower() or
            any(not isinstance(a, dict) or not isinstance(a.get("name"), str) or not a["name"] for a in actors)):
        raise ValueError("invalid self-attached candidate actor export")
    actors_by_name = {a["name"]: a for a in actors}
    if len(actors_by_name) != len(actors):
        raise ValueError("self-attached candidate actor names are ambiguous")
    observed = {bridge.fullname(e.get("owner_name"), e.get("owner_fname"))
                for row in records if row.get("event") == "scene_bridge_observation"
                for e in row.get("entries", []) if isinstance(e, dict)}
    existing = {x.id.rsplit(":", 1)[-1] for x in scene.mesh_instances}
    selected = [actor for name, actor in actors_by_name.items() if name in observed and name not in existing and
                actor.get("attached_to") == name]
    if not selected:
        return {}
    map_digest = scene.source.fingerprints.get("map")
    if not map_digest:
        raise ValueError("self-attached candidate map fingerprint missing")
    local = resolve_map(Path(game), scene.source.map, ".ibt")
    paths = [("bundle", local)]
    if "kernel" in scene.source.fingerprints:
        paths.append(("kernel", local.parent / "Kernel_GFXALL.ibt"))
    with ExitStack() as stack:
        bundles = []
        for role, path in paths:
            data = preview.read_limited(path, preview.MAX_INPUT)
            digest = hashlib.sha256(data).hexdigest()
            if digest != scene.source.fingerprints.get(role):
                raise ValueError(f"self-attached candidate {role} fingerprint mismatch")
            bundles.append((role, stack.enter_context(IBT(path, data=data))))
        result = {}
        prefix = f"{scene.source.map}:{map_digest}"
        for actor in selected:
            name, mesh_name, skin_name = actor["name"], actor.get("mesh"), actor.get("skin") or "Default"
            if not isinstance(mesh_name, str) or not mesh_name or not isinstance(skin_name, str) or not skin_name:
                raise ValueError("self-attached candidate lacks a mesh or skin")
            chosen = None
            for role, bundle in bundles:
                matches = [r for r in bundle.of_type(TYPE_STATICMESH) if r.name.lower() == mesh_name.lower()]
                if len(matches) > 1:
                    raise ValueError("self-attached candidate mesh is ambiguous")
                if matches:
                    chosen = role, bundle, matches[0]
                    break
            if chosen is None:
                raise ValueError("self-attached candidate mesh is missing")
            role, bundle, resource = chosen
            mesh = parse_static_mesh(resource, bundle.split(resource))
            if not isinstance(mesh.name, str) or mesh.name.lower() != mesh_name.lower():
                raise ValueError("self-attached candidate mesh name disagrees with the export")
            skins = [skin for skin in mesh.skins if skin.name.lower() == skin_name.lower()]
            if len(skins) != 1 or len(skins[0].materials) != len(mesh.render.sections) or any(
                    not isinstance(mat, str) or not mat for mat in skins[0].materials):
                raise ValueError("self-attached candidate skin is missing or ambiguous")
            resource_id = f"mesh:{prefix}:{scene.source.fingerprints[role]}:{resource.name.lower()}"
            parsed = StaticMeshResource.parse({"id": resource_id, "name": mesh.name,
                     "geometry": importer.geometry(mesh.render, [None] * len(mesh.render.sections))}, "candidate.resource")
            material_ids = tuple(f"material:{prefix}:{mat.lower()}" for mat in skins[0].materials)
            materials = {key: mat for key, mat in zip(material_ids, skins[0].materials)}
            result[name] = {"target_id": f"actor:{prefix}:{name}", "resource": parsed,
                            "material_ids": material_ids, "materials": materials}
        return result
