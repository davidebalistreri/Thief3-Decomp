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
import re
import struct
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


def nonnegative(value):
    value = integer(value)
    return value if value is not None and value >= 0 else None


def captured_hex(raw):
    """Only return data when the producer status and actual byte length agree."""
    status = raw.get("status")
    size = nonnegative(raw.get("byte_count"))
    value = raw.get("data")
    if (status not in ("captured", "empty") or size is None or raw.get("encoding") != "hex"
            or not isinstance(value, str) or len(value) != 2 * size
            or re.fullmatch(r"[0-9a-fA-F]*", value) is None
            or (status == "empty" and size != 0) or (status == "captured" and size == 0)):
        return None
    return status, size, value


def primitive_elements(draw):
    count = nonnegative(draw.get("primitive_count"))
    kind = integer(draw.get("primitive_type"))
    if count is None or kind not in TOPOLOGIES:
        return None
    if count == 0:
        return 0
    return {1: count, 2: count * 2, 3: count + 1, 4: count * 3, 5: count + 2, 6: count + 2}[kind]


def immediate_vertex(raw, draw, indexed):
    unknown = {"status": "unknown", "first_vertex": None, "vertex_count": None, "stride_bytes": None,
               "byte_offset": None, "byte_count": None, "encoding": None, "data": None}
    if not isinstance(raw, dict):
        return unknown
    data = captured_hex(raw)
    first = nonnegative(raw.get("first_vertex"))
    count = nonnegative(raw.get("vertex_count"))
    stride = nonnegative(raw.get("stride"))
    offset = nonnegative(raw.get("byte_offset"))
    if (data is None or first is None or count is None or stride is None or offset is None
            or first > 0xffffffff or stride > 0xffffffff or (count > 0 and stride == 0)
            or offset != first * stride or data[1] != count * stride):
        return unknown
    if "vertex_stride" in draw and nonnegative(draw["vertex_stride"]) != stride:
        return unknown
    if indexed:
        if first + count > 0x100000000:
            return unknown
        if "min_vertex_index" in draw and nonnegative(draw["min_vertex_index"]) != first:
            return unknown
        primitives = nonnegative(draw.get("primitive_count"))
        declared = nonnegative(draw.get("num_vertices"))
        if primitives == 0 and count != 0:
            return unknown
        if primitives is not None and primitives > 0 and declared is not None and count != declared:
            return unknown
    else:
        if first != 0:
            return unknown
        expected = primitive_elements(draw)
        if expected is not None and count != expected:
            return unknown
    return {"status": data[0], "first_vertex": first, "vertex_count": count, "stride_bytes": stride,
            "byte_offset": offset, "byte_count": data[1], "encoding": "hex", "data": data[2]}


def immediate_index(raw, draw):
    unknown = {"status": "unknown", "index_count": None, "index_format": None,
               "byte_count": None, "encoding": None, "data": None}
    if not isinstance(raw, dict):
        return unknown
    data = captured_hex(raw)
    count = nonnegative(raw.get("index_count"))
    fmt = integer(raw.get("index_format"))
    width = {101: 2, 102: 4}.get(fmt)
    if data is None or count is None or width is None or data[1] != count * width:
        return unknown
    if "index_format" in draw and integer(draw["index_format"]) != fmt:
        return unknown
    expected = primitive_elements(draw)
    if expected is not None and count != expected:
        return unknown
    return {"status": data[0], "index_count": count, "index_format": INDEX_FORMATS[fmt],
            "byte_count": data[1], "encoding": "hex", "data": data[2]}


def uint32(value):
    value = nonnegative(value)
    return value if value is not None and value <= 0xffffffff else None


def framed_declaration(words):
    """Check the producer's bounded token framing, not D3D8 shader semantics."""
    if not isinstance(words, list) or not 1 <= len(words) <= 1024:
        return False
    if any(uint32(word) is None for word in words):
        return False
    position = 0
    while position < len(words):
        token = words[position]
        position += 1
        if token == 0xffffffff:
            return position == len(words)
        kind = token >> 29
        if kind == 0:
            if token != 0:
                return False
            payload = 0
        elif kind in (1, 2, 3):
            payload = 0
        elif kind == 4:  # CONSTMEM payload can itself contain END words.
            payload = ((token >> 25) & 0x0f) * 4
        elif kind == 5:  # EXT payload words are opaque.
            payload = (token >> 24) & 0x1f
        else:
            return False
        if payload > len(words) - position:
            return False
        position += payload
    return False


