#!/usr/bin/env python3
"""Convert one explicitly attested synthetic D3D8 fixture frame for DX12 replay.

This is deliberately not a general frame validator. The capture does not
observe every operation (including render-target changes and CopyRects). The
caller must use --synthetic-fixture to attest that the known isolated fixture
was the only producer of the frame; the checks below cover only observed data.
"""

import argparse
import io
import json
import math
from pathlib import Path
import re
import struct
import sys
import tempfile

import trace_summary


MAX_TRACE_BYTES = 4 * 1024 * 1024
WIDTH = HEIGHT = 256
FVF_XYZRHW_DIFFUSE = 0x44
FVF_XYZ_DIFFUSE = 0x42
TRIANGLE_LIST = 4
TARGET_CLEAR = 1
# D3D8 state IDs and values required by the single untextured fixture draw.
REQUIRED_RENDER_STATES = {
    "7": 0,    # ZENABLE: off
    "8": 3,    # FILLMODE: solid
    "9": 2,    # SHADEMODE: Gouraud
    "15": 0,   # ALPHATESTENABLE: off
    "22": 1,   # CULLMODE: none
    "26": 0,   # DITHERENABLE: off
    "27": 0,   # ALPHABLENDENABLE: off
    "28": 0,   # FOGENABLE: off
    "29": 0,   # SPECULARENABLE: off
    "52": 0,   # STENCILENABLE: off
    "137": 0,  # LIGHTING: off
    "151": 0,  # VERTEXBLEND: disabled
    "152": 0,  # CLIPPLANEENABLE: none
    "168": 15, # COLORWRITEENABLE: RGBA
}
STAGE0_STATES = {"1": 2, "2": 0, "4": 2, "5": 0}  # select diffuse RGB and alpha


class PacketError(ValueError):
    """Trace data is absent, unsupported, or inconsistent for this fixture."""


def integer(value, minimum=0, maximum=0xffffffff):
    if type(value) is not int or not minimum <= value <= maximum:
        return None
    return value


def number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return value if isinstance(value, int) or math.isfinite(value) else None


def hresult_succeeded(value):
    if type(value) is not int or not -(1 << 31) <= value <= 0xffffffff:
        return False
    return (value & 0x80000000) == 0


def require(condition, message):
    if not condition:
        raise PacketError(message)


def finite_float(value):
    result = float(value)
    if not math.isfinite(result):
        raise PacketError("trace contains a non-finite number")
    return result


def validated_records(path):
    """Validate and parse the same bounded immutable bytes to avoid races."""
    with Path(path).open("rb") as source:
        data = source.read(MAX_TRACE_BYTES + 1)
    require(len(data) <= MAX_TRACE_BYTES, "trace exceeds the fixture size limit")
    with tempfile.TemporaryDirectory(prefix="t3render-up-packet-") as directory:
        snapshot = Path(directory) / "trace.jsonl"
        snapshot.write_bytes(data)
        summary = trace_summary.summarize(snapshot)
    require(summary["frame_count"] == 1, "fixture requires exactly one complete frame")
    return [json.loads(line, parse_float=finite_float) for line in io.StringIO(data.decode("utf-8"), newline=None)]


def require_states(draw):
    states = draw.get("render_states")
    require(isinstance(states, dict), "render states are missing")
    for name, expected in REQUIRED_RENDER_STATES.items():
        require(integer(states.get(name)) == expected, f"unsupported or missing render state {name}")

    stages = draw.get("stages")
    require(isinstance(stages, list) and len(stages) >= 2, "texture stages are incomplete")
    seen = set()
    for stage in stages:
        require(isinstance(stage, dict), "invalid texture stage")
        index = integer(stage.get("stage"), maximum=7)
        require(index is not None and index not in seen, "invalid or duplicate texture stage")
        seen.add(index)
        require("texture" in stage and stage["texture"] is None and "texture_hr" not in stage,
                f"texture stage {index} is bound or its getter failed")
        values = stage.get("states")
        require(isinstance(values, dict), f"texture-stage states {index} are missing")
        expected = STAGE0_STATES if index == 0 else {"1": 1}  # disabled after stage zero
        for name, value in expected.items():
            require(integer(values.get(name)) == value, f"unsupported or missing texture-stage state {index}:{name}")
    require(0 in seen and 1 in seen, "texture stages 0 and 1 are required")


