#!/usr/bin/env python3
"""Attach bounded D3D8 FVF geometry candidates to a validated offline Render IR.

This does not certify a game frame for replay: a shader handle can resemble
an FVF, UP memory may change after observation, and capture omits operations
such as render-target changes. Missing or ambiguous bytes stay unavailable.
"""

import argparse
import json
import math
from pathlib import Path
import struct
import sys

import trace_to_ir
import trace_summary


class GeometryError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise GeometryError(message)


def fvf_layout(value, stride):
    """Describe the non-blended fixed-function subset, allowing stride padding."""
    require(type(value) is int and 0 <= value <= 0xffffffff, "vertex shader/FVF is missing")
    require(type(stride) is int and 0 < stride <= 0xffffffff, "invalid vertex stride")
    position = value & 0x0e
    require(position in (2, 4), "unsupported position or vertex blending")
    count = (value >> 8) & 0x0f
    require(count <= 8, "unsupported texture coordinate count")
    high_mask = ((1 << (2 * count)) - 1) << 16 if count else 0
    allowed = 0x0e | 0x10 | 0x20 | 0x40 | 0x80 | 0x0f00 | high_mask
    require(value & ~allowed == 0, "unsupported FVF bits")
    require(not (position == 4 and value & 0x10), "transformed vertices with normals are unsupported")
    attributes = []
    offset = 0

    def add(name, encoding, components, size):
        nonlocal offset
        attributes.append({"semantic": name, "encoding": encoding, "components": components,
                           "offset_bytes": offset})
        offset += size

    add("position", "float32", 3 if position == 2 else 4, 12 if position == 2 else 16)
    if value & 0x10: add("normal", "float32", 3, 12)
    if value & 0x20: add("point_size", "float32", 1, 4)
    if value & 0x40: add("diffuse", "d3dcolor_argb8", 4, 4)
    if value & 0x80: add("specular", "d3dcolor_argb8", 4, 4)
    for index in range(count):
        components = (2, 3, 4, 1)[(value >> (16 + 2 * index)) & 3]
        add(f"texcoord{index}", "float32", components, 4 * components)
    require(offset <= stride, "FVF does not fit the observed stride")
    return {"fvf_candidate": value, "position_kind": "xyz" if position == 2 else "xyzrhw",
            "stride_bytes": stride, "minimum_stride_bytes": offset, "attributes": attributes,
            "shader_interpretation": "unverified_fvf_or_programmable_handle"}