# Metadata describes the latest conservative invalidation and the pre-create
# scanner result, not the driver's actual declaration or shader semantics.
DECLARATION_SCAN_STATUSES = frozenset(("complete", "invalid_configuration", "null_address", "address_overflow",
                                      "unreadable", "word_limit", "unsupported_token", "allocation_failure"))
DECLARATION_UNAVAILABLE_REASONS = frozenset(("scan_incomplete", "invalid_snapshot", "handle_collision",
                                             "reset_attempt", "delete_failed", "observation_error", "unknown"))


def observed_declaration(draw, result, header):
    """Publish only a paired, stable and structurally valid candidate."""
    stable = result.get("shader_stable") if isinstance(result.get("shader_stable"), bool) else None
    hr = integer(result.get("hr"))
    observed = {"status": "unknown", "shader_value": None, "generation": None, "words": None,
                "shader_stable": stable, "draw_hresult": hr}
    capability = object_or_empty(header.get("declaration_capture"))
    raw = object_or_empty(draw.get("vertex_declaration"))
    shader = uint32(draw.get("vertex_shader"))
    if (capability.get("requested") is not True or stable is not True or
            hresult_succeeded(hr) is not True or shader is None):
        return observed
    if not {"status", "generation", "words"}.issubset(raw):
        return observed
    status = raw.get("status")
    generation = integer(raw.get("generation"))
    if ((raw.get("generation") is not None and generation is None) or
            (generation is not None and not 1 <= generation <= 0xffffffffffffffff)):
        return observed
    words = raw.get("words")
    if status == "candidate":
        if generation is None or not framed_declaration(words):
            return observed
        observed.update(status=status, shader_value=shader, generation=generation, words=words[:])
    elif status in ("unknown", "unavailable", "unsafe") and words is None:
        observed.update(status=status, shader_value=shader, generation=generation)
        if status == "unavailable" and generation is not None:
            reason, scan_status = raw.get("reason"), raw.get("scan_status")
            if (isinstance(reason, str) and isinstance(scan_status, str) and
                    reason in DECLARATION_UNAVAILABLE_REASONS and scan_status in DECLARATION_SCAN_STATUSES and
                    (reason != "scan_incomplete" or scan_status != "complete") and
                    (reason not in ("invalid_snapshot", "observation_error") or scan_status == "complete")):
                observed.update(reason=reason, scan_status=scan_status)
    return observed


def pre_reset_frame_words(draw, header):
    """Account for every emitted history payload, including failed draws.

    An unmeasurable or malformed payload makes the frame's history unusable;
    the raw trace remains available in the compatibility extension.
    """
    if "vertex_declaration_pre_reset" not in draw:
        return 0
    raw = draw["vertex_declaration_pre_reset"]
    if not isinstance(raw, dict) or not framed_declaration(raw.get("words")):
        return None
    limit = uint32(object_or_empty(header.get("declaration_history")).get("words_per_observation"))
    if limit is None or len(raw["words"]) > limit:
        return None
    return len(raw["words"])


