#!/usr/bin/env python3
"""Exercise positive and negative per-draw mip LOD bias on the DX12 path."""
import argparse
from pathlib import Path
import struct
import sys

from scene_legacy_states import DEFAULT, FIELDS
from scene_texture_fixture import IDENTITY

EXPECTED = {(8, 32): (0, 255, 0), (32, 32): (255, 0, 0), (56, 32): (0, 0, 255)}


def fixture():
    data = bytearray(struct.pack("<8sIIff32fII", b"T3SCN03\0", 64, 64, 0, 1, *IDENTITY, *IDENTITY, 1, 3))
    data.extend(struct.pack("<III", 4, 4, 3))
    data.extend(bytes((255, 0, 0, 255)) * 16)
    data.extend(bytes((0, 255, 0, 255)) * 4)
    data.extend(bytes((0, 0, 255, 255)))
    for left, right, bias in ((-1, -.5, 0), (-.25, .25, -1), (.5, 1, 1)):
        data.extend(struct.pack("<13If", 0, 6, *(DEFAULT[field] for field in FIELDS), bias))
        for x, y, u, v in ((left, -1, 0, 0), (left, 1, 0, 32), (right, 1, 8, 32),
                            (left, -1, 0, 0), (right, 1, 8, 32), (right, -1, 8, 0)):
            data.extend(struct.pack("<5f", x, y, .5, u, v))
    return bytes(data)


def verify(image):
    header = b"P6\n64 64\n255\n"
    if not image.startswith(header) or len(image) != len(header) + 64 * 64 * 3:
        raise ValueError("invalid fixture image")
    pixels = image[len(header):]
    actual = {}
    for (x, y), expected in EXPECTED.items():
        offset = (y * 64 + x) * 3
        color = tuple(pixels[offset:offset + 3])
        actual[f"{x},{y}"] = color
        if any(abs(a - b) > 2 for a, b in zip(color, expected)):
            raise ValueError(f"wrong pixel at {x},{y}: {color}, expected {expected}")
    return actual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("path", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "create":
            with args.path.open("xb") as stream:
                stream.write(fixture())
        else:
            print(f"sampler_lod_bias=PASS pixels={verify(args.path.read_bytes())}")
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
