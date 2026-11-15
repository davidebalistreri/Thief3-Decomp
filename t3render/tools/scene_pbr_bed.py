"""Select the generated Godot bed by its observed semantic SceneIR actor."""
import hashlib
import json
import math
import struct

import scene_ir_to_d3d12_packet as preview
import scene_ir_to_textured_packet as legacy
import scene_pbr_flames as generated
from scene_ir import transform_point

MATERIALS = ("BedTrimWood", "HeadboardWood", "GENiFABRICwoolA2_D", "BlanketSeam", "PillowLinen", "PillowSeam")
MAX_TRIANGLES = 50000
MAX_VERTICES = 50000


def _frames(vertices):
    def dot(a, b): return sum(x*y for x, y in zip(a, b))
    def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
    def normalized(v):
        length = math.sqrt(dot(v, v))
        return tuple(x/length for x in v) if math.isfinite(length) and length > 1e-10 else None
    result, fallback = [], 0
    for at in range(0, len(vertices), 3):
        tri = vertices[at:at+3]
        e1 = [tri[1][i]-tri[0][i] for i in range(3)]
        e2 = [tri[2][i]-tri[0][i] for i in range(3)]
        du1, dv1 = tri[1][3]-tri[0][3], tri[1][4]-tri[0][4]
        du2, dv2 = tri[2][3]-tri[0][3], tri[2][4]-tri[0][4]
        determinant = du1*dv2-dv1*du2
        tangent = bitangent = None
        if abs(determinant) > 1e-10:
            tangent = [(e1[i]*dv2-e2[i]*dv1)/determinant for i in range(3)]
            bitangent = normalized([(e2[i]*du1-e1[i]*du2)/determinant for i in range(3)])
        frames, signs = [], []
        for vertex in tri:
            n = vertex[5:8]
            direction = normalized([tangent[i]-dot(tangent,n)*n[i] for i in range(3)]) if tangent is not None else None
            alignment = dot(cross(n,direction), bitangent) if direction is not None and bitangent is not None else 0
            frames.append(direction)
            signs.append(1 if alignment > 1e-4 else -1 if alignment < -1e-4 else 0)
        if 0 in signs or len(set(signs)) != 1:
            result.extend((*vertex, 0, 0, 0, 0) for vertex in tri)
            fallback += 1
        else:
            result.extend((*vertex, *direction, sign) for vertex, direction, sign in zip(tri, frames, signs))
    return result, fallback


def _primitives(glb):
    # A first bounded read validates the container before inspecting names.
    first = generated.read_geometry(glb, MATERIALS[0], include_uv=True, max_vertices=MAX_VERTICES, max_indices=MAX_TRIANGLES*3)
    json_size = struct.unpack_from("<I", glb, 12)[0]
    data = json.loads(glb[20:20+json_size], object_pairs_hook=preview._pairs, parse_constant=preview._invalid_number)
    materials, primitives = data["materials"], data["meshes"][0]["primitives"]
    names = []
    for primitive in primitives:
        if (not isinstance(primitive, dict) or type(primitive.get("material")) is not int or
                not 0 <= primitive["material"] < len(materials) or not isinstance(materials[primitive["material"]], dict)):
            raise ValueError("invalid generated bed material binding")
        names.append(materials[primitive["material"]].get("name"))
    if len(names) != len(MATERIALS) or set(names) != set(MATERIALS):
        raise ValueError("generated bed has missing, duplicate or unsupported materials")
    geometry = {MATERIALS[0]: first}
    for name in MATERIALS[1:]:
        geometry[name] = generated.read_geometry(glb, name, include_uv=True, max_vertices=MAX_VERTICES, max_indices=MAX_TRIANGLES*3)
    triangles = sum(len(item[3])//3 for item in geometry.values())
    if not 1 <= triangles <= MAX_TRIANGLES or sum(len(item[0]) for item in geometry.values()) > MAX_VERTICES:
        raise ValueError("generated bed geometry exceeds bounds")
    return geometry


def select(scene, anchor_id, glb, render_pass, stage):
    if scene.conventions.meters_per_unit is None or not math.isclose(scene.conventions.meters_per_unit, .01905, rel_tol=0, abs_tol=1e-9):
        raise ValueError("generated bed requires the SDK unit scale")
    instances = [item for item in scene.mesh_instances if item.id == anchor_id]
    meshes = {item.id: item for item in scene.static_mesh_resources}
    if (len(instances) != 1 or instances[0].resource_id not in meshes or
            meshes[instances[0].resource_id].name.lower() != "innbed1a1" or
            not any(s.target_kind == "mesh_instance" and s.target_id == anchor_id and
                    (s.render_pass, s.stage) == (render_pass, stage) for s in scene.submissions)):
        raise ValueError("generated bed requires a submitted INNbed1A1 semantic anchor")
    instance, mesh = instances[0], meshes[instances[0].resource_id]
    matrix, scale = instance.transform, scene.conventions.meters_per_unit
    if any(abs(matrix[i]) > 1e-6 for i in (3,7,11)) or abs(matrix[15]-1) > 1e-6:
        raise ValueError("generated bed transform must be affine")
    normal_matrix = legacy.normal_matrix(matrix)
    a,b,c,d,e,f,g,h,i = (matrix[n] for n in (0,1,2,4,5,6,8,9,10))
    mirrored = a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g) < 0
    geometry = _primitives(glb)
    source = [(x*scale,z*scale,y*scale) for x,y,z in mesh.geometry.positions]
    replacement = [p for positions, _, _, _ in geometry.values() for p in positions]
    if not source or any(abs(f(source, key=lambda p:p[k])[k]-f(replacement, key=lambda p:p[k])[k]) > .3
                         for k in range(3) for f in (min,max)):
        raise ValueError("generated bed bounds do not match the source asset")
    result = []
    for name in MATERIALS:
        positions, normals, uv, indices = geometry[name]
        transformed = []
        for point, normal, texcoord in zip(positions, normals, uv):
            x,y,z = point
            position = transform_point((x/scale,z/scale,y/scale), matrix)
            nx,ny,nz = normal
            normal = legacy.transform_normal((nx,nz,ny), normal_matrix)
            if abs(position[3]-1) > 1e-6 or any(not math.isfinite(x) or abs(x) > 1000000 for x in position[:3]):
                raise ValueError("generated bed position exceeds bounds")
            transformed.append((*position[:3], *texcoord, *normal))
        order = [index for at in range(0,len(indices),3) for index in
                 (indices[at:at+3] if mirrored else (indices[at],indices[at+2],indices[at+1]))]
        vertices, fallback = _frames([transformed[index] for index in order])
        result.append({"material": name, "vertices": vertices, "fallback_triangles": fallback})
    return result, {"target_id": anchor_id, "resource_id": instance.resource_id, "resource_name": mesh.name,
                    "source_sha256": hashlib.sha256(glb).hexdigest(), "triangles": sum(len(x["vertices"])//3 for x in result),
                    "fallback_triangles": sum(x["fallback_triangles"] for x in result),
                    "source": "generated_godot_mesh_with_observed_actor_transform"}