def pre_reset_declaration(draw, result, header, within_frame_budget):
    """Keep prior application-memory data explicitly separate from candidates."""
    capability = object_or_empty(header.get("declaration_history"))
    per_observation = uint32(capability.get("words_per_observation"))
    per_frame = uint32(capability.get("words_per_frame"))
    if (capability.get("requested") is not True or per_observation is None or
            not 1 <= per_observation <= 1024 or per_frame is None or
            not per_observation <= per_frame <= 262144 or not within_frame_budget):
        return None
    raw = draw.get("vertex_declaration_pre_reset")
    if not isinstance(raw, dict) or raw.get("status") != "hypothesis" or \
            raw.get("source") != "application_memory_before_reset":
        return None
    generation = integer(raw.get("generation"))
    reset_epoch = integer(raw.get("reset_epoch"))
    rebind_epoch = integer(raw.get("rebind_epoch"))
    if (generation is None or not 1 <= generation <= 0xffffffffffffffff or
            reset_epoch is None or not 1 <= reset_epoch <= 0xffffffffffffffff or
            rebind_epoch is None or not reset_epoch < rebind_epoch <= 0xffffffffffffffff or
            hresult_succeeded(raw.get("reset_hr")) is not True or
            not framed_declaration(raw.get("words")) or len(raw["words"]) > per_observation):
        return None
    observation = observed_declaration(draw, result, header)
    if (observation["status"] != "unavailable" or observation.get("reason") != "reset_attempt" or
            observation.get("scan_status") != "complete" or observation["generation"] != generation):
        return None
    return {"status": "hypothesis", "source": raw["source"], "shader_value": observation["shader_value"],
            "generation": generation, "words": raw["words"][:], "reset_epoch": reset_epoch,
            "reset_hr": raw["reset_hr"], "rebind_epoch": rebind_epoch,
            "shader_stable": observation["shader_stable"], "draw_hresult": observation["draw_hresult"]}


def framed_vertex_program(words):
    """Frame the supported VS 1.0/1.1 token subset, without validating semantics."""
    if not isinstance(words, list) or not 2 <= len(words) <= 1024 or any(uint32(w) is None for w in words):
        return False
    if words[0] not in (0xfffe0100, 0xfffe0101):
        return False
    counts = {0: 0, 1: 2, 2: 3, 3: 3, 4: 4, 5: 3, 6: 2, 7: 2, 8: 3, 9: 3, 10: 3,
              11: 3, 12: 3, 13: 3, 14: 2, 15: 2, 16: 2, 17: 3, 18: 4, 19: 2,
              20: 3, 21: 3, 22: 3, 23: 3, 24: 3, 78: 2, 79: 2, 81: 5}
    pos = 1
    while pos < len(words):
        token = words[pos]
        pos += 1
        if token == 0xffff:
            return pos == len(words)
        if token & 0xffff == 0xfffe and not token & 0x80000000:
            count = (token >> 16) & 0x7fff
            if count > len(words) - pos:
                return False
            pos += count
            continue
        count = counts.get(token)
        if count is None or count > len(words) - pos:
            return False
        for i in range(count):
            if (token != 81 or i == 0) and not words[pos + i] & 0x80000000:
                return False
        pos += count
    return False


PROGRAM_UNAVAILABLE_REASONS = frozenset(("null_address", "invalid_configuration", "address_overflow", "unreadable",
                                         "unsupported_version", "unsupported_instruction", "invalid_parameter",
                                         "word_limit", "allocation_failure", "invalid_snapshot", "frame_word_limit"))


def vertex_program_frame_words(draw, header):
    total = 0
    for field in ("vertex_program", "vertex_program_pre_reset"):
        if field not in draw:
            continue
        raw = draw[field]
        if (field == "vertex_program" and isinstance(raw, dict) and raw.get("status") == "unavailable" and
                raw.get("words", "missing") is None and isinstance(raw.get("reason"), str) and
                raw["reason"] in PROGRAM_UNAVAILABLE_REASONS):
            continue
        if not isinstance(raw, dict) or not framed_vertex_program(raw.get("words")):
            return None
        limit = uint32(object_or_empty(header.get("vertex_program_capture")).get("words_per_observation"))
        if limit is None or len(raw["words"]) > limit:
            return None
        total += len(raw["words"])
    return total


