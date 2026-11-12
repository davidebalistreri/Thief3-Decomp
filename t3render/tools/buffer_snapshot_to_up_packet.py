#!/usr/bin/env python3
"""Convert a controlled synthetic buffer snapshot into a one-triangle UP packet.

The input is a fixture-generated snapshot read through driver locks, not a TDS
frame or proxy trace. Its format does not contain render state: the caller's
explicit attestation is required and cannot be verified by this converter.
The format has no checksum, so not every byte corruption can be detected.
This is neither a general buffer reader nor a buffered DX12 replay.
"""

import argparse
import math
from pathlib import Path
import re
import struct
import sys


MAX_INPUT_BYTES = 4096
WIDTH = HEIGHT = 256


class SnapshotError(ValueError):
    """The snapshot is missing, inconsistent or outside this fixture's scope."""


def require(condition, message):
    if not condition:
        raise SnapshotError(message)


def uints(line, count, name):
    fields = line.split()
    require(len(fields) == count, f"invalid {name} field count")
    values = []
    for field in fields:
        require(re.fullmatch(r"[0-9]{1,10}", field) is not None, f"invalid {name} integer")
        value = int(field)
        require(value <= 0xffffffff, f"{name} integer exceeds uint32")
        values.append(value)
    return tuple(values)


def snapshot_lines(path):
    with Path(path).open("rb") as source:
        data = source.read(MAX_INPUT_BYTES + 1)
    require(len(data) <= MAX_INPUT_BYTES, "snapshot exceeds size limit")
    text = data.decode("ascii")
    require(all(ord(c) >= 32 or c in "\t\r\n" for c in text), "snapshot contains control bytes")
    lines = text.split("\n")
    if lines[-1] == "":
        lines.pop()
    lines = [line[:-1] if line.endswith("\r") else line for line in lines]
    require(len(lines) == 8 and all("\r" not in line for line in lines), "snapshot must have exactly eight lines")
    return lines


def convert(path, *, synthetic_fixture=False):
    require(synthetic_fixture is True, "--synthetic-fixture is required")
    lines = snapshot_lines(path)
    require(lines[0] == "T3R_BUFFER_V1", "unsupported snapshot header")
    require(uints(lines[1], 2, "viewport") == (WIDTH, HEIGHT), "fixture viewport must be 256x256")
    (clear,) = uints(lines[2], 1, "clear color")
    require(clear == 0xff000000, "fixture clear color must be opaque black")
    base, minimum, vertex_count, start_index = uints(lines[3], 4, "indexed draw")
    require((base, minimum, vertex_count, start_index) == (1, 1, 3, 2), "unsupported fixture draw parameters")
    stride, vb_offset, vb_length = uints(lines[4], 3, "vertex buffer")
    require((stride, vb_offset, vb_length) == (20, 20, 80), "unsupported fixture vertex buffer layout")
    ib_offset, ib_length = uints(lines[5], 2, "index buffer")
    require((ib_offset, ib_length) == (4, 6), "unsupported fixture index buffer layout")
    require(re.fullmatch(r"[0-9a-fA-F]{160}", lines[6]) is not None,
            "vertex buffer must contain exactly 80 hex-encoded bytes")
    require(re.fullmatch(r"[0-9a-fA-F]{12}", lines[7]) is not None,
            "index buffer must contain exactly 6 hex-encoded bytes")
    vb = bytes.fromhex(lines[6])
    ib = bytes.fromhex(lines[7])

    # StartIndex is in uint16 indices in the original IB; captured bytes begin
    # at ib_offset. BaseVertexIndex is added before finding a VB byte address.
    first_index = start_index * 2 - ib_offset
    require(first_index >= 0 and first_index + 3 * 2 <= len(ib), "index draw is outside captured bytes")
    indices = struct.unpack_from("<3H", ib, first_index)
    vertices = []
    for index in indices:
        require(minimum <= index < minimum + vertex_count, "index is outside the draw's vertex range")
        offset = (base + index) * stride - vb_offset
        require(offset >= 0 and offset + stride <= len(vb), "indexed vertex is outside captured bytes")
        x, y, z, rhw, color = struct.unpack_from("<4fI", vb, offset)
        require(all(math.isfinite(value) for value in (x, y, z, rhw)), "vertex contains non-finite floats")
        require(0 <= x <= WIDTH and 0 <= y <= HEIGHT and z == 0.0 and rhw == 1.0,
                "vertex is outside supported fixture space")
        require(color >> 24 == 0xff, "vertex color must be opaque")
        vertices.append((x, y, z, rhw, color))
    require(len({vertex[4] for vertex in vertices}) == 1, "vertex colors must be uniform")
    require(vertices[0][4] != clear, "triangle color must differ from clear color")
    (x0, y0, _, _, _), (x1, y1, _, _, _), (x2, y2, _, _, _) = vertices
    require((x1 - x0) * (y2 - y0) != (y1 - y0) * (x2 - x0), "triangle is degenerate")
    output = ["T3R_UP_V1", f"{WIDTH} {HEIGHT}", str(clear)]
    output.extend(f"{x:.9g} {y:.9g} {z:.9g} {rhw:.9g} {color}" for x, y, z, rhw, color in vertices)
    return "\n".join(output) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", help="controlled fixture T3R_BUFFER_V1 snapshot")
    parser.add_argument("--synthetic-fixture", action="store_true", required=True,
                        help="attest the known fixture and its render state")
    parser.add_argument("-o", "--output", help="new UP packet (default: stdout)")
    args = parser.parse_args(argv)
    try:
        packet = convert(args.snapshot, synthetic_fixture=args.synthetic_fixture)
        if args.output:
            with Path(args.output).open("x", encoding="ascii", newline="\n") as output:
                output.write(packet)
        else:
            sys.stdout.write(packet)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print("Controlled synthetic fixture only: render state and snapshot provenance are not verified; "
          "no checksum is available.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
