#!/usr/bin/env python3
"""Compare BSP section candidates against diagnostic D3D8 lock shadows.

The GMP scanner is heuristic. Identical captured shadows do not prove the
driver's later GPU inputs, rasterized pixels, visibility, or runtime identity.
This program only reads inputs and does not print payloads or resource pointers.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

import trace_summary

ASSETS = Path(__file__).resolve().parents[2] / "tools" / "assets"
sys.path.insert(0, str(ASSETS))
from t3bsp import find_render_blocks, level_export  # noqa: E402
from upkg import Package  # noqa: E402

IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def candidate(draw, reference, section):
    transforms = draw.get("transforms") or {}
    target = reference.get("transforms") or {}
    world = transforms.get("world") or []
    return (draw.get("call") == "DrawIndexedPrimitive" and draw.get("primitive_type") == 4 and
            draw.get("primitive_count") == section.triangles and section.index_count == 3 * section.triangles and
            draw.get("num_vertices") == section.vertex_count and
            draw.get("min_vertex_index") == section.first_vertex and
            draw.get("start_index") == section.first_index and
            isinstance(world, list) and bool(world) and world[0] == IDENTITY and
            transforms.get("view") is not None and transforms.get("view") == target.get("view") and
            transforms.get("projection") is not None and transforms.get("projection") == target.get("projection") and
            draw.get("viewport") is not None and draw.get("viewport") == reference.get("viewport"))


def observation(part, *, size, resource, offset):
    if (not isinstance(part, dict) or part.get("status") != "captured" or
            part.get("encoding") != "hex" or part.get("byte_count") != size or
            part.get("byte_offset") != offset or not isinstance(resource, str) or
            not resource.startswith("0x") or len(resource) > 10 or
            part.get("resource") != resource):
        return None
    data = part.get("data")
    if not isinstance(data, str) or len(data) != size * 2:
        return None
    try:
        identity = int(resource[2:], 16)
        decoded = bytes.fromhex(data)
        return decoded if 0 < identity <= 0xffffffff and len(decoded) == size else None
    except ValueError:
        return None


def compare(block, section, source, draw, result, frame_end):
    """Return comparisons only when both complete, stable shadows are bound."""
    unavailable = {"vertex_bytes": "unavailable", "index_bytes": "unavailable"}
    if (not isinstance(result, dict) or result.get("hr") != 0 or result.get("buffer_stable") is not True or
            frame_end.get("present_hr") != 0 or frame_end.get("concurrent_operations") is not False or
            frame_end.get("had_reset") is not False):
        return unavailable
    shadows = draw.get("buffer_data") or {}
    streams = draw.get("streams") or []
    stream0 = [s for s in streams if isinstance(s, dict) and s.get("stream") == 0]
    indices = draw.get("indices") or {}
    if (shadows.get("source") != "application_lock_shadow" or len(stream0) != 1 or
            stream0[0].get("hr") != 0 or stream0[0].get("stride") != 32 or indices.get("hr") != 0):
        return unavailable
    vertex, index = shadows.get("vertex") or {}, shadows.get("index") or {}
    base = indices.get("base_vertex_index")
    if (type(base) is not int or base < 0 or vertex.get("stream") != 0 or vertex.get("stride") != 32 or
            vertex.get("first_vertex") != base + section.first_vertex or
            vertex.get("vertex_count") != section.vertex_count or index.get("index_format") != 101 or
            index.get("index_count") != section.index_count):
        return unavailable
    vertex_bytes = observation(vertex, size=section.vertex_count * 32, resource=stream0[0].get("buffer"),
                               offset=(base + section.first_vertex) * 32)
    index_bytes = observation(index, size=section.index_count * 2, resource=indices.get("buffer"),
                              offset=section.first_index * 2)
    if vertex_bytes is None or index_bytes is None:
        return unavailable
    start = block.offset + 0x50 + section.first_vertex * 32
    reference_vertices = source[start:start + section.vertex_count * 32]
    if len(reference_vertices) != section.vertex_count * 32:
        return unavailable
    values = block.mesh.indices[section.first_index:section.first_index + section.index_count]
    if len(values) != section.index_count:
        return unavailable
    reference_indices = struct.pack(f"<{section.index_count}H", *values)
    return {"vertex_bytes": "identical" if vertex_bytes == reference_vertices else "different",
            "index_bytes": "identical" if index_bytes == reference_indices else "different"}


def correlate(blocks, source, records, frame, reference_op, selected):
    frame_records = [r for r in records if r.get("frame") == frame]
    draws = [r for r in frame_records if r.get("event") == "draw"]
    references = [r for r in draws if r.get("op_id") == reference_op]
    ends = [r for r in frame_records if r.get("event") == "frame_end"]
    if len(references) != 1 or len(ends) != 1:
        raise ValueError("reference draw or complete frame is missing")
    results = {r.get("op_id"): r for r in frame_records if r.get("event") == "draw_result"}
    blocks_out = []
    for index in selected:
        if index < 0 or index >= len(blocks):
            raise ValueError(f"block index {index} is out of range")
        block = blocks[index]
        sections = []
        for section in block.mesh.sections:
            occurrences = []
            for draw in draws:
                if candidate(draw, references[0], section):
                    occurrence = {"op_id": draw.get("op_id")}
                    occurrence.update(compare(block, section, source, draw, results.get(draw.get("op_id")), ends[0]))
                    occurrences.append(occurrence)
            sections.append({"material": section.name, "triangles": section.triangles,
                             "first_vertex": section.first_vertex, "vertex_count": section.vertex_count,
                             "first_index": section.first_index, "index_count": section.index_count,
                             "matches": occurrences})
        blocks_out.append({"block_index": index, "level_export_offset": block.offset, "block_id": block.block_id,
                           "bbox_min": block.mesh.bbox_min, "bbox_max": block.mesh.bbox_max,
                           "sections": sections})
    return {"frame": frame, "reference_op": reference_op, "present_hresult": ends[0].get("present_hr"),
            "blocks": blocks_out,
            "limitations": ["BSP blocks are found by a heuristic scan; offsets are Level-export relative.",
                            "Comparison covers application lock shadows, not post-submit GPU contents.",
                            "Matching bytes and matrices do not prove runtime actor identity or visible pixels."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", help="complete diagnostic JSONL")
    parser.add_argument("gmp", help="local level .gmp")
    parser.add_argument("--frame", required=True, type=int)
    parser.add_argument("--reference-op", required=True, type=int, help="draw that identifies view/projection/viewport")
    parser.add_argument("--block", required=True, action="append", type=int, help="scanner index; may be repeated")
    args = parser.parse_args(argv)
    try:
        if len(args.block) != len(set(args.block)):
            raise ValueError("duplicate block index")
        trace_path = Path(args.trace)
        before = hashlib.sha256(trace_path.read_bytes()).digest()
        trace_summary.summarize(trace_path)
        raw = trace_path.read_bytes()
        if hashlib.sha256(raw).digest() != before:
            raise ValueError("trace changed while validating")
        records = [json.loads(line) for line in raw.decode("utf-8").splitlines()]
        gmp = Path(args.gmp)
        gmp_digest = hashlib.sha256(gmp.read_bytes()).hexdigest()
        pkg = Package(gmp)
        blocks = find_render_blocks(pkg)
        source = pkg.export_bytes(level_export(pkg))
        if hashlib.sha256(gmp.read_bytes()).hexdigest() != gmp_digest:
            raise ValueError("GMP changed while reading")
        result = correlate(blocks, source, records, args.frame, args.reference_op, args.block)
        result.update({"trace_sha256": before.hex(), "gmp_sha256": gmp_digest, "scanned_blocks": len(blocks)})
        print(json.dumps(result, indent=2, allow_nan=False))
    except (OSError, UnicodeError, ValueError, KeyError, TypeError, struct.error, trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