def vertex_data(draw, camera=False):
    up = draw.get("up_data")
    require(isinstance(up, dict) and up.get("index") is None and "index" in up,
            "non-indexed UP data is missing or inconsistent")
    vertex = up.get("vertex")
    require(isinstance(vertex, dict), "UP vertex data is missing")
    stride = 16 if camera else 20
    expected = {"status": "captured", "first_vertex": 0, "vertex_count": 3,
                "stride": stride, "byte_offset": 0, "byte_count": 3 * stride, "encoding": "hex"}
    for key, value in expected.items():
        actual = vertex.get(key)
        if type(value) is int:
            require(integer(actual) == value, f"unsupported UP vertex {key}")
        else:
            require(actual == value, f"unsupported UP vertex {key}")
    text = vertex.get("data")
    require(isinstance(text, str) and len(text) == 6 * stride and re.fullmatch(r"[0-9a-fA-F]+", text),
            f"UP vertex hex must contain exactly {3 * stride} complete bytes")
    raw = bytes.fromhex(text)
    vertices = []
    for index in range(3):
        if camera:
            x, y, z, color = struct.unpack_from("<3fI", raw, index * stride)
            rhw = 1.0
        else:
            x, y, z, rhw, color = struct.unpack_from("<4fI", raw, index * stride)
        require(all(math.isfinite(value) for value in (x, y, z, rhw)) and rhw > 0,
                f"vertex {index} contains invalid floats or rhw")
        # The initial DX12 comparison has only been designed for this bounded
        # screen-space case; do not imply general interpolation parity.
        if camera:
            require(max(abs(x), abs(y), abs(z)) <= 10000,
                    f"vertex {index} is outside the supported fixture space")
        else:
            require(0 <= x <= WIDTH and 0 <= y <= HEIGHT and z == 0.0 and rhw == 1.0,
                    f"vertex {index} is outside the supported fixture space")
        require(color >> 24 == 0xff, f"vertex {index} must have opaque alpha")
        vertices.append((x, y, z, rhw, color))
    require(len({vertex[4] for vertex in vertices}) == 1, "fixture requires uniform vertex color")
    return vertices


def camera_matrices(draw):
    transforms = draw.get("transforms")
    require(isinstance(transforms, dict), "camera transforms are missing")
    world = transforms.get("world")
    require(isinstance(world, list) and world, "world transform is missing")
    result = []
    for name, source in (("world", world[0]), ("view", transforms.get("view")),
                         ("projection", transforms.get("projection"))):
        require(isinstance(source, list) and len(source) == 16, f"{name} transform must have 16 values")
        matrix = []
        for value in source:
            require(number(value) is not None, f"{name} transform contains invalid values")
            try:
                value = float(value)
            except (OverflowError, ValueError):
                raise PacketError(f"{name} transform contains invalid values") from None
            require(math.isfinite(value) and abs(value) <= 10000, f"{name} transform is out of range")
            matrix.append(struct.unpack("<f", struct.pack("<f", value))[0])
        result.append(matrix)
    return result


def validate_camera_triangle(vertices, matrices):
    projected = []
    for x, y, z, _, _ in vertices:
        vector = [x, y, z, 1.0]
        for matrix in matrices:
            vector = [sum(vector[row] * matrix[row * 4 + col] for row in range(4)) for col in range(4)]
        x, y, z, w = vector
        require(all(math.isfinite(v) and abs(v) <= 1e12 for v in vector) and
                w > 1e-5 and -w <= x <= w and -w <= y <= w and 0 <= z <= w,
                "camera triangle must be finite and wholly inside the clip volume")
        projected.append((x / w, y / w))
    (x0, y0), (x1, y1), (x2, y2) = projected
    require(abs((x1-x0)*(y2-y0)-(y1-y0)*(x2-x0)) > 1e-8,
            "camera triangle is degenerate")


def convert(path, *, synthetic_fixture=False):
    require(synthetic_fixture is True, "--synthetic-fixture is required; general frame replay is not supported")
    records = validated_records(path)
    return _convert_records(records)


