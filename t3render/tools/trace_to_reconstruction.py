#!/usr/bin/env python3
"""Create a partial, visibly approximate DX12 reconstruction from one trace.

Use stable observed VB/IB bytes only for a narrow verified declaration and
vertex-position program shape. Textures and most render state remain absent.
This is not a faithful replay or proof of GPU-consumed data.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

import offline_mesh_reconstruction as common
import trace_summary
import trace_to_ir


def matches_position_program(words):
    """Match a vs_1_0 position transform by D3D8 opcode/register semantics.

    Require four DP4 instructions writing oPos.xyzw from v0 and c1..c4,
    with absolute, unmodified and unswizzled sources. Tokens are constructed
    from the D3D8 shader format, not copied from a game shader.
    """
    if not isinstance(words, list):
        return False
    version = (0xfffe << 16) | (1 << 8)  # D3DVS_VERSION(1, 0)
    opcode_dp4 = 9  # D3DSIO_DP4
    marker = 1 << 31
    input_register, constant_register, raster_output = 1, 2, 4  # D3DSPR_*
    no_swizzle = sum(component << (16 + 2 * component) for component in range(4))
    source_position = marker | (input_register << 28) | no_swizzle  # v0
    prefix = [version]
    for component in range(4):
        destination = marker | (raster_output << 28) | (1 << (16 + component))  # oPos
        constant = marker | (constant_register << 28) | no_swizzle | (component + 1)
        prefix.extend((opcode_dp4, destination, source_position, constant))
    if words[:len(prefix)] != prefix:
        return False
    # Keep the prior conservative check against any later oPos token, even in
    # payloads we do not interpret, rather than accepting a later overwrite.
    position_mask = (0xf << 28) | 0xffff
    position_register = marker | (raster_output << 28)
    return not any((word & position_mask) == position_register for word in words[len(prefix):-1])


def multiply(a, b):
    return [sum(a[row * 4 + k] * b[k * 4 + col] for k in range(4))
            for row in range(4) for col in range(4)]


def verify_position_shader(command, world, view, projection):
    declaration = command.get("vertex_declaration_observed", {})
    program = command.get("vertex_program_observed", {})
    constants = command.get("vertex_constants_observed", {})
    if (declaration.get("status") != "candidate" or
            not common.correlation.matches_packed_declaration(declaration.get("words"))):
        raise ValueError("unsupported_declaration")
    words = program.get("words")
    if program.get("status") != "candidate" or not matches_position_program(words):
        raise ValueError("unsupported_position_shader")
    bits = constants.get("bits")
    if constants.get("status") != "sampled" or not isinstance(bits, list) or len(bits) < 20:
        raise ValueError("missing_position_constants")
    sampled = struct.unpack("<16f", struct.pack("<16I", *bits[4:20]))
    combined = multiply(multiply(world, view), projection)
    if any(not math.isfinite(sampled[row * 4 + col]) or
           abs(sampled[row * 4 + col] - combined[col * 4 + row]) >
           0.001 + 0.00001 * abs(combined[col * 4 + row])
           for row in range(4) for col in range(4)):
        raise ValueError("position_constants_do_not_match_matrices")


def color_for_triangle(points, op):
    a = [points[1][i] - points[0][i] for i in range(3)]
    b = [points[2][i] - points[0][i] for i in range(3)]
    normal = [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]
    length = math.sqrt(sum(v*v for v in normal))
    brightness = 0.58 if length < 1e-12 else 0.48 + 0.42 * abs(
        (normal[0] * 0.24 + normal[1] * -0.36 + normal[2] * 0.9) / length)
    base = common.PALETTE[op % len(common.PALETTE)]
    rgb = [max(0, min(255, round(((base >> shift) & 255) * brightness))) for shift in (16, 8, 0)]
    return 0xff000000 | rgb[0] << 16 | rgb[1] << 8 | rgb[2]


def extract(command, mesh, expected_camera):
    draw = command["compatibility"]["d3d8"]["draw"]
    if command.get("topology") != "triangle_list" or command.get("indexed") is not True:
        raise ValueError("unsupported_topology")
    states = draw.get("render_states", {})
    required = {"7": 1, "14": 1, "22": 3, "23": 2, "27": 0, "52": 0, "168": 15}
    if not isinstance(states, dict) or any(states.get(key) != value for key, value in required.items()):
        raise ValueError("unsupported_render_state")
    world, view, projection, viewport = common.vectors(draw)
    if (view, projection, viewport) != expected_camera:
        raise ValueError("different_camera_or_viewport")
    verify_position_shader(command, world, view, projection)
    data = mesh.get("buffered_data_observed", {})
    vertex, index = data.get("vertex", {}), data.get("index", {})
    if vertex.get("status") != "captured" or index.get("status") != "captured":
        raise ValueError("unavailable_buffer_bytes")
    if vertex.get("stride_bytes") != 32 or index.get("index_format") != "uint16":
        raise ValueError("unsupported_buffer_layout")
    raw_vertex = bytes.fromhex(vertex["data"])
    raw_index = bytes.fromhex(index["data"])
    count = vertex["vertex_count"]
    minimum = draw.get("min_vertex_index")
    if type(minimum) is not int or minimum < 0 or len(raw_vertex) != count * 32 or len(raw_index) % 6:
        raise ValueError("invalid_buffer_window")
    result = []
    for ids in struct.iter_unpack("<3H", raw_index):
        if any(index < minimum or index >= minimum + count for index in ids):
            raise ValueError("index_outside_window")
        points = [common.transformed_position(struct.unpack_from("<3f", raw_vertex, (idx-minimum) * 32), world)
                  for idx in ids]
        color = color_for_triangle(points, draw["op_id"])
        result.extend((point, color) for point in points)
    return result


def convert(path, frame_number, camera_op):
    ir = trace_to_ir.convert(path)
    frames = [item for item in ir["frames"] if item["frame"] == frame_number]
    if len(frames) != 1:
        raise ValueError("frame is missing or ambiguous")
    frame = frames[0]
    end = frame["compatibility"]["d3d8"]["frame_end"]
    if end.get("present_hr") != 0 or end.get("had_reset") is not False or end.get("concurrent_operations") is not False:
        raise ValueError("frame has failed Present, reset or recorded overlap")
    ref = [c for c in frame["commands"] if c["compatibility"]["d3d8"]["draw"].get("op_id") == camera_op]
    if len(ref) != 1:
        raise ValueError("camera operation is missing or ambiguous")
    ref_world, view, projection, viewport = common.vectors(ref[0]["compatibility"]["d3d8"]["draw"])
    verify_position_shader(ref[0], ref_world, view, projection)
    meshes = {m["id"]: m for m in ir["meshes"]}
    vertices, selected, rejected = [], [], Counter()
    for command in frame["commands"]:
        draw = command["compatibility"]["d3d8"]["draw"]
        try:
            points = extract(command, meshes[command["mesh"]], (view, projection, viewport))
            if len(vertices) + len(points) > common.MAX_TRIANGLES * 3:
                rejected["triangle_limit"] += 1
                continue
            vertices.extend(points)
            selected.append({"op_id": draw["op_id"], "triangles": len(points) // 3})
        except (KeyError, TypeError, ValueError, struct.error, OverflowError) as exc:
            # Do not leak resource IDs or arbitrary exception strings into the manifest.
            label = str(exc)
            rejected[label if label in {"unsupported_topology", "unsupported_render_state", "different_camera_or_viewport",
                "unsupported_declaration", "unsupported_position_shader", "missing_position_constants",
                "position_constants_do_not_match_matrices", "unavailable_buffer_bytes", "unsupported_buffer_layout",
                "invalid_buffer_window", "index_outside_window"} else "invalid_or_unavailable_observation"] += 1
    if not vertices:
        raise ValueError("no compatible indexed triangles with captured buffers")
    width, height, near, far = viewport
    lines = ["T3R_RECONSTRUCTION_XYZ_V1", f"{width} {height}", f"{near:.9g} {far:.9g}",
             str(0xff16181c), " ".join(map(str, common.IDENTITY)),
             " ".join(f"{x:.9g}" for x in view), " ".join(f"{x:.9g}" for x in projection),
             str(len(vertices) // 3)]
    lines.extend(" ".join(f"{x:.9g}" for x in point) + f" {color}" for point, color in vertices)
    packet = "\n".join(lines) + "\n"
    return packet, {"kind": "trace_geometry_reconstruction_not_faithful_replay", "frame": frame_number,
        "trace_sha256": ir["source"]["sha256"], "camera_reference_op": camera_op,
        "viewport": {"width": width, "height": height, "min_z": near, "max_z": far},
        "triangles": len(vertices) // 3, "selected_draws": selected, "excluded_draws": dict(rejected),
        "packet_sha256": hashlib.sha256(packet.encode("ascii")).hexdigest(),
        "limitations": ["VB/IB shadows and shader candidates are observed application inputs, not verified GPU input.",
            "Only a narrow vertex declaration and position program, matching sampled constants, opaque LESS depth draws and one camera are used.",
            "Materials, lights and triangle colors are synthetic; textures, shader effects, blending, depth history and postprocessing are omitted.",
            "LESS depth and CCW culling are reconstructed from selected draw states; depth format and winding parity remain unverified.",
            "Unselected draws and render-target changes are omitted; this is not the complete TDS frame."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    parser.add_argument("--frame", type=int, required=True)
    parser.add_argument("--camera-op", type=int, required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    try:
        packet, manifest = convert(args.trace, args.frame, args.camera_op)
        target = Path(args.output)
        sidecar = Path(str(target) + ".manifest.json")
        if target.exists() or sidecar.exists():
            raise ValueError("packet and manifest must both be new files")
        with target.open("x", encoding="ascii", newline="\n") as stream:
            stream.write(packet)
        with sidecar.open("x", encoding="utf-8") as stream:
            json.dump(manifest, stream, indent=2, allow_nan=False)
            stream.write("\n")
        print(json.dumps(manifest, indent=2, allow_nan=False))
    except (OSError, UnicodeError, ValueError, KeyError, TypeError, struct.error, trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