def geometry_candidate(command, mesh, result, frame_end, up_component_limit, up_within_budget):
    """Decode only observations proven coherent by trace_to_ir; reject uncertainty."""
    try:
        require(isinstance(frame_end, dict) and frame_end.get("concurrent_operations") is False and
                frame_end.get("had_reset") is False and
                trace_to_ir.hresult_succeeded(frame_end.get("present_hr")) is True,
                "frame outcome or ordering is unknown")
        require(isinstance(result, dict) and trace_to_ir.hresult_succeeded(result.get("hr")) is True,
                "draw outcome is unknown or failed")
        require(command.get("topology") in ("point_list", "line_list", "line_strip", "triangle_list",
                                             "triangle_strip", "triangle_fan"), "unsupported topology")
        raw_draw = command["compatibility"]["d3d8"]["draw"]
        primitives = trace_to_ir.uint32(raw_draw.get("primitive_count"))
        elements = trace_to_ir.primitive_elements(raw_draw)
        require(primitives is not None and primitives > 0 and elements is not None,
                "primitive count or topology is unknown")
        source = mesh.get("geometry_source")
        indexed = command.get("indexed")
        require(indexed is True or indexed is False, "unknown draw kind")
        if source == "buffered":
            data = mesh.get("buffered_data_observed")
            require(isinstance(data, dict) and data.get("source") == "application_lock_shadow" and
                    data.get("buffer_stable") is True,
                    "buffer observations are unavailable or unstable")
        elif source == "immediate":
            data = mesh.get("immediate_data_observed")
            require(up_component_limit is not None and up_within_budget,
                    "immediate capture capability or frame budget is unavailable")
        else:
            raise GeometryError("unsupported geometry source")
        require(isinstance(data, dict), "geometry observations are unavailable")
        vertex = data.get("vertex")
        require(isinstance(vertex, dict) and vertex.get("status") == "captured" and
                vertex.get("encoding") == "hex", "vertex bytes are unavailable")
        count, stride = vertex.get("vertex_count"), vertex.get("stride_bytes")
        require(type(count) is int and count > 0 and type(stride) is int and stride > 0,
                "empty or invalid vertex range")
        if source == "immediate":
            require(trace_to_ir.uint32(raw_draw.get("vertex_stride")) == stride,
                    "immediate vertex stride is missing or inconsistent")
            raw_up = raw_draw.get("up_data")
            if not indexed:
                require(isinstance(raw_up, dict) and "index" in raw_up and raw_up["index"] is None,
                        "non-indexed immediate draw has inconsistent index data")
        if indexed:
            require(trace_to_ir.uint32(raw_draw.get("num_vertices")) == count,
                    "declared vertex count is missing or inconsistent")
        else:
            require(count == elements, "vertex count differs from topology")
        layout = fvf_layout(raw_draw.get("vertex_shader"), stride)
        raw_vertices = bytes.fromhex(vertex["data"])
        require(len(raw_vertices) == count * stride, "vertex byte count differs from range")
        if source == "immediate":
            require(len(raw_vertices) <= up_component_limit, "immediate vertex component exceeds the capture limit")
        for n in range(count):
            for attribute in layout["attributes"]:
                if attribute["encoding"] == "float32":
                    values = struct.unpack_from("<" + "f" * attribute["components"], raw_vertices,
                                                n * stride + attribute["offset_bytes"])
                    require(all(math.isfinite(v) for v in values), "non-finite vertex attribute")
                    if attribute["semantic"] == "position" and layout["position_kind"] == "xyzrhw":
                        require(values[3] > 0, "invalid reciprocal homogeneous coordinate")
        index_output = None
        if indexed:
            part = data.get("index")
            require(isinstance(part, dict) and part.get("status") == "captured" and
                    part.get("encoding") == "hex" and part.get("index_format") in ("uint16", "uint32"),
                    "index bytes are unavailable")
            raw_indices = bytes.fromhex(part["data"])
            if source == "immediate":
                require(len(raw_indices) <= up_component_limit, "immediate index component exceeds the capture limit")
            width = 2 if part["index_format"] == "uint16" else 4
            fmt = "<H" if width == 2 else "<I"
            require(type(part.get("index_count")) is int and len(raw_indices) == part["index_count"] * width,
                    "index byte count differs from range")
            require(part["index_count"] == elements, "index count differs from topology")
            if source == "immediate":
                require(trace_to_ir.uint32(raw_draw.get("index_format")) == (101 if width == 2 else 102),
                        "immediate index format is missing or inconsistent")
            minimum = command.get("parameters", {}).get("minimum_vertex")
            require(type(minimum) is int and minimum >= 0, "minimum vertex is unknown")
            normalized = bytearray()
            for (index,) in struct.iter_unpack(fmt, raw_indices):
                require(minimum <= index < minimum + count, "index is outside the observed vertex window")
                normalized.extend(struct.pack(fmt, index - minimum))
            index_output = {"index_format": part["index_format"], "index_count": part["index_count"],
                            "encoding": "hex", "data": normalized.hex(),
                            "normalization": "observed index minus declared minimum vertex"}
        return {"status": "candidate", "source": source, "layout": layout,
                "vertex_count": count, "vertices": {"encoding": "hex", "data": vertex["data"]},
                "indices": index_output}
    except (GeometryError, KeyError, TypeError, ValueError, struct.error, OverflowError) as exc:
        return {"status": "unavailable", "reason": str(exc)}