def _convert_records(records):
    """Validate one draw from an already validated, explicitly attested fixture."""
    expected = ["header", "frame_begin", "clear", "clear_result", "draw", "draw_result", "frame_end", "capture_end"]
    require([record.get("event") for record in records] == expected,
            "fixture requires exactly one ordered clear and one draw; observed extra events are unsupported")
    header, begin, clear, clear_result, draw, draw_result, end, _ = records
    camera = integer(draw.get("vertex_shader")) == FVF_XYZ_DIFFUSE
    require(integer(header.get("operation_results")) == 1, "operation result capture capability is missing")
    frame = integer(begin.get("frame"))
    require(frame is not None, "invalid frame identifier")
    for record in (clear, clear_result, draw, draw_result, end):
        require(integer(record.get("frame")) == frame, "operation frame does not match frame_begin")
    clear_id = integer(clear.get("op_id"), minimum=1)
    draw_id = integer(draw.get("op_id"), minimum=1)
    require(clear_id is not None and draw_id is not None and draw_id > clear_id and
            integer(clear_result.get("op_id"), minimum=1) == clear_id and
            integer(draw_result.get("op_id"), minimum=1) == draw_id,
            "clear/draw operation identifiers are invalid or unmatched")
    require(hresult_succeeded(clear_result.get("hr")) and hresult_succeeded(draw_result.get("hr")) and
            hresult_succeeded(end.get("present_hr")), "clear, draw or Present did not succeed")
    require(end.get("concurrent_operations") is False and end.get("had_reset") is False,
            "concurrent operations or reset are present or not recorded")
    require(integer(end.get("draws")) == 1, "frame draw count is missing or differs from one")

    presentation = begin.get("presentation")
    require(isinstance(presentation, dict), "presentation parameters are missing")
    require(integer(presentation.get("width")) == WIDTH and integer(presentation.get("height")) == HEIGHT,
            "fixture backbuffer must be 256x256")
    require(integer(presentation.get("format")) in (21, 22), "unsupported backbuffer format")
    require(integer(presentation.get("multisample")) == 0, "multisampled backbuffer is unsupported")
    require(integer(presentation.get("count")) == 1 and presentation.get("windowed") is True,
            "fixture requires one windowed backbuffer")

    require(integer(clear.get("rect_count")) == 0 and clear.get("rects_status") == "none" and
            clear.get("rects") == [] and integer(clear.get("flags")) == TARGET_CLEAR,
            "fixture requires one unrestricted color-target clear")
    color = integer(clear.get("color"))
    require(color is not None and color >> 24 == 0xff, "clear ARGB color must be recorded and opaque")

    stride = 16 if camera else 20
    fvf = FVF_XYZ_DIFFUSE if camera else FVF_XYZRHW_DIFFUSE
    require(draw.get("call") == "DrawPrimitiveUP" and integer(draw.get("primitive_type")) == TRIANGLE_LIST and
            integer(draw.get("primitive_count")) == 1 and integer(draw.get("vertex_stride")) == stride,
            "fixture requires one three-vertex DrawPrimitiveUP triangle list")
    require(integer(draw.get("vertex_shader")) == fvf and integer(draw.get("pixel_shader")) == 0,
            "fixture requires the expected FVF and the fixed-function pixel shader")
    viewport = draw.get("viewport")
    require(isinstance(viewport, dict) and integer(viewport.get("x")) == 0 and integer(viewport.get("y")) == 0 and
            integer(viewport.get("width")) == WIDTH and integer(viewport.get("height")) == HEIGHT and
            number(viewport.get("min_z")) == 0 and number(viewport.get("max_z")) == 1,
            "fixture requires a full 256x256 viewport with depth range [0,1]")
    require_states(draw)
    vertices = vertex_data(draw, camera=camera)
    require(vertices[0][4] != color, "triangle color must differ from the clear color")
    lines = ["T3R_XYZ_V1" if camera else "T3R_UP_V1", f"{WIDTH} {HEIGHT}", str(color)]
    if camera:
        matrices = camera_matrices(draw)
        validate_camera_triangle(vertices, matrices)
        lines.extend(" ".join(f"{value:.9g}" for value in matrix) for matrix in matrices)
        for x, y, z, _, argb in vertices:
            lines.append(f"{x:.9g} {y:.9g} {z:.9g} {argb}")
    else:
        x0, y0, _, _, _ = vertices[0]
        x1, y1, _, _, _ = vertices[1]
        x2, y2, _, _, _ = vertices[2]
        require((x1 - x0) * (y2 - y0) != (y1 - y0) * (x2 - x0), "triangle is degenerate")
        for x, y, z, rhw, argb in vertices:
            lines.append(f"{x:.9g} {y:.9g} {z:.9g} {rhw:.9g} {argb}")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", help="complete diagnostic JSONL trace")
    parser.add_argument("--synthetic-fixture", action="store_true",
                        help="attest that the frame came only from the known isolated fixture")
    parser.add_argument("-o", "--output", help="new output packet (default: stdout)")
    args = parser.parse_args(argv)
    try:
        packet = convert(args.trace, synthetic_fixture=args.synthetic_fixture)
        if args.output:
            with Path(args.output).open("x", encoding="ascii", newline="\n") as output:
                output.write(packet)
        else:
            sys.stdout.write(packet)
    except (OSError, UnicodeError, ValueError, trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print("Synthetic fixture only: unobserved D3D8 operations remain outside validation.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
