#!/usr/bin/env python3
"""GPU-check the explicitly approximate projected spotlight preview."""
import argparse
from pathlib import Path
import struct
import sys

from scene_legacy_states import DEFAULT, FIELDS
from scene_texture_fixture import IDENTITY

EXPECTED = {(32, 32): (136, 58, 31), (4, 4): (32, 32, 24)}


def fixture():
    body = bytearray(struct.pack("<8sIIff32fII", b"T3SCN06\0", 64, 64, 0, 1,
                                 *IDENTITY, *IDENTITY, 1, 3))
    body.extend(struct.pack("<III4B", 1, 1, 1, 128, 64, 32, 255))
    projector = (499.75, 0, -.5, 500, 0, 499.75, -.5, 500, 0, 0, -1, 1000)

    def quad(state, mode, color, light, rows, normal):
        body.extend(struct.pack("<13IfI4f8f12f", 0, 6, *(state[field] for field in FIELDS), 0, mode,
                                *color, *light, *rows))
        for x, y in ((-1, -1), (-1, 1), (1, 1), (-1, -1), (1, 1), (1, -1)):
            body.extend(struct.pack("<8f", x, y, .5, .5, .5, *normal))
    quad(dict(DEFAULT), 1, (.125, .25, .375, 1), (0,) * 8, (0,) * 12, (0, 0, 0))
    quad(dict(DEFAULT, depth_compare=3, depth_write=0, blend_enable=1, blend_src=1, blend_dst=1, color_mask=7),
         4, (0,) * 4, (0, 0, 1000, .0001, 1, .5, .25, .5), projector, (0, 0, 1))
    quad(dict(DEFAULT, depth_compare=3, depth_write=0, blend_enable=1, blend_src=6, blend_dst=2),
         0, (0,) * 4, (0,) * 8, (0,) * 12, (0, 0, 0))
    return bytes(body)


def verify(image):
    header = b"P6\n64 64\n255\n"
    if not image.startswith(header) or len(image) != len(header) + 64 * 64 * 3:
        raise ValueError("invalid fixture image")
    for (x, y), expected in EXPECTED.items():
        offset = len(header) + (y * 64 + x) * 3
        actual = image[offset:offset + 3]
        if any(abs(a - b) > 2 for a, b in zip(actual, expected)):
            raise ValueError(f"wrong pixel at {x},{y}: {tuple(actual)}, expected {expected}")
    return len(EXPECTED)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("path", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "create":
            with args.path.open("xb") as output:
                output.write(fixture())
        else:
            print(f"spot_projection=PASS pixels={verify(args.path.read_bytes())}")
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