def observed_vertex_program(draw, result, header, within_frame_budget):
    capability = object_or_empty(header.get("vertex_program_capture"))
    per_observation = uint32(capability.get("words_per_observation"))
    per_frame = uint32(capability.get("words_per_frame"))
    if (capability.get("requested") is not True or per_observation is None or
            not 1 <= per_observation <= 1024 or per_frame is None or
            not per_observation <= per_frame <= 262144 or not within_frame_budget):
        return None
    raw = draw.get("vertex_program")
    if not isinstance(raw, dict):
        return None
    generation = integer(raw.get("generation"))
    if generation is None or not 1 <= generation <= 0xffffffffffffffff:
        return None
    declaration = observed_declaration(draw, result, header)
    if declaration["status"] != "candidate" or declaration["generation"] != generation:
        return None
    if (raw.get("status") == "unavailable" and raw.get("words", "missing") is None and
            isinstance(raw.get("reason"), str) and raw["reason"] in PROGRAM_UNAVAILABLE_REASONS):
        return {"status": "unavailable", "reason": raw["reason"], "shader_value": declaration["shader_value"],
                "generation": generation, "words": None, "shader_stable": declaration["shader_stable"],
                "draw_hresult": declaration["draw_hresult"]}
    if (raw.get("status") != "candidate" or not framed_vertex_program(raw.get("words")) or
            len(raw["words"]) > per_observation or
            raw.get("version") != {0xfffe0100: "vs_1_0", 0xfffe0101: "vs_1_1"}[raw["words"][0]]):
        return None
    return {"status": "candidate", "version": raw["version"], "shader_value": declaration["shader_value"],
            "generation": generation, "words": raw["words"][:],
            "shader_stable": declaration["shader_stable"], "draw_hresult": declaration["draw_hresult"]}


def pre_reset_vertex_program(draw, result, header, program_budget, history_budget):
    capability = object_or_empty(header.get("vertex_program_capture"))
    per_observation = uint32(capability.get("words_per_observation"))
    per_frame = uint32(capability.get("words_per_frame"))
    if (capability.get("requested") is not True or per_observation is None or
            not 1 <= per_observation <= 1024 or per_frame is None or
            not per_observation <= per_frame <= 262144 or not program_budget):
        return None
    raw = draw.get("vertex_program_pre_reset")
    if (not isinstance(raw, dict) or raw.get("status") != "hypothesis" or
            raw.get("source") != "application_memory_before_reset" or
            not framed_vertex_program(raw.get("words")) or len(raw["words"]) > per_observation):
        return None
    history = pre_reset_declaration(draw, result, header, history_budget)
    if history is None:
        return None
    for field in ("generation", "reset_epoch", "reset_hr", "rebind_epoch"):
        if integer(raw.get(field)) is None or raw[field] != history[field]:
            return None
    return {"status": "hypothesis", "source": raw["source"], "shader_value": history["shader_value"],
            "generation": history["generation"], "reset_epoch": history["reset_epoch"],
            "reset_hr": history["reset_hr"], "rebind_epoch": history["rebind_epoch"], "words": raw["words"][:],
            "shader_stable": history["shader_stable"], "draw_hresult": history["draw_hresult"]}


CONSTANT_UNAVAILABLE_REASONS = frozenset(("caps_unavailable", "invalid_count", "getter_failed", "verification_failed",
                                          "inconsistent_read", "frame_word_limit"))


def vertex_constant_frame_words(draw, header):
    if "vertex_constants" not in draw:
        return 0
    raw = draw["vertex_constants"]
    if not isinstance(raw, dict):
        return None
    if (raw.get("status") == "unavailable" and raw.get("bits", "missing") is None and
            isinstance(raw.get("reason"), str) and raw["reason"] in CONSTANT_UNAVAILABLE_REASONS):
        return 0
    count = uint32(raw.get("count"))
    limit = uint32(object_or_empty(header.get("vertex_constant_capture")).get("registers_per_observation"))
    bits = raw.get("bits")
    if (raw.get("status") != "sampled" or uint32(raw.get("start_register")) != 0 or
            count is None or limit is None or not 1 <= count <= min(limit, 96) or
            not isinstance(bits, list) or len(bits) != count * 4 or any(uint32(v) is None for v in bits)):
        return None
    return len(bits)


