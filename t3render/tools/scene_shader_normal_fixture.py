#!/usr/bin/env python3
"""GPU-check the sampled shader-basis hypothesis without TDS assets."""
import argparse
from pathlib import Path
import struct
import sys

from scene_legacy_states import DEFAULT, FIELDS
from scene_texture_fixture import IDENTITY

EXPECTED = {(5, 32): (135, 84, 58), (15, 32): (84, 58, 45),
            (25, 32): (98, 65, 49), (35, 32): (32, 32, 32),
            (45, 32): (84, 58, 45), (55, 32): (135, 84, 58)}


def fixture():
    data = bytearray(struct.pack("<8sIIff32fII", b"T3SCN10\0", 64, 64, 0, 1, *IDENTITY, *IDENTITY, 4, 7))
    for rgba in ((255, 255, 255, 255), (128, 128, 192, 255), (255, 128, 128, 255), (255, 255, 255, 255)):
        data.extend(struct.pack("<III4B", 1, 1, 1, *rgba))
    def quad(left, right, texture, state, mode, color, light, rows, projector, tangent):
        data.extend(struct.pack("<13IfI4f8f12fI", texture, 6, *(state[field] for field in FIELDS), 0, mode,
                                *color, *light, *rows, projector))
        for x, y in ((left, -1), (left, 1), (right, 1), (left, -1), (right, 1), (right, -1)):
            data.extend(struct.pack("<12f", x, y, .5, .5, .5, 0, 0, 1, *tangent))
    quad(-1, 1, 0, dict(DEFAULT), 1, (.125, .125, .125, 1), (0,) * 8, (0,) * 12, 0, (0, 0, 0, 0))
    additive = dict(DEFAULT, depth_compare=3, depth_write=0, blend_enable=1, blend_src=1, blend_dst=1, color_mask=7)
    point = (0, 0, 1000, .0001, 1, .5, .25, .5)
    diagonal = (1000, 0, 1000, .0001, 1, .5, .25, .5)
    rows = (0, 0, -.5, 500, 0, 0, -.5, 500, 0, 0, -1, 1000)
    draws = ((1, 7, point, (1, 0, 0, 1)), (1, 10, point, (1, 0, 0, 1)),
             (2, 10, diagonal, (1, 0, 0, 1)), (2, 10, diagonal, (-1, 0, 0, 1)),
             (1, 12, point, (1, 0, 0, 1)), (1, 10, point, (0, 0, 0, 0)))
    for i, (texture, mode, light, tangent) in enumerate(draws):
        left, right = -1 + 20*i/64, -1 + 20*(i+1)/64
        quad(left, right, texture, additive, mode, (0,) * 4, light, rows if mode == 12 else (0,) * 12,
             3 if mode == 12 else 0, tangent)
    return bytes(data)


def verify(image):
    header = b"P6\n64 64\n255\n"
    if not image.startswith(header) or len(image) != len(header) + 64*64*3:
        raise ValueError("invalid fixture image")
    for (x, y), expected in EXPECTED.items():
        offset = len(header) + (y*64+x)*3
        actual = tuple(image[offset:offset+3])
        if any(abs(a-b) > 2 for a, b in zip(actual, expected)):
            raise ValueError(f"wrong pixel at {x},{y}: {actual}, expected {expected}")
    return len(EXPECTED)


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
            print(f"sampled_shader_normal_basis=PASS pixels={verify(args.path.read_bytes())}")
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
