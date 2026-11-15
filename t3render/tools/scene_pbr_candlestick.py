"""Select the Godot candlestick for the bedroom's qualified SceneIR actors."""
import hashlib
import json
import math
import struct

import scene_ir_to_d3d12_packet as preview
import scene_ir_to_textured_packet as legacy
import scene_pbr_bed as bed
import scene_pbr_flames as gltf
from scene_ir import transform_point

MATERIALS = ("METAL1Goodness_D", "CandleWax", "CandleFlame")
ACTORS = frozenset(("D_384__5", "D_384__13"))
MAX_VERTICES = 8192
MAX_TRIANGLES = 10000


def select(scene, raw, render_pass, stage):
    if scene.source.map != "SouthQuarter1_int1" or scene.source.kind != "runtime" or not scene.source.camera_synchronized:
        raise ValueError("candlestick replacement requires the synchronized bedroom scene")
    scale = scene.conventions.meters_per_unit
    if scale is None or not math.isclose(scale, .01905, rel_tol=0, abs_tol=1e-9):
        raise ValueError("candlestick replacement requires the SDK unit scale")
    # Validate the GLB container before inspecting its exact primitive set.
    first = gltf.read_geometry(raw, MATERIALS[0], include_uv=True,
                               max_vertices=MAX_VERTICES, max_indices=MAX_TRIANGLES*3)
    size = struct.unpack_from("<I", raw, 12)[0]
    data = json.loads(raw[20:20+size], object_pairs_hook=preview._pairs, parse_constant=preview._invalid_number)
    materials, primitives = data["materials"], data["meshes"][0]["primitives"]
    names = []
    for primitive in primitives:
        index = primitive.get("material") if isinstance(primitive, dict) else None
        if type(index) is not int or not 0 <= index < len(materials) or not isinstance(materials[index], dict):
            raise ValueError("invalid generated candlestick material binding")
        names.append(materials[index].get("name"))
    if len(names) != len(MATERIALS) or set(names) != set(MATERIALS):
        raise ValueError("generated candlestick has missing, duplicate or unsupported materials")
    geometry = {MATERIALS[0]: first}
    for name in MATERIALS[1:]:
        geometry[name] = gltf.read_geometry(raw, name, include_uv=True,
                                             max_vertices=MAX_VERTICES, max_indices=MAX_TRIANGLES*3)
    if (sum(len(g[0]) for g in geometry.values()) > MAX_VERTICES or
            sum(len(g[3])//3 for g in geometry.values()) > MAX_TRIANGLES):
        raise ValueError("generated candlestick geometry exceeds bounds")
    resources = {r.id: r for r in scene.static_mesh_resources}
    submitted = {s.target_id for s in scene.submissions if s.target_kind == "mesh_instance" and
                 (s.render_pass, s.stage) == (render_pass, stage)}
    prefix = f"actor:{scene.source.map}:{scene.source.fingerprints.get('map')}:"
    result, reports = [], []
    for instance in scene.mesh_instances:
        if instance.id not in submitted or instance.id not in {prefix+name for name in ACTORS}:
            continue
        resource = resources.get(instance.resource_id)
        if resource is None or resource.name.lower() != "genxlight2candles":
            raise ValueError("candlestick actor has an unexpected resource")
        source = [(x*scale, z*scale, y*scale) for x, y, z in resource.geometry.positions]
        replacement = [p for positions, _, _, _ in geometry.values() for p in positions]
        if not source or any(abs(f(source, key=lambda p: p[k])[k] - f(replacement, key=lambda p: p[k])[k]) > .04
                             for k in range(3) for f in (min, max)):
            raise ValueError("generated candlestick bounds do not match the source asset")
        matrix = instance.transform
        if any(abs(matrix[i]) > 1e-6 for i in (3, 7, 11)) or abs(matrix[15]-1) > 1e-6:
            raise ValueError("candlestick transform must be affine")
        normal_matrix = legacy.normal_matrix(matrix)
        a,b,c,d,e,f,g,h,i = (matrix[n] for n in (0,1,2,4,5,6,8,9,10))
        mirrored = a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g) < 0
        for name in MATERIALS[:2]:
            positions, normals, uv, indices = geometry[name]
            transformed = []
            for (x,y,z), (nx,ny,nz), texcoord in zip(positions, normals, uv):
                point = transform_point((x/scale,z/scale,y/scale), matrix)
                normal = legacy.transform_normal((nx,nz,ny), normal_matrix)
                if abs(point[3]-1) > 1e-6 or any(not math.isfinite(v) or abs(v) > 1000000 for v in point[:3]):
                    raise ValueError("candlestick position exceeds bounds")
                transformed.append((*point[:3], *texcoord, *normal))
            order = [index for at in range(0,len(indices),3) for index in
                     (indices[at:at+3] if mirrored else (indices[at],indices[at+2],indices[at+1]))]
            vertices, fallback = bed._frames([transformed[index] for index in order])
            result.append({"target_id": instance.id, "material": name, "vertices": vertices,
                           "fallback_triangles": fallback})
        reports.append({"target_id": instance.id, "resource_id": instance.resource_id,
                        "resource_name": resource.name, "source": "generated_godot_mesh_with_scoped_actor_transform"})
    if not reports:
        raise ValueError("no submitted bedroom candlestick actor found")
    return result, {"source_sha256": hashlib.sha256(raw).hexdigest(), "instances": reports,
                    "triangles": sum(len(p["vertices"])//3 for p in result),
                    "fallback_triangles": sum(p["fallback_triangles"] for p in result)}