def observed_vertex_constants(draw, result, header, frame_begin, within_frame_budget):
    capability = object_or_empty(header.get("vertex_constant_capture"))
    limit = uint32(capability.get("registers_per_observation"))
    frame_limit = uint32(capability.get("words_per_frame"))
    if (capability.get("requested") is not True or limit is None or not 1 <= limit <= 96 or
            frame_limit is None or not limit * 4 <= frame_limit <= 262144 or not within_frame_budget or
            object_or_empty(header.get("declaration_capture")).get("requested") is not True or
            result.get("shader_stable") is not True or hresult_succeeded(result.get("hr")) is not True or
            uint32(draw.get("vertex_shader")) is None):
        return None
    raw = draw.get("vertex_constants")
    if not isinstance(raw, dict):
        return None
    base = {"shader_value": draw["vertex_shader"], "shader_stable": True, "draw_hresult": result["hr"]}
    caps = uint32(object_or_empty(frame_begin.get("caps")).get("vertex_shader_constants"))
    if (raw.get("status") == "unavailable" and raw.get("bits", "missing") is None and
            isinstance(raw.get("reason"), str) and raw["reason"] in CONSTANT_UNAVAILABLE_REASONS):
        count = uint32(raw.get("count"))
        first = hresult_succeeded(raw.get("getter_hr"))
        second = hresult_succeeded(raw.get("verify_hr"))
        reason = raw["reason"]
        if count is None or first is None or second is None:
            return None
        if reason == "caps_unavailable":
            if count != 0 or (caps is not None and caps != 0) or first is not False or second is not False: return None
        elif reason == "invalid_count":
            if count <= limit or first is not False or second is not False: return None
        elif (caps is None or count != min(caps, limit) or count == 0 or
              (reason == "getter_failed" and (first is not False or second is not False)) or
              (reason == "verification_failed" and (first is not True or second is not False)) or
              (reason in ("inconsistent_read", "frame_word_limit") and (first is not True or second is not True))):
            return None
        return {**base, "status": "unavailable", "reason": reason, "count": count, "bits": None}
    if (vertex_constant_frame_words(draw, header) is None or raw.get("status") != "sampled" or
            caps is None or raw["count"] != min(caps, limit) or
            hresult_succeeded(raw.get("getter_hr")) is not True or hresult_succeeded(raw.get("verify_hr")) is not True):
        return None
    return {**base, "status": "sampled", "start_register": 0, "count": raw["count"], "bits": raw["bits"][:]}


def buffer_binding(record):
    """Validate a getter's resource identity and declared byte size."""
    if not isinstance(record, dict) or hresult_succeeded(record.get("hr")) is not True:
        return None
    token = record.get("buffer")
    desc = record.get("description")
    if (not isinstance(token, str) or re.fullmatch(r"0x[0-9a-fA-F]{1,16}", token) is None
            or int(token[2:], 16) == 0 or not isinstance(desc, dict)):
        return None
    size = uint32(desc.get("size"))
    return (token, size) if size is not None else None


def buffered_vertex(raw, draw, indexed, limit):
    unknown = {"status": "unknown", "resource": None, "stream": None, "first_vertex": None,
               "vertex_count": None, "stride_bytes": None, "byte_offset": None,
               "byte_count": None, "encoding": None, "data": None}
    if not isinstance(raw, dict) or not isinstance(draw.get("streams"), list):
        return unknown
    streams = [s for s in draw["streams"] if isinstance(s, dict) and uint32(s.get("stream")) == 0]
    if len(streams) != 1:
        return unknown
    stream = streams[0]
    bound = buffer_binding(stream)
    data = captured_hex(raw)
    first, count = uint32(raw.get("first_vertex")), nonnegative(raw.get("vertex_count"))
    stride, offset = uint32(raw.get("stride")), nonnegative(raw.get("byte_offset"))
    elements = primitive_elements(draw)
    primitives = uint32(draw.get("primitive_count"))
    if bound is None or data is None or first is None or count is None or stride is None or offset is None:
        return unknown
    if elements is None or primitives is None or uint32(raw.get("stream")) != 0:
        return unknown
    if indexed:
        ib = draw.get("indices")
        ib_bound = buffer_binding(ib)
        base = uint32(ib.get("base_vertex_index")) if isinstance(ib, dict) else None
        minimum, declared = uint32(draw.get("min_vertex_index")), uint32(draw.get("num_vertices"))
        if ib_bound is None or base is None or minimum is None or declared is None:
            return unknown
        expected_first, expected_count = base + minimum, declared if primitives else 0
    else:
        expected_first, expected_count = uint32(draw.get("start_vertex")), elements
    if (expected_first is None or expected_first > 0xffffffff or count != expected_count or first != expected_first
            or first + count > 0x100000000 or uint32(stream.get("stride")) != stride
            or raw.get("resource") != bound[0] or (count and not stride) or offset != first * stride
            or data[1] != count * stride or data[1] > limit or offset > bound[1] or data[1] > bound[1] - offset):
        return unknown
    return {"status": data[0], "resource": capture_id(bound[0]), "stream": 0, "first_vertex": first,
            "vertex_count": count, "stride_bytes": stride, "byte_offset": offset,
            "byte_count": data[1], "encoding": "hex", "data": data[2]}


