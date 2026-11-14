#!/usr/bin/env python3
"""Make an explicitly partial DX12 diagnostic packet for one observed quad.

This deliberately ignores texture, depth, blending and the rest of the frame.
The numeric shader value is only an FVF hypothesis, not verified GPU input.
"""

import argparse
import math
from pathlib import Path
import struct
import sys

import trace_geometry_candidates as geometry
import trace_summary
import trace_to_ir


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def finite(value, limit=10000):
    return type(value) in (int, float) and math.isfinite(value) and abs(value) <= limit


def convert(path, frame_number, draw_number):
    ir = geometry.enrich(trace_to_ir.convert(path))
    frames = [frame for frame in ir["frames"] if frame["frame"] == frame_number]
    require(len(frames) == 1, "frame is missing or ambiguous")
    commands = [command for command in frames[0]["commands"] if command["id"] == f"draw:{draw_number}"]
    require(len(commands) == 1, "draw is missing from the selected frame")
    command = commands[0]
    mesh = next(mesh for mesh in ir["meshes"] if mesh["id"] == command["mesh"])
    camera = next(camera for camera in ir["cameras"] if camera["id"] == command["camera"])
    candidate = mesh["geometry_candidate"]
    draw = command["compatibility"]["d3d8"]["draw"]
    require(candidate.get("status") == "candidate" and candidate.get("source") == "buffered" and
            command.get("topology") == "triangle_list" and command.get("indexed") is True and
            draw.get("primitive_count") == 2 and draw.get("vertex_shader") == 0x242 and
            draw.get("pixel_shader") == 0, "draw is not the supported observed indexed FVF quad")
    states = draw.get("render_states")
    color_write = states.get("168") if isinstance(states, dict) else None
    require(type(color_write) is int and color_write & 7, "RGB color writes were not observed enabled")
    layout = candidate["layout"]
    require(layout["stride_bytes"] == 32 and layout["fvf_candidate"] == 0x242,
            "unsupported candidate layout")
    viewport = camera["viewport_observed"]
    require(isinstance(viewport, dict) and viewport.get("x") == viewport.get("y") == 0 and
            type(viewport.get("width")) is int and 1 <= viewport["width"] <= 2048 and
            type(viewport.get("height")) is int and 1 <= viewport["height"] <= 2048 and
            finite(viewport.get("min_depth"), 1) and finite(viewport.get("max_depth"), 1) and
            0 <= viewport["min_depth"] <= viewport["max_depth"] <= 1,
            "unsupported or missing viewport")
    world = command.get("transforms_observed", {}).get("world")
    matrices = [world[0] if isinstance(world, list) and world else None,
                camera.get("view_matrix_observed"), camera.get("projection_matrix_observed")]
    values = []
    for matrix in matrices:
        v = matrix.get("values") if isinstance(matrix, dict) else None
        require(isinstance(v, list) and len(v) == 16 and all(finite(x) for x in v),
                "missing or unsupported observed matrix")
        values.append(v)
    indices = candidate.get("indices")
    require(isinstance(indices, dict) and indices.get("index_count") == 6 and
            indices.get("index_format") in ("uint16", "uint32"), "invalid quad indices")
    index_bytes = bytes.fromhex(indices["data"])
    index_format = "<H" if indices["index_format"] == "uint16" else "<I"
    ids = [item[0] for item in struct.iter_unpack(index_format, index_bytes)]
    require(len(ids) == 6 and all(i < candidate["vertex_count"] for i in ids), "invalid quad index range")
    first, second = set(ids[:3]), set(ids[3:])
    require(len(first) == len(second) == 3 and len(first & second) == 2,
            "indices do not form two triangles sharing one edge")
    raw = bytes.fromhex(candidate["vertices"]["data"])
    vertices = [struct.unpack_from("<3fI", raw, index * 32) for index in ids]
    for tri in (vertices[:3], vertices[3:]):
        require(all(all(finite(v) for v in vertex[:3]) and vertex[3] >> 24 == 255 for vertex in tri) and
                tri[0][3] == tri[1][3] == tri[2][3], "diagnostic triangles require finite positions and flat opaque colors")
    lines = ["T3R_DIAGNOSTIC_XYZ_V1", f"{viewport['width']} {viewport['height']}",
             f"{viewport['min_depth']:.9g} {viewport['max_depth']:.9g}", str(0xff000000)]
    lines.extend(" ".join(f"{value:.9g}" for value in matrix) for matrix in values)
    lines.extend(" ".join([*(f"{value:.9g}" for value in vertex[:3]), str(vertex[3])]) for vertex in vertices)
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    parser.add_argument("--frame", required=True, type=int, help="frame number recorded by the proxy")
    parser.add_argument("--draw", required=True, type=int, help="global draw number from the offline IR")
    parser.add_argument("-o", "--output", required=True, help="new diagnostic packet; never overwritten")
    args = parser.parse_args(argv)
    try:
        packet = convert(args.trace, args.frame, args.draw)
        with Path(args.output).open("x", encoding="ascii", newline="\n") as output:
            output.write(packet)
    except (OSError, UnicodeError, ValueError, KeyError, StopIteration, struct.error, trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print("Partial diagnostic only: FVF and GPU inputs unverified; texture, depth, blending and other draws omitted.",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
