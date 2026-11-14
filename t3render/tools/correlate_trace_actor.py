#!/usr/bin/env python3
"""Correlate an offline actor/mesh with conservative D3D8 draw candidates.

The actor export is not a live actor, and a matching draw does not prove GPU
inputs or visibility. Only identity rotation and unit scale are supported.
No game bytes or transient resource addresses are emitted.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import tempfile

import trace_summary
import trace_to_ir

ASSETS = Path(__file__).resolve().parents[2] / "tools" / "assets"
sys.path.insert(0, str(ASSETS))
from ibt import open_ibt  # noqa: E402
from t3mesh import TYPE_STATICMESH, parse_static_mesh  # noqa: E402

def matches_packed_declaration(words):
    """Match a stream-0 D3D8 declaration using the public D3DVSD token layout.

    The five registers use FLOAT3, D3DCOLOR, FLOAT2, D3DCOLOR, D3DCOLOR;
    other stream/attribute layouts must remain unsupported. These tokens are
    constructed from the D3DVSD_STREAM/REG/END API encodings, not a capture.
    """
    token_stream, token_stream_data = 1, 2
    float2, float3, d3dcolor = 1, 2, 4

    def d3dvsd_reg(register, data_type):
        return (token_stream_data << 29) | (data_type << 16) | register

    expected = [(token_stream << 29),
                *(d3dvsd_reg(register, data_type) for register, data_type in
                  enumerate((float3, d3dcolor, float2, d3dcolor, d3dcolor))),
                (1 << 32) - 1]  # D3DVSD_END()
    return words == expected


def finite_vector(value, size):
    if not isinstance(value, list) or len(value) != size:
        return None
    if any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) for v in value):
        return None
    return value


def compare_payloads(mesh, section, draw, observed, declaration):
    """Compare validated lock shadows with source asset fields, without emitting bytes."""
    unavailable = {"status": "unavailable"}
    vertex, index = observed.get("vertex"), observed.get("index")
    if (declaration.get("status") != "candidate" or not matches_packed_declaration(declaration.get("words")) or
            not isinstance(vertex, dict) or not isinstance(index, dict) or
            vertex.get("status") != "captured" or index.get("status") != "captured" or
            vertex.get("stride_bytes") != 32 or index.get("index_format") != "uint16" or
            draw.get("min_vertex_index") != section.first_vertex or
            vertex.get("vertex_count") != section.vertex_count or index.get("index_count") != section.index_count):
        return unavailable
    try:
        vertices, indices = bytes.fromhex(vertex["data"]), bytes.fromhex(index["data"])
        lo, hi = section.first_vertex, section.first_vertex + section.vertex_count
        if (len(vertices) != section.vertex_count * 32 or len(indices) != section.index_count * 2 or
                lo < 0 or hi > len(mesh.render.positions) or hi > len(mesh.render.uv0) or
                section.first_index < 0 or section.first_index + section.index_count > len(mesh.render.indices)):
            return unavailable
        positions_equal = all(vertices[i * 32:i * 32 + 12] == struct.pack("<3f", *position)
                              for i, position in enumerate(mesh.render.positions[lo:hi]))
        uv_equal = all(vertices[i * 32 + 16:i * 32 + 24] == struct.pack("<2f", *uv)
                       for i, uv in enumerate(mesh.render.uv0[lo:hi]))
        asset_indices = mesh.render.indices[section.first_index:section.first_index + section.index_count]
        indices_equal = indices == struct.pack("<" + "H" * len(asset_indices), *asset_indices)
    except (KeyError, TypeError, ValueError, struct.error, OverflowError):
        return unavailable
    return {"status": "compared", "position_bits_equal": positions_equal,
            "uv0_bits_equal": uv_equal, "indices_u16_equal": indices_equal}


def correlate(actors, actor_name, mesh, records, frame, tolerance):
    if not math.isfinite(tolerance) or not 0 < tolerance <= 0.1:
        raise ValueError("position tolerance must be greater than zero and at most 0.1 T3 units")
    selected = [a for a in actors if isinstance(a, dict) and a.get("name") == actor_name]
    if len(selected) != 1:
        raise ValueError("actor name must identify exactly one exported actor")
    actor = selected[0]
    loc = finite_vector(actor.get("location"), 3)
    rotation = finite_vector(actor.get("rotation"), 3)
    scale = actor.get("draw_scale")
    if (loc is None or rotation != [0, 0, 0] or isinstance(scale, bool) or
            not isinstance(scale, (int, float)) or scale != 1):
        raise ValueError("only actors with finite position, zero rotation and unit scale are supported")
    if actor.get("mesh") != mesh.name:
        raise ValueError("actor and mesh names do not match")
    skin_name = actor.get("skin") or "Default"
    skin = next((s for s in mesh.skins if s.name.casefold() == skin_name.casefold()), None)
    if skin is None:
        raise ValueError("actor skin is not present in the mesh")
    nearby = [a.get("name") for a in actors if isinstance(a, dict) and
              (other := finite_vector(a.get("location"), 3)) is not None and
              math.dist(loc, other) <= tolerance]
    frame_records = [r for r in records if r.get("frame") == frame]
    ends = [r for r in frame_records if r.get("event") == "frame_end"]
    if len(ends) != 1:
        raise ValueError("requested frame is absent or incomplete")
    results = {r.get("op_id"): r for r in frame_records if r.get("event") == "draw_result"}
    header = next((r for r in records if r.get("event") == "header"), {})
    capability = header.get("buffer_capture")
    limit = capability.get("frame_limit_bytes") if isinstance(capability, dict) else None
    emitted = [trace_to_ir.buffered_frame_bytes(r) for r in frame_records if r.get("event") == "draw"]
    within_budget = (type(limit) is int and all(type(n) is int for n in emitted) and sum(emitted) <= limit)
    frame_stable = (trace_to_ir.hresult_succeeded(ends[0].get("present_hr")) is True and
                    ends[0].get("had_reset") is False and ends[0].get("concurrent_operations") is False)
    matches = []
    expected = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    for draw in frame_records:
        if (draw.get("event") != "draw" or draw.get("call") != "DrawIndexedPrimitive" or
                type(draw.get("primitive_type")) is not int or draw["primitive_type"] != 4 or
                any(type(draw.get(k)) is not int or draw[k] < 0
                    for k in ("primitive_count", "num_vertices", "start_index"))):
            continue
        transforms = draw.get("transforms")
        world = transforms.get("world") if isinstance(transforms, dict) else None
        matrix = finite_vector(world[0], 16) if isinstance(world, list) and world else None
        if matrix is None or any(abs(matrix[i] - expected[i]) > 1e-5 for i in range(16) if i not in (12, 13, 14)):
            continue
        distance = math.dist(matrix[12:15], loc)
        if distance > tolerance:
            continue
        for index, section in enumerate(mesh.render.sections):
            if (draw.get("primitive_count") != section.triangles or
                    draw.get("num_vertices") != section.vertex_count or
                    draw.get("start_index") != section.first_index or
                    section.index_count != 3 * section.triangles):
                continue
            result = results.get(draw.get("op_id"), {})
            buffers = draw.get("buffer_data")
            buffers = buffers if isinstance(buffers, dict) else {}
            observations = {}
            for kind in ("vertex", "index"):
                value = buffers.get(kind)
                value = value if isinstance(value, dict) else {}
                observations[kind] = {"status": value.get("status"), "reason": value.get("shadow_reason")}
            observed = trace_to_ir.buffered_data(draw, result, header, True, within_budget and frame_stable)
            declaration = trace_to_ir.observed_declaration(draw, result, header)
            comparison = compare_payloads(mesh, section, draw, observed, declaration)
            matches.append({"op_id": draw.get("op_id"), "section": index, "section_name": section.name,
                            "material": skin.materials[index] if index < len(skin.materials) else None,
                            "triangles": section.triangles, "vertex_count": section.vertex_count,
                            "first_index": section.first_index, "index_count": section.index_count,
                            "position_distance_t3_units": distance, "draw_hresult": result.get("hr"),
                            "buffer_stable": result.get("buffer_stable"), "buffer_observations": observations,
                            "asset_payload_comparison": comparison})
    return {"frame": frame, "actor": actor_name, "mesh": mesh.name, "skin": skin.name,
            "position_t3_units": loc, "rotation": actor["rotation"], "draw_scale": actor["draw_scale"],
            "actors_within_tolerance": nearby, "position_tolerance_t3_units": tolerance,
            "mesh_vertex_count": len(mesh.render.positions), "mesh_index_count": len(mesh.render.indices),
            "present_hresult": ends[0].get("present_hr"), "matches": matches,
            "limitations": ["Actor identity is from an offline map export, not a runtime pointer.",
                            "Matching transform and section counts do not prove GPU inputs or actual visibility.",
                            "Byte comparisons cover intercepted lock shadows and specified attributes, not GPU readback.",
                            "The trace may omit calls and the actor export may differ from the live save.",
                            "Only zero rotation and unit scale are supported; other matches are not searched."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", help="complete diagnostic trace")
    parser.add_argument("actors", help="offline actors.json from t3map.py")
    parser.add_argument("map", help="level .ibt or map name")
    parser.add_argument("--game-dir", help="game installation root")
    parser.add_argument("--actor", required=True, help="exact actor name in the export")
    parser.add_argument("--frame", required=True, type=int)
    parser.add_argument("--tolerance", type=float, default=0.1, help="maximum translation distance in T3 units")
    args = parser.parse_args(argv)
    try:
        source = Path(args.trace).read_bytes()
        export_bytes = Path(args.actors).read_bytes()
        export = json.loads(export_bytes)
        if not isinstance(export, dict) or not isinstance(export.get("actors"), list):
            raise ValueError("invalid actor export")
        if str(export.get("level", "")).casefold() != Path(args.map).stem.casefold():
            raise ValueError("actor export level and map name differ")
        with tempfile.TemporaryDirectory(prefix="t3render-correlation-") as directory:
            snapshot = Path(directory) / "trace.jsonl"
            snapshot.write_bytes(source)
            trace_summary.summarize(snapshot)
        records = [json.loads(line) for line in source.decode("utf-8").splitlines()]
        actor = next((a for a in export["actors"] if isinstance(a, dict) and a.get("name") == args.actor), None)
        if actor is None or not isinstance(actor.get("mesh"), str):
            raise ValueError("actor or its mesh is absent from the export")
        with open_ibt(args.map, args.game_dir) as ibt:
            resource = ibt.find(actor["mesh"], TYPE_STATICMESH)
            if resource is None:
                raise ValueError("mesh is not present in this level bundle")
            mesh = parse_static_mesh(resource, ibt.split(resource))
            asset_sha1 = hashlib.sha1(ibt.read(resource)).hexdigest()
        result = correlate(export["actors"], args.actor, mesh, records, args.frame, args.tolerance)
        result.update({"map": export["level"], "trace_sha256": hashlib.sha256(source).hexdigest(),
                       "actors_sha256": hashlib.sha256(export_bytes).hexdigest(), "mesh_resource_sha1": asset_sha1})
        print(json.dumps(result, indent=2, allow_nan=False))
    except (OSError, ValueError, KeyError, TypeError, trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