def buffered_index(raw, draw, limit):
    unknown = {"status": "unknown", "resource": None, "index_count": None, "index_format": None,
               "byte_offset": None, "byte_count": None, "encoding": None, "data": None}
    if not isinstance(raw, dict):
        return unknown
    binding_record = draw.get("indices")
    bound = buffer_binding(binding_record)
    desc = object_or_empty(object_or_empty(binding_record).get("description"))
    data = captured_hex(raw)
    count, fmt, offset = nonnegative(raw.get("index_count")), uint32(raw.get("index_format")), nonnegative(raw.get("byte_offset"))
    start, primitives = uint32(draw.get("start_index")), uint32(draw.get("primitive_count"))
    minimum, vertices = uint32(draw.get("min_vertex_index")), uint32(draw.get("num_vertices"))
    elements = primitive_elements(draw)
    width = {101: 2, 102: 4}.get(fmt)
    if (bound is None or data is None or count is None or offset is None or start is None or
            primitives is None or minimum is None or vertices is None or elements is None or width is None):
        return unknown
    if (raw.get("resource") != bound[0] or uint32(desc.get("format")) != fmt or count != elements or
            minimum + vertices > 0x100000000 or offset != start * width or data[1] != count * width or
            data[1] > limit or offset > bound[1] or data[1] > bound[1] - offset):
        return unknown
    # An indexed draw can reference only the vertex range declared by the
    # application. This check does not decode vertices or make the frame replayable.
    values = struct.iter_unpack("<H" if width == 2 else "<I", bytes.fromhex(data[2]))
    if any(not minimum <= value[0] < minimum + vertices for value in values):
        return unknown
    return {"status": data[0], "resource": capture_id(bound[0]), "index_count": count,
            "index_format": INDEX_FORMATS[fmt], "byte_offset": offset, "byte_count": data[1],
            "encoding": "hex", "data": data[2]}


def buffered_data(draw, result, header, indexed, within_frame_budget):
    unknown_vertex = buffered_vertex(None, draw, indexed, 0)
    unknown_index = buffered_index(None, draw, 0) if indexed else None
    raw = object_or_empty(draw.get("buffer_data"))
    capability = object_or_empty(header.get("buffer_capture"))
    limit = uint32(capability.get("component_limit_bytes"))
    frame_limit = uint32(capability.get("frame_limit_bytes"))
    stable = result.get("buffer_stable") if isinstance(result.get("buffer_stable"), bool) else None
    hr = integer(result.get("hr"))
    output = {"source": raw.get("source") if raw.get("source") == "application_lock_shadow" else None,
              "buffer_stable": stable, "draw_hresult": hr, "vertex": unknown_vertex, "index": unknown_index}
    if (raw.get("source") != "application_lock_shadow" or capability.get("requested") is not True or
            capability.get("encoding") != "hex" or limit is None or not 0 < limit <= 256 * 1024 or
            frame_limit is None or not limit <= frame_limit <= 8 * 1024 * 1024 or
            not within_frame_budget or stable is not True or hresult_succeeded(hr) is not True or
            (not indexed and ("index" not in raw or raw["index"] is not None))):
        return output
    output["vertex"] = buffered_vertex(raw.get("vertex"), draw, indexed, limit)
    if indexed:
        output["index"] = buffered_index(raw.get("index"), draw, limit)
    for component, unknown in (("vertex", unknown_vertex), ("index", unknown_index)):
        part = raw.get(component)
        if not isinstance(part, dict) or "shadow_observation" not in part:
            continue
        facade = object_or_empty(header.get("experimental_facade"))
        if (part["shadow_observation"] != "post_repeated_unlock_write" or
                facade.get("enabled") is not True or
                facade.get("repeated_unlock_resource_discard") is not True or
                facade.get("repeated_unlock_post_write_recovery") is not True):
            output[component] = unknown
        elif output[component] is not None and output[component]["status"] == "captured":
            output[component]["shadow_observation"] = "post_repeated_unlock_write"
    return output


