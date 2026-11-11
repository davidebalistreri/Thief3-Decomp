#!/usr/bin/env python3
"""Convert a validated D3D8 diagnostic trace to a partial offline Render IR.

This is neither a runtime IR nor a replay. Every object is an observed per-draw
snapshot, not a reconstructed game asset. See README_render_ir.md for schema.
"""

import argparse
import hashlib
import io
import json
import math
from pathlib import Path
import sys
import tempfile

import trace_summary


TOPOLOGIES = {1: "point_list", 2: "line_list", 3: "line_strip", 4: "triangle_list",
              5: "triangle_strip", 6: "triangle_fan"}
CALLS = {
    "DrawPrimitive": ("buffered", False),
    "DrawIndexedPrimitive": ("buffered", True),
    "DrawPrimitiveUP": ("immediate", False),
    "DrawIndexedPrimitiveUP": ("immediate", True),
}
LIGHT_TYPES = {1: "point", 2: "spot", 3: "directional"}
INDEX_FORMATS = {101: "uint16", 102: "uint32"}


def integer(value):
    return value if isinstance(value, int) and not isinstance(value, bool) else None


def hresult_succeeded(value):
    """Interpret signed or unsigned 32-bit HRESULT; None means invalid/absent."""
    value = integer(value)
    if value is None or not -(1 << 31) <= value <= (1 << 32) - 1:
        return None
    return (value & (1 << 31)) == 0


def number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return value if isinstance(value, int) or math.isfinite(value) else None


def vector(value, size):
    if not isinstance(value, list) or len(value) != size:
        return None
    return [number(component) for component in value]


def matrix(value):
    values = vector(value, 16)
    if values is None:
        return None
    return {"values": values, "storage_order": "row_major", "coordinate_convention": None}


def matrix_list(value):
    if not isinstance(value, list):
        return None
    return [matrix(item) for item in value]


def object_or_empty(value):
    return value if isinstance(value, dict) else {}


def capture_id(value):
    if not isinstance(value, str) or not value:
        return None
    return {"scope": "capture", "token": value}


def binding(record):
    """Getters distinguish a successful null binding from a failed getter."""
    if not isinstance(record, dict):
        return {"status": "unknown", "resource": None}
    if hresult_succeeded(record.get("hr")) is not True or "buffer" not in record:
        return {"status": "unknown", "resource": None}
    if record["buffer"] is None:
        return {"status": "unbound", "resource": None}
    identifier = capture_id(record["buffer"])
    if identifier is None:
        return {"status": "unknown", "resource": None}
    desc = object_or_empty(record.get("description"))
    return {"status": "bound", "resource": {"id": identifier, "size_bytes": integer(desc.get("size"))}}


def vertex_streams(value):
    if not isinstance(value, list):
        return None
    streams = []
    for stream in value:
        if not isinstance(stream, dict):
            streams.append(None)
            continue
        b = binding(stream)
        streams.append({"slot": integer(stream.get("stream")), **b,
                        "stride_bytes": integer(stream.get("stride")) if b["status"] != "unknown" else None})
    return streams


def index_buffer(value):
    b = binding(value)
    data = object_or_empty(value)
    desc = object_or_empty(data.get("description"))
    fmt = integer(desc.get("format"))
    return {**b,
            "base_vertex_index": integer(data.get("base_vertex_index")) if b["status"] != "unknown" else None,
            "index_format": INDEX_FORMATS.get(fmt) if b["status"] == "bound" else None}


