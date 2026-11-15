"""Read the generated Godot candle flames as bounded semantic preview geometry."""
import hashlib
import json
import math
import struct

import scene_ir_to_d3d12_packet as preview
import scene_ir_to_textured_packet as legacy
from scene_ir import transform_point

MAX_GLB_BYTES = 8 * 1024 * 1024
MAX_VERTICES = 8192
MAX_INDICES = 30000
MAX_FLAME_INSTANCES = 16
MAX_FLAME_TRIANGLES = 10000


def read_geometry(raw, material_name="CandleFlame", include_uv=False, max_vertices=MAX_VERTICES, max_indices=MAX_INDICES):
    """Read a named, bounded triangle primitive from a generated local GLB."""
    if not isinstance(raw, bytes) or len(raw) > MAX_GLB_BYTES or len(raw) < 28:
        raise ValueError("invalid or oversized generated GLB")
    magic, version, size = struct.unpack_from("<4sII", raw)
    if (magic, version, size) != (b"glTF", 2, len(raw)):
        raise ValueError("invalid generated GLB header")
    json_size, json_type = struct.unpack_from("<II", raw, 12)
    offset = 20 + json_size
    if json_type != 0x4e4f534a or json_size % 4 or offset + 8 > len(raw):
        raise ValueError("invalid generated GLB JSON chunk")
    bin_size, bin_type = struct.unpack_from("<II", raw, offset)
    if bin_type != 0x004e4942 or bin_size % 4 or offset + 8 + bin_size != len(raw):
        raise ValueError("invalid generated GLB binary chunk")
    data = json.loads(raw[20:offset], object_pairs_hook=preview._pairs, parse_constant=preview._invalid_number)
    blob = raw[offset+8:]
    if not isinstance(data, dict):
        raise ValueError("invalid generated GLB JSON")
    asset, buffers = data.get("asset"), data.get("buffers")
    nodes, meshes, scenes = data.get("nodes"), data.get("meshes"), data.get("scenes")
    if (not isinstance(asset, dict) or asset.get("version") != "2.0" or
            data.get("extensionsUsed") or data.get("skins") or data.get("animations") or
            not isinstance(buffers, list) or len(buffers) != 1 or not isinstance(buffers[0], dict) or
            buffers[0].get("uri") is not None or type(buffers[0].get("byteLength")) is not int or
            not 0 <= len(blob)-buffers[0]["byteLength"] <= 3 or
            not isinstance(nodes, list) or len(nodes) != 1 or not isinstance(meshes, list) or len(meshes) != 1 or
            not isinstance(meshes[0], dict)):
        raise ValueError("unsupported generated GLB structure")
    node = nodes[0]
    if not isinstance(node, dict) or type(node.get("mesh")) is not int or node["mesh"] != 0 or set(node) - {"name", "mesh"}:
        raise ValueError("generated GLB must have one untransformed mesh node")
    if (type(data.get("scene")) is not int or data["scene"] != 0 or not isinstance(scenes, list) or
            len(scenes) != 1 or not isinstance(scenes[0], dict) or scenes[0].get("nodes") != [0]):
        raise ValueError("generated GLB scene must contain exactly the mesh node")
    materials = data.get("materials", [])
    if not isinstance(materials, list):
        raise ValueError("invalid generated GLB materials")
    matches = [i for i, material in enumerate(materials) if isinstance(material, dict) and material.get("name") == material_name]
    if len(matches) != 1:
        raise ValueError(f"generated GLB needs one {material_name} material")
    primitives = meshes[0].get("primitives", [])
    if not isinstance(primitives, list):
        raise ValueError("invalid generated GLB primitives")
    matches = [item for item in primitives if isinstance(item, dict) and
               type(item.get("material")) is int and item["material"] == matches[0]]
    if len(matches) != 1 or matches[0].get("mode", 4) != 4 or matches[0].get("targets") or matches[0].get("extensions"):
        raise ValueError("generated GLB needs one triangle primitive for each requested material")
    primitive = matches[0]

    def accessor(index, shape, components, limit):
        accessors, views = data.get("accessors", []), data.get("bufferViews", [])
        if not isinstance(accessors, list) or not isinstance(views, list) or type(index) is not int or not 0 <= index < len(accessors):
            raise ValueError("invalid generated GLB accessor")
        item = accessors[index]
        if (not isinstance(item, dict) or item.get("type") != shape or item.get("componentType") not in components or
                item.get("normalized", False) is not False or item.get("sparse") is not None or
                item.get("extensions") is not None or
                type(item.get("count")) is not int or not 1 <= item["count"] <= limit):
            raise ValueError("unsupported generated GLB accessor")
        view_index = item.get("bufferView")
        if type(view_index) is not int or not 0 <= view_index < len(views):
            raise ValueError("invalid generated GLB buffer view")
        view = views[view_index]
        if not isinstance(view, dict) or view.get("extensions") is not None:
            raise ValueError("unsupported generated GLB buffer view")
        component = item["componentType"]
        fmt = {5123: "H", 5125: "I", 5126: "f"}[component]
        width = {"VEC3": 3, "VEC2": 2, "SCALAR": 1}[shape]
        element = struct.calcsize("<" + fmt * width)
        start, length, inner = view.get("byteOffset", 0), view.get("byteLength"), item.get("byteOffset", 0)
        stride = view.get("byteStride", element)
        component_size = struct.calcsize("<" + fmt)
        if (type(view.get("buffer")) is not int or view["buffer"] != 0 or
                any(type(x) is not int for x in (start, length, inner, stride)) or
                start < 0 or length < 0 or inner < 0 or stride < element or stride > 252 or
                (start+inner) % component_size or stride % component_size or
                start + length > data["buffers"][0]["byteLength"] or
                inner + (item["count"]-1)*stride + element > length):
            raise ValueError("generated GLB accessor exceeds buffer bounds")
        return [struct.unpack_from("<" + fmt * width, blob, start+inner+i*stride) for i in range(item["count"])]

    attrs = primitive.get("attributes", {})
    if not isinstance(attrs, dict) or set(attrs) - {"POSITION", "NORMAL", "TEXCOORD_0"}:
        raise ValueError("invalid generated GLB primitive attributes")
    positions = accessor(attrs.get("POSITION"), "VEC3", (5126,), max_vertices)
    normals = accessor(attrs.get("NORMAL"), "VEC3", (5126,), max_vertices)
    uv = accessor(attrs.get("TEXCOORD_0"), "VEC2", (5126,), max_vertices) if include_uv else None
    indices = [x[0] for x in accessor(primitive.get("indices"), "SCALAR", (5123, 5125), max_indices)]
    if (len(positions) != len(normals) or len(indices) % 3 or any(x >= len(positions) for x in indices) or
            any(not math.isfinite(x) or abs(x) > 10 for point in positions for x in point) or
            any(not all(math.isfinite(x) for x in n) or not .98 <= sum(x*x for x in n) <= 1.02 for n in normals) or
            (uv is not None and (len(uv) != len(positions) or any(not math.isfinite(x) or abs(x) > 10000 for item in uv for x in item)))):
        raise ValueError("invalid generated GLB geometry")
    return (positions, normals, uv, indices) if include_uv else (positions, normals, indices)