def buffered_frame_bytes(draw):
    """Conservatively account for encoded snapshots even if a later draw fails.

    An impossible captured record still contributes its larger claimed or
    encoded size; if either cannot be measured, the frame budget is unknown.
    Other statuses contain no successfully emitted buffer bytes.
    """
    raw = object_or_empty(draw.get("buffer_data"))
    total = 0
    for name in ("vertex", "index"):
        part = raw.get(name)
        if not isinstance(part, dict) or part.get("status") not in ("captured", "empty"):
            continue
        claimed = nonnegative(part.get("byte_count"))
        encoded = part.get("data")
        if claimed is None or not isinstance(encoded, str):
            return None
        total += max(claimed, (len(encoded) + 1) // 2)
    return total


def mesh(draw, identifier, source, indexed):
    # UP data is supplied directly to the call; bound streams/indices are NOT
    # that data. Captured bytes are opaque snapshots, not decoded geometry.
    streams = vertex_streams(draw.get("streams")) if source == "buffered" else None
    indices = index_buffer(draw.get("indices")) if source == "buffered" and indexed is True else None
    immediate_data = None
    if source == "immediate":
        raw = object_or_empty(draw.get("up_data"))
        immediate_data = {"vertex": immediate_vertex(raw.get("vertex"), draw, indexed),
                          "index": immediate_index(raw.get("index"), draw) if indexed else None}
    return {"id": identifier, "geometry_available": False, "geometry_source": source,
            "vertex_streams_observed": streams, "index_buffer_observed": indices,
            "immediate_data_observed": immediate_data}


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
    buffered_pending = {}
    shader_pending = {}
    buffered_results = []
    frame_buffer_bytes = 0
    pre_reset_results = []
    frame_history_words = 0
    program_results = []
    frame_program_words = 0
    constant_results = []
    frame_constant_words = 0
    draw_index = 0
    for line_number, event in enumerate(records, 1):
        kind = event["event"]
        if kind == "header":
            continue
        if kind == "capture_end":
            ir["compatibility"]["d3d8"]["capture_end"] = event
        elif kind == "frame_begin":
            buffered_pending = {}
            shader_pending = {}
            buffered_results = []
            frame_buffer_bytes = 0
            pre_reset_results = []
            frame_history_words = 0
            program_results = []
            frame_program_words = 0
            constant_results = []
            frame_constant_words = 0
            current = {"frame": event["frame"], "commands": [],
                       "compatibility": {"d3d8": {"frame_begin": event, "events": [], "frame_end": None}}}
            ir["frames"].append(current)
        elif kind == "frame_end":
            limit = uint32(object_or_empty(records[0].get("buffer_capture")).get("frame_limit_bytes"))
            within_budget = frame_buffer_bytes is not None and limit is not None and frame_buffer_bytes <= limit
            for mesh_record, draw, result, indexed in buffered_results:
                mesh_record["buffered_data_observed"] = buffered_data(draw, result, records[0], indexed, within_budget)
            limit = uint32(object_or_empty(records[0].get("declaration_history")).get("words_per_frame"))
            within_history_budget = frame_history_words is not None and limit is not None and frame_history_words <= limit
            for command, draw, result in pre_reset_results:
                history = pre_reset_declaration(draw, result, records[0], within_history_budget)
                if history is not None:
                    command["vertex_declaration_pre_reset"] = history
            limit = uint32(object_or_empty(records[0].get("vertex_program_capture")).get("words_per_frame"))
            within_budget = frame_program_words is not None and limit is not None and frame_program_words <= limit
            for command, draw, result in program_results:
                program = observed_vertex_program(draw, result, records[0], within_budget)
                if program is not None:
                    command["vertex_program_observed"] = program
                program_history = pre_reset_vertex_program(draw, result, records[0], within_budget, within_history_budget)
                if program_history is not None:
                    command["vertex_program_pre_reset"] = program_history
            limit = uint32(object_or_empty(records[0].get("vertex_constant_capture")).get("words_per_frame"))
            within_budget = frame_constant_words is not None and limit is not None and frame_constant_words <= limit
            for command, draw, result in constant_results:
                constants = observed_vertex_constants(draw, result, records[0], current["compatibility"]["d3d8"]["frame_begin"], within_budget)
                if constants is not None:
                    command["vertex_constants_observed"] = constants
            current["compatibility"]["d3d8"]["frame_end"] = event
            current = None
        elif kind == "draw":
            used_words = pre_reset_frame_words(event, records[0])
            frame_history_words = (frame_history_words + used_words
                                   if frame_history_words is not None and used_words is not None else None)
            used_words = vertex_program_frame_words(event, records[0])
            frame_program_words = (frame_program_words + used_words
                                   if frame_program_words is not None and used_words is not None else None)
            used_words = vertex_constant_frame_words(event, records[0])
            frame_constant_words = (frame_constant_words + used_words
                                    if frame_constant_words is not None and used_words is not None else None)
            if "buffer_data" in event:
                used = buffered_frame_bytes(event)
                frame_buffer_bytes = (frame_buffer_bytes + used
                                      if frame_buffer_bytes is not None and used is not None else None)
            source, indexed = CALLS.get(event["call"], (None, None))
            camera_id, mesh_id, material_id = f"camera:{draw_index}", f"mesh:{draw_index}", f"material:{draw_index}"
            ir["cameras"].append(camera(event, camera_id))
            mesh_record = mesh(event, mesh_id, source, indexed)
            ir["meshes"].append(mesh_record)
            if source == "buffered" and "buffer_data" in event:
                buffered_pending[event["op_id"]] = (mesh_record, event, indexed)
            ir["materials"].append(material(event, material_id))
            transforms = object_or_empty(event.get("transforms"))
            command = {
                "id": f"draw:{draw_index}", "source_line": line_number,
                "camera": camera_id, "mesh": mesh_id, "material": material_id,
                "lights": observed_lights(event, ir, draw_index),
                "topology": TOPOLOGIES.get(integer(event.get("primitive_type"))), "indexed": indexed,
                "parameters": command_parameters(event, source, indexed),
                "transforms_observed": {"world": matrix_list(transforms.get("world")),
                                        "texture": matrix_list(transforms.get("texture"))},
                "compatibility": {"d3d8": {"draw": event}},
            }
            current["commands"].append(command)
            if "op_id" in event:
                shader_pending[event["op_id"]] = (command, event)
            draw_index += 1
        else:
            if kind == "draw_result" and event.get("op_id") in shader_pending:
                command, draw = shader_pending.pop(event["op_id"])
                if "vertex_declaration" in draw or "shader_stable" in event:
                    command["vertex_declaration_observed"] = observed_declaration(draw, event, records[0])
                if "vertex_declaration_pre_reset" in draw:
                    pre_reset_results.append((command, draw, event))
                if "vertex_program" in draw or "vertex_program_pre_reset" in draw:
                    program_results.append((command, draw, event))
                if "vertex_constants" in draw:
                    constant_results.append((command, draw, event))
            if kind == "draw_result" and event.get("op_id") in buffered_pending:
                mesh_record, draw, indexed = buffered_pending.pop(event["op_id"])
                buffered_results.append((mesh_record, draw, event, indexed))
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