def texture_slots(value):
    if not isinstance(value, list):
        return None
    slots = []
    for stage in value:
        if not isinstance(stage, dict):
            slots.append(None)
            continue
        success = hresult_succeeded(stage.get("texture_hr")) if "texture_hr" in stage else True
        texture = stage.get("texture")
        status = "unknown"
        resource = None
        if "texture" in stage and success is True:
            if texture is None:
                status = "unbound"
            elif isinstance(texture, dict):
                identifier = capture_id(texture.get("id"))
                if identifier is not None:
                    status = "bound"
                    resource = {"id": identifier, "width": integer(texture.get("width")),
                                "height": integer(texture.get("height")), "depth": integer(texture.get("depth")),
                                "levels": integer(texture.get("levels"))}
        slots.append({"slot": integer(stage.get("stage")), "status": status, "resource": resource})
    return slots


def camera(draw, identifier):
    transforms = object_or_empty(draw.get("transforms"))
    raw_viewport = draw.get("viewport")
    viewport = None
    if isinstance(raw_viewport, dict):
        viewport = {key: integer(raw_viewport.get(key)) for key in ("x", "y", "width", "height")}
        viewport["min_depth"] = number(raw_viewport.get("min_z"))
        viewport["max_depth"] = number(raw_viewport.get("max_z"))
    return {"id": identifier, "view_matrix_observed": matrix(transforms.get("view")),
            "projection_matrix_observed": matrix(transforms.get("projection")),
            "viewport_observed": viewport, "effective_for_draw": None}


def material(draw, identifier):
    raw = draw.get("material")
    parameters = None
    if isinstance(raw, dict):
        parameters = {name: vector(raw.get(name), 4) for name in ("diffuse", "ambient", "specular", "emissive")}
        parameters["power"] = number(raw.get("power"))
    return {"id": identifier, "legacy_parameters_observed": parameters,
            "texture_slots_observed": texture_slots(draw.get("stages")), "pbr_parameters": None}


def mesh(draw, identifier, source, indexed):
    # UP data is supplied directly to the call; bound streams/indices are NOT
    # that data. The trace does not contain bytes for either path.
    streams = vertex_streams(draw.get("streams")) if source == "buffered" else None
    indices = index_buffer(draw.get("indices")) if source == "buffered" and indexed is True else None
    return {"id": identifier, "geometry_available": False, "geometry_source": source,
            "vertex_streams_observed": streams, "index_buffer_observed": indices}


def observed_lights(draw, ir, draw_index):
    values = draw.get("lights")
    if not isinstance(values, list) or any(not isinstance(light, dict) for light in values):
        return {"observed": None, "complete": False}
    result = []
    for i, raw in enumerate(values):
        identifier = f"light:{draw_index}:{i}"
        enabled = raw.get("enabled")
        light = {"id": identifier, "kind": LIGHT_TYPES.get(integer(raw.get("type"))),
                 "enabled": enabled if isinstance(enabled, bool) else None,
                 "diffuse": vector(raw.get("diffuse"), 4), "specular": vector(raw.get("specular"), 4),
                 "ambient": vector(raw.get("ambient"), 4), "position": vector(raw.get("position"), 3),
                 "direction": vector(raw.get("direction"), 3), "range": number(raw.get("range")),
                 "falloff": number(raw.get("falloff")), "attenuation": vector(raw.get("attenuation"), 3),
                 "inner_angle": number(raw.get("theta")), "outer_angle": number(raw.get("phi")),
                 "compatibility": {"d3d8": {"observed_light": raw}}}
        ir["lights"].append(light)
        result.append(identifier)
    return {"observed": result, "complete": False}


def command_parameters(draw, source, indexed):
    # Irrelevant fields in the diagnostic producer are zero-filled, so do not
    # turn those zeros into meaningful parameters for a different draw kind.
    values = {"primitive_count": integer(draw.get("primitive_count")), "first_vertex": None,
              "minimum_vertex": None, "vertex_count": None, "first_index": None,
              "immediate_vertex_stride_bytes": None, "immediate_index_format": None}
    if source == "buffered" and indexed is False:
        values["first_vertex"] = integer(draw.get("start_vertex"))
    if indexed is True:
        values["minimum_vertex"] = integer(draw.get("min_vertex_index"))
        values["vertex_count"] = integer(draw.get("num_vertices"))
        if source == "buffered":
            values["first_index"] = integer(draw.get("start_index"))
        if source == "immediate":
            values["immediate_index_format"] = INDEX_FORMATS.get(integer(draw.get("index_format")))
    if source == "immediate":
        values["immediate_vertex_stride_bytes"] = integer(draw.get("vertex_stride"))
    return values


