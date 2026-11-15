"""Select explicit offline static caster candidates for a scoped PBR frame.

These candidates are not runtime visibility or shadow-state observations.
"""
from collections import Counter
import hashlib
import math
from pathlib import Path
import sys

import scene_ir_to_d3d12_packet as preview
from scene_ir import SceneSnapshot, transform_point
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "assets"))
from t3mesh import HIDDEN_MATERIALS  # noqa: E402

MAX_CASTERS = 8192
MAX_TRIANGLES = 100000


def select(scene, offline, offline_raw, actors, resolve_diffuse, lights, shadow, owners, render_pass, stage,
           replaced_instances=()):
    if (not isinstance(offline_raw, bytes) or
            hashlib.sha256(offline_raw).hexdigest() != scene.source.fingerprints.get("offline_scene") or
            SceneSnapshot.from_json(offline_raw) != offline or
            offline.source.kind != "offline" or offline.source.map != scene.source.map or
            offline.conventions != scene.conventions or any(
                scene.source.fingerprints.get(key) != value for key, value in offline.source.fingerprints.items())):
        raise ValueError("offline caster fixture does not match the runtime scene")
    if not isinstance(actors, dict) or not callable(resolve_diffuse):
        raise ValueError("offline caster selection requires actors and material resolution")
    seen = {(s.target_kind, s.target_id, s.section) for s in scene.submissions
            if (s.render_pass, s.stage) == (render_pass, stage)}
    runtime_instances = {item.id: item for item in scene.mesh_instances}
    runtime_meshes = {item.id: item for item in scene.static_mesh_resources}
    runtime_blocks = {item.id: item for item in scene.bsp_blocks}
    offline_meshes = {item.id: item for item in offline.static_mesh_resources}
    materials = {item.id: item.name for item in offline.materials}
    opacity, casters, omitted, counts = {}, [], [], Counter()
    radius_squared = (shadow["far_m"] / scene.conventions.meters_per_unit)**2
    triangle_total = 0

    def omit(kind, identity, section, reason):
        counts[reason] += 1
        if reason != "outside_light_range":
            omitted.append({"target_kind": kind, "target_id": identity, "section": section, "reason": reason})

    def is_opaque(name):
        key = name.lower()
        if key not in opacity:
            try:
                texture = resolve_diffuse(name)
                opacity[key] = ("opaque" if all(mip[3::4].count(255) == len(mip)//4 for mip in texture["mips"])
                                else "nonopaque_offline_diffuse")
            except (ValueError, EOFError) as exc:
                message = str(exc)
                if "exceed" in message.lower() or "ambiguous" in message.lower():
                    raise
                opacity[key] = "offline_diffuse_unavailable"
        return opacity[key]

    def add_sections(kind, identity, geometry, matrix, bindings, source, actor_name=None):
        nonlocal triangle_total
        if any(abs(matrix[i]) > 1e-6 for i in (3, 7, 11)) or abs(matrix[15]-1) > 1e-6:
            raise ValueError("offline shadow caster requires an affine transform")
        for section_index, section in enumerate(geometry.sections):
            if (kind, identity, section_index) in seen:
                omit(kind, identity, section_index, "already_submitted")
                continue
            indices = geometry.indices[section.first_index:section.first_index+section.index_count]
            if not indices:
                omit(kind, identity, section_index, "empty_section")
                continue
            name = materials.get(bindings[section_index] if bindings else section.material_id)
            if not name:
                omit(kind, identity, section_index, "missing_material")
                continue
            if name.lower() in HIDDEN_MATERIALS:
                omit(kind, identity, section_index, "hidden_material")
                continue
            points = []
            for index in indices:
                point = transform_point(geometry.positions[index], matrix)
                if (abs(point[3]-1) > 1e-6 or any(not math.isfinite(x) or abs(x) > 1000000 for x in point[:3])):
                    raise ValueError("offline shadow caster position exceeds bounds")
                points.append(tuple(point[:3]))
            bounds = [(min(p[axis] for p in points), max(p[axis] for p in points)) for axis in range(3)]
            mask = 0
            for index, light in enumerate(lights):
                if actor_name is not None and actor_name in owners[index]:
                    continue
                distance_squared = sum(max(low-x, 0, x-high)**2 for x, (low, high) in zip(light["position"], bounds))
                if distance_squared <= radius_squared:
                    mask |= 1 << index
            if not mask:
                omit(kind, identity, section_index, "outside_light_range")
                continue
            status = is_opaque(name)
            if status != "opaque":
                omit(kind, identity, section_index, status)
                continue
            triangle_total += len(points)//3
            if triangle_total > MAX_TRIANGLES or len(casters) >= MAX_CASTERS:
                raise ValueError("offline shadow caster geometry exceeds bounds")
            casters.append({"target_kind": kind, "target_id": identity, "section": section_index,
                            "source": source, "material": name, "vertices": points, "shadow_mask": mask})
            counts["selected"] += 1

    for block in offline.bsp_blocks:
        current = runtime_blocks.get(block.id)
        if current is not None and current != block:
            raise ValueError("offline BSP caster differs from the scoped block")
        add_sections("bsp_block", block.id, block.geometry, preview.IDENTITY, (), "offline_bsp")
    for instance in offline.mesh_instances:
        if instance.id in replaced_instances:
            counts["replaced_by_generated_mesh"] += len(offline_meshes[instance.resource_id].geometry.sections)
            continue
        name = instance.id.rsplit(":", 1)[-1]
        actor = actors.get(name)
        if not isinstance(actor, dict) or actor.get("base") != "StaticMeshActor":
            counts["not_static_mesh_actor"] += 1
            continue
        resource = offline_meshes[instance.resource_id]
        if (type(actor.get("casts_shadows")) is not bool or not isinstance(actor.get("mesh"), str) or
                actor["mesh"].lower() != resource.name.lower()):
            raise ValueError("offline static caster lacks a matching mesh or cast flag")
        if not actor["casts_shadows"] or actor.get("attached_to") is not None:
            counts["disabled_or_attached_actor"] += 1
            continue
        current = runtime_instances.get(instance.id)
        if current is not None:
            if (current.resource_id != instance.resource_id or current.resource_id not in runtime_meshes or
                    runtime_meshes[current.resource_id] != resource):
                raise ValueError("offline static caster differs from the scoped mesh")
            geometry, matrix, bindings, source = (runtime_meshes[current.resource_id].geometry, current.transform,
                                                   current.material_ids, "offline_section_runtime_transform")
        else:
            geometry, matrix, bindings, source = resource.geometry, instance.transform, instance.material_ids, "offline_static_actor"
        add_sections("mesh_instance", instance.id, geometry, matrix, bindings, source, name)
    return casters, {"selected_sections": len(casters), "selected_triangles": triangle_total,
                     "counts": dict(sorted(counts.items())), "omitted": omitted}