def up_frame_bytes(frame, component_limit):
    """Account for all emitted UP components, even for failed or unusable draws."""
    if component_limit is None:
        return None
    total = 0
    for command in frame["commands"]:
        draw = command["compatibility"]["d3d8"]["draw"]
        if draw.get("call") not in ("DrawPrimitiveUP", "DrawIndexedPrimitiveUP"):
            continue
        data = draw.get("up_data")
        if not isinstance(data, dict):
            return None
        indexed = draw["call"] == "DrawIndexedPrimitiveUP"
        if "index" not in data or (not indexed and data["index"] is not None):
            return None
        for name in (("vertex", "index") if indexed else ("vertex",)):
            part = data.get(name)
            if not isinstance(part, dict) or part.get("encoding") != "hex" or "data" not in part:
                return None
            if part.get("status") not in ("captured", "empty"):
                if (part.get("status") not in ("unreadable", "null_pointer", "overflow", "per_draw_limit",
                                                "frame_limit", "unsupported_topology", "invalid_stride",
                                                "unsupported_index_format") or part["data"] is not None):
                    return None
                continue
            if trace_to_ir.captured_hex(part) is None:
                return None
            size = trace_to_ir.nonnegative(part.get("byte_count"))
            text = part.get("data")
            if size is None or not isinstance(text, str):
                return None
            if max(size, (len(text) + 1) // 2) > component_limit:
                return None
            total += max(size, (len(text) + 1) // 2)
    return total


def enrich(ir):
    meshes = {item["id"]: item for item in ir["meshes"]}
    capability = ir["compatibility"]["d3d8"]["header"].get("up_capture")
    capability = capability if isinstance(capability, dict) else {}
    up_component_limit = trace_to_ir.uint32(capability.get("component_limit_bytes"))
    up_frame_limit = trace_to_ir.uint32(capability.get("frame_limit_bytes"))
    if (capability.get("encoding") != "hex" or up_component_limit is None or
            not 0 < up_component_limit <= 256 * 1024 or up_frame_limit is None or
            not up_component_limit <= up_frame_limit <= 8 * 1024 * 1024):
        up_component_limit = up_frame_limit = None
    for frame in ir["frames"]:
        events = frame["compatibility"]["d3d8"]["events"]
        results = {item["event"].get("op_id"): item["event"] for item in events
                   if item["event"].get("event") == "draw_result"}
        end = frame["compatibility"]["d3d8"]["frame_end"]
        recorded_reset = any(item["event"].get("event") == "reset" for item in events)
        valid_draw_count = (isinstance(end, dict) and type(end.get("draws")) is int and
                            end["draws"] == len(frame["commands"]))
        emitted_up_bytes = up_frame_bytes(frame, up_component_limit)
        up_within_budget = (up_frame_limit is not None and emitted_up_bytes is not None and
                            emitted_up_bytes <= up_frame_limit)
        for command in frame["commands"]:
            mesh = meshes[command["mesh"]]
            draw = command["compatibility"]["d3d8"]["draw"]
            mesh["geometry_candidate"] = ({"status": "unavailable", "reason": "reset event recorded in frame"}
                                          if recorded_reset else {"status": "unavailable", "reason": "frame draw count is missing or inconsistent"}
                                          if not valid_draw_count else
                                          geometry_candidate(command, mesh, results.get(draw.get("op_id")), end,
                                                             up_component_limit, up_within_budget))
    ir["geometry_candidate_policy"] = (
        "Observed bytes and an inferred FVF layout only; shader identity, GPU input and full frame replay are unverified.")
    return ir


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", help="complete diagnostic JSONL trace")
    parser.add_argument("-o", "--output", help="new offline IR JSON file (default: stdout)")
    args = parser.parse_args(argv)
    try:
        ir = enrich(trace_to_ir.convert(args.trace))
        encoded = json.dumps(ir, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
        if args.output:
            with Path(args.output).open("x", encoding="utf-8") as destination:
                destination.write(encoded)
        else:
            sys.stdout.write(encoded)
    except (OSError, UnicodeError, ValueError, trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
