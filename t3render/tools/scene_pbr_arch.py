"""Replace only the bedroom arch rosette and add its generated jambs."""
import hashlib
import json
import math
import struct

import scene_ir_to_d3d12_packet as preview
import scene_ir_to_textured_packet as legacy
import scene_pbr_bed as bed
import scene_pbr_flames as gltf
from scene_ir import transform_point

MATERIALS = ("CIVGoodness_D", "STMwall512D_D")
MAX_VERTICES = 8192
MAX_TRIANGLES = 10000


def select(scene, raw, render_pass, stage):
    if scene.source.map != "SouthQuarter1_int1" or scene.source.kind != "runtime" or not scene.source.camera_synchronized:
        raise ValueError("arch replacement requires the synchronized bedroom scene")
    scale = scene.conventions.meters_per_unit
    if scale is None or not math.isclose(scale, .01905, rel_tol=0, abs_tol=1e-9):
        raise ValueError("arch replacement requires the SDK unit scale")
    identity = f"actor:{scene.source.map}:{scene.source.fingerprints.get('map')}:StaticMeshActor__59"
    candidates = [i for i in scene.mesh_instances if i.id == identity]
    resources = {r.id: r for r in scene.static_mesh_resources}
    if len(candidates) != 1 or candidates[0].resource_id not in resources:
        raise ValueError("arch replacement requires the qualified bedroom actor")
    instance, resource = candidates[0], resources[candidates[0].resource_id]
    if resource.name.lower() != "genskndoorfrmarc4x10half":
        raise ValueError("arch actor has an unexpected resource")
    materials = {m.id: m.name for m in scene.materials}
    section_names = [materials.get(instance.material_ids[n] if instance.material_ids else section.material_id)
                     for n, section in enumerate(resource.geometry.sections)]
    if len(section_names) != 2 or set(section_names) != set(MATERIALS):
        raise ValueError("arch source requires distinct rosette and stone sections")
    section_ids = {name: section_names.index(name) for name in MATERIALS}
    submitted = {s.section for s in scene.submissions if s.target_kind == "mesh_instance" and
                 s.target_id == identity and (s.render_pass, s.stage) == (render_pass, stage)}
    if not set(section_ids.values()) <= submitted:
        raise ValueError("arch replacement requires both source sections in the selected scope")
    first = gltf.read_geometry(raw, MATERIALS[0], include_uv=True,
                               max_vertices=MAX_VERTICES, max_indices=MAX_TRIANGLES*3)
    size = struct.unpack_from("<I", raw, 12)[0]
    data = json.loads(raw[20:20+size], object_pairs_hook=preview._pairs, parse_constant=preview._invalid_number)
    glb_materials, primitives = data["materials"], data["meshes"][0]["primitives"]
    names = []
    for primitive in primitives:
        index = primitive.get("material") if isinstance(primitive, dict) else None
        if type(index) is not int or not 0 <= index < len(glb_materials) or not isinstance(glb_materials[index], dict):
            raise ValueError("invalid generated arch material binding")
        names.append(glb_materials[index].get("name"))
    if len(names) != len(MATERIALS) or set(names) != set(MATERIALS):
        raise ValueError("generated arch has missing, duplicate or unsupported materials")
    geometry = {MATERIALS[0]: first, MATERIALS[1]: gltf.read_geometry(raw, MATERIALS[1], include_uv=True,
                         max_vertices=MAX_VERTICES, max_indices=MAX_TRIANGLES*3)}
    if (sum(len(g[0]) for g in geometry.values()) > MAX_VERTICES or
            sum(len(g[3])//3 for g in geometry.values()) > MAX_TRIANGLES):
        raise ValueError("generated arch geometry exceeds bounds")
    for name in MATERIALS:
        section = resource.geometry.sections[section_ids[name]]
        indices = resource.geometry.indices[section.first_index:section.first_index+section.index_count]
        source = [(x*scale,z*scale,y*scale) for x,y,z in (resource.geometry.positions[i] for i in indices)]
        replacement = geometry[name][0]
        if not source:
            raise ValueError("arch source section has no geometry")
        for axis in range(3):
            low, high = min(p[axis] for p in source), max(p[axis] for p in source)
            gen_low, gen_high = min(p[axis] for p in replacement), max(p[axis] for p in replacement)
            if ((name == MATERIALS[0] and (abs(low-gen_low) > .04 or abs(high-gen_high) > .04)) or
                    (name == MATERIALS[1] and (gen_low < low-.04 or gen_high > high+.04))):
                raise ValueError("generated arch bounds do not match the source sections")
    matrix = instance.transform
    if any(abs(matrix[i]) > 1e-6 for i in (3,7,11)) or abs(matrix[15]-1) > 1e-6:
        raise ValueError("arch transform must be affine")
    normal_matrix = legacy.normal_matrix(matrix)
    a,b,c,d,e,f,g,h,i = (matrix[n] for n in (0,1,2,4,5,6,8,9,10))
    mirrored = a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g) < 0
    parts = []
    for name in MATERIALS:
        positions, normals, uv, indices = geometry[name]
        transformed = []
        for (x,y,z), (nx,ny,nz), texcoord in zip(positions, normals, uv):
            point = transform_point((x/scale,z/scale,y/scale), matrix)
            normal = legacy.transform_normal((nx,nz,ny), normal_matrix)
            if abs(point[3]-1) > 1e-6 or any(not math.isfinite(v) or abs(v) > 1000000 for v in point[:3]):
                raise ValueError("arch position exceeds bounds")
            transformed.append((*point[:3], *texcoord, *normal))
        order = [index for at in range(0,len(indices),3) for index in
                 (indices[at:at+3] if mirrored else (indices[at],indices[at+2],indices[at+1]))]
        vertices, fallback = bed._frames([transformed[index] for index in order])
        parts.append({"material": name, "vertices": vertices, "fallback_triangles": fallback})
    return parts, {"target_id": identity, "resource_id": resource.id, "resource_name": resource.name,
                   "replaced_section": section_ids[MATERIALS[0]], "preserved_section": section_ids[MATERIALS[1]],
                   "source_sha256": hashlib.sha256(raw).hexdigest(),
                   "triangles": sum(len(p["vertices"])//3 for p in parts),
                   "fallback_triangles": sum(p["fallback_triangles"] for p in parts),
                   "source": "generated_godot_mesh_with_scoped_actor_transform"}