def convert(path):
    # Validate an immutable private copy, then parse those exact bytes. This
    # avoids races between validation and reading a file being written.
    data = Path(path).read_bytes()
    with tempfile.TemporaryDirectory(prefix="t3render-ir-") as temporary:
        snapshot = Path(temporary) / "trace.jsonl"
        snapshot.write_bytes(data)
        summary = trace_summary.summarize(snapshot)

    def finite_float(text):
        value = float(text)
        if not math.isfinite(value):
            raise ValueError("trace contains a number outside the supported finite range")
        return value

    records = [json.loads(line, parse_float=finite_float) for line in io.StringIO(data.decode("utf-8"), newline=None)]
    ir = {
        "schema": "t3render.render-ir.offline", "version": 1,
        "source": {"format": summary["format"], "version": summary["version"],
                   "sha256": hashlib.sha256(data).hexdigest()},
        "runtime_ir": False, "replayable": False, "extraction_complete": False,
        "frames": [], "cameras": [], "meshes": [], "materials": [], "lights": [],
        "compatibility": {"d3d8": {"header": records[0], "unmapped_events": [], "capture_end": None}},
    }
    current = None
    draw_index = 0
    for line_number, event in enumerate(records, 1):
        kind = event["event"]
        if kind == "header":
            continue
        if kind == "capture_end":
            ir["compatibility"]["d3d8"]["capture_end"] = event
        elif kind == "frame_begin":
            current = {"frame": event["frame"], "commands": [],
                       "compatibility": {"d3d8": {"frame_begin": event, "events": [], "frame_end": None}}}
            ir["frames"].append(current)
        elif kind == "frame_end":
            current["compatibility"]["d3d8"]["frame_end"] = event
            current = None
        elif kind == "draw":
            source, indexed = CALLS.get(event["call"], (None, None))
            camera_id, mesh_id, material_id = f"camera:{draw_index}", f"mesh:{draw_index}", f"material:{draw_index}"
            ir["cameras"].append(camera(event, camera_id))
            ir["meshes"].append(mesh(event, mesh_id, source, indexed))
            ir["materials"].append(material(event, material_id))
            transforms = object_or_empty(event.get("transforms"))
            current["commands"].append({
                "id": f"draw:{draw_index}", "source_line": line_number,
                "camera": camera_id, "mesh": mesh_id, "material": material_id,
                "lights": observed_lights(event, ir, draw_index),
                "topology": TOPOLOGIES.get(integer(event.get("primitive_type"))), "indexed": indexed,
                "parameters": command_parameters(event, source, indexed),
                "transforms_observed": {"world": matrix_list(transforms.get("world")),
                                        "texture": matrix_list(transforms.get("texture"))},
                "compatibility": {"d3d8": {"draw": event}},
            })
            draw_index += 1
        else:
            target = (current["compatibility"]["d3d8"]["events"] if current is not None
                      else ir["compatibility"]["d3d8"]["unmapped_events"])
            target.append({"source_line": line_number, "event": event})
    return ir


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", help="complete diagnostic JSONL trace")
    parser.add_argument("-o", "--output", help="new output JSON file (default: standard output)")
    args = parser.parse_args(argv)
    try:
        encoded = json.dumps(convert(args.trace), ensure_ascii=False, indent=2, allow_nan=False) + "\n"
        if args.output:
            with Path(args.output).open("x", encoding="utf-8") as output:
                output.write(encoded)
        else:
            sys.stdout.write(encoded)
    except (OSError, UnicodeError, ValueError, trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