def select(scene, lights, actors, glb, emission, render_pass, stage):
    positions, normals, indices = read_geometry(glb)
    scale = scene.conventions.meters_per_unit
    if scale is None or not math.isclose(scale, .01905, rel_tol=0, abs_tol=1e-9):
        raise ValueError("generated candle geometry requires the SDK unit scale")
    if not isinstance(actors, dict) or len(emission) != 3 or any(not math.isfinite(x) or not 0 <= x <= 1000 for x in emission):
        raise ValueError("invalid candle emission or actor export")
    resources = {m.id: m for m in scene.static_mesh_resources}
    submitted = {s.target_id for s in scene.submissions if s.target_kind == "mesh_instance" and
                 (s.render_pass, s.stage) == (render_pass, stage)}
    light_prefix = f"candidate:light:{scene.source.map}:{scene.source.fingerprints.get('map')}:"
    selected_lights = {light["id"]: light for light in lights}
    result, matched = [], 0
    for instance in scene.mesh_instances:
        if instance.id not in submitted or resources[instance.resource_id].name.lower() != "genxlight2candles":
            continue
        name = instance.id.rsplit(":", 1)[-1]
        expected = f"actor:{scene.source.map}:{scene.source.fingerprints.get('map')}:{name}"
        light_id = light_prefix + name
        if instance.id != expected:
            raise ValueError("candle actor identity is not qualified")
        light = selected_lights.get(light_id)
        if light is None:
            continue
        actor = actors.get(name)
        source_light = actor.get("light") if isinstance(actor, dict) else None
        if (not isinstance(actor, dict) or not isinstance(actor.get("mesh"), str) or
                actor["mesh"].lower() != "genxlight2candles" or not isinstance(source_light, dict) or
                type(source_light.get("on")) is not bool or source_light.get("kind") != "omni"):
            raise ValueError("candle actor lacks a matching offline mesh or light state")
        matched += 1
        if not source_light["on"] or light["intensity_candela"] <= 0:
            continue
        if len(result) >= MAX_FLAME_INSTANCES or (len(result)+1)*len(indices)//3 > MAX_FLAME_TRIANGLES:
            raise ValueError("generated candle geometry exceeds bounds")
        matrix = instance.transform
        if any(abs(matrix[i]) > 1e-6 for i in (3, 7, 11)) or abs(matrix[15]-1) > 1e-6:
            raise ValueError("candle actor transform must be affine")
        normal_matrix = legacy.normal_matrix(matrix)
        a,b,c,d,e,f,g,h,i = (matrix[n] for n in (0,1,2,4,5,6,8,9,10))
        mirrored = a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g) < 0
        vertices = []
        for first in range(0, len(indices), 3):
            # glTF-to-T3 swaps Y/Z; that reflection reverses winding.
            for index in (indices[first:first+3] if mirrored else
                          (indices[first], indices[first+2], indices[first+1])):
                x,y,z = positions[index]
                point = transform_point((x/scale,z/scale,y/scale), matrix)
                nx,ny,nz = normals[index]
                normal = legacy.transform_normal((nx,nz,ny), normal_matrix)
                if abs(point[3]-1) > 1e-6 or any(not math.isfinite(x) or abs(x) > 1000000 for x in point[:3]):
                    raise ValueError("candle flame position exceeds bounds")
                vertices.append((*point[:3], 0, 0, *normal, 0, 0, 0, 0))
        result.append({"target_id": instance.id, "light_id": light_id, "vertices": vertices,
                       "emission": tuple(emission), "source": "generated_godot_flame_offline_light_state"})
    if not matched:
        raise ValueError("no submitted candle actor matches a candidate light")
    return result, hashlib.sha256(glb).hexdigest()
