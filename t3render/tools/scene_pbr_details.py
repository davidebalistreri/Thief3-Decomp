"""Place generated bedroom details relative to the semantic bed anchor."""
import hashlib
import json
import math
import struct

import scene_ir_to_d3d12_packet as preview
import scene_ir_to_textured_packet as legacy
import scene_pbr_bed as bed
import scene_pbr_flames as gltf
from scene_ir import transform_point

MATERIALS = ("DecorWood", "DecorPages", "DecorLeather")
MAX_VERTICES = 20000
MAX_TRIANGLES = 10000
LOW = (-.5, -.1, -2.3)
HIGH = (5, 4, 3.5)


def select(scene, anchor_id, raw, render_pass, stage):
    scale = scene.conventions.meters_per_unit
    if scale is None or not math.isclose(scale, .01905, rel_tol=0, abs_tol=1e-9):
        raise ValueError("bedroom details require the SDK unit scale")
    resources = {r.id: r for r in scene.static_mesh_resources}
    instances = [i for i in scene.mesh_instances if i.id == anchor_id]
    if (len(instances) != 1 or instances[0].resource_id not in resources or
            resources[instances[0].resource_id].name.lower() != "innbed1a1" or
            not any(s.target_kind == "mesh_instance" and s.target_id == anchor_id and
                    (s.render_pass, s.stage) == (render_pass, stage) for s in scene.submissions)):
        raise ValueError("bedroom details require a submitted INNbed1A1 anchor")
    first = gltf.read_geometry(raw, MATERIALS[0], include_uv=True,
                               max_vertices=MAX_VERTICES, max_indices=MAX_TRIANGLES*3)
    size = struct.unpack_from("<I", raw, 12)[0]
    data = json.loads(raw[20:20+size], object_pairs_hook=preview._pairs, parse_constant=preview._invalid_number)
    materials, primitives = data["materials"], data["meshes"][0]["primitives"]
    names = []
    for primitive in primitives:
        index = primitive.get("material") if isinstance(primitive, dict) else None
        if type(index) is not int or not 0 <= index < len(materials) or not isinstance(materials[index], dict):
            raise ValueError("invalid generated bedroom details material binding")
        names.append(materials[index].get("name"))
    if len(names) != len(MATERIALS) or set(names) != set(MATERIALS):
        raise ValueError("generated bedroom details have missing, duplicate or unsupported materials")
    geometry = {MATERIALS[0]: first}
    for name in MATERIALS[1:]:
        geometry[name] = gltf.read_geometry(raw, name, include_uv=True,
                                             max_vertices=MAX_VERTICES, max_indices=MAX_TRIANGLES*3)
    if (sum(len(g[0]) for g in geometry.values()) > MAX_VERTICES or
            sum(len(g[3])//3 for g in geometry.values()) > MAX_TRIANGLES):
        raise ValueError("generated bedroom details exceed geometry bounds")
    if any(not LOW[k] <= p[k] <= HIGH[k] for g in geometry.values() for p in g[0] for k in range(3)):
        raise ValueError("generated bedroom details exceed the bed-relative region")
    matrix = instances[0].transform
    if any(abs(matrix[i]) > 1e-6 for i in (3,7,11)) or abs(matrix[15]-1) > 1e-6:
        raise ValueError("bedroom details anchor must be affine")
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
                raise ValueError("bedroom detail position exceeds bounds")
            transformed.append((*point[:3], *texcoord, *normal))
        order = [index for at in range(0,len(indices),3) for index in
                 (indices[at:at+3] if mirrored else (indices[at],indices[at+2],indices[at+1]))]
        vertices, fallback = bed._frames([transformed[index] for index in order])
        parts.append({"material": name, "vertices": vertices, "fallback_triangles": fallback})
    return parts, {"anchor_id": anchor_id, "source_sha256": hashlib.sha256(raw).hexdigest(),
                   "source": "offline_generated_godot_visual_addition", "visibility": "not_runtime_observed",
                   "triangles": sum(len(p["vertices"])//3 for p in parts),
                   "fallback_triangles": sum(p["fallback_triangles"] for p in parts)}
