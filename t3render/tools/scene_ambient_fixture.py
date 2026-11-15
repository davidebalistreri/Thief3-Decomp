#!/usr/bin/env python3
"""Exercise constant ambient and diffuse modulation on the SceneIR DX12 backend."""
import argparse
from pathlib import Path
import struct
import sys

from scene_legacy_states import DEFAULT, FIELDS
from scene_texture_fixture import IDENTITY

EXPECTED = {(16, 32): (32, 32, 24), (48, 32): (128, 64, 32)}


def fixture():
    body = bytearray(struct.pack("<8sIIff32fII", b"T3SCN04\0", 64, 64, 0, 1,
                                 *IDENTITY, *IDENTITY, 1, 3))
    body.extend(struct.pack("<III4B", 1, 1, 1, 128, 64, 32, 255))

    def quad(left, right, depth, state, ambient, color):
        body.extend(struct.pack("<13IfI4f", 0, 6, *(state[field] for field in FIELDS), 0, ambient, *color))
        for x, y in ((left, -1), (left, 1), (right, 1), (left, -1), (right, 1), (right, -1)):
            body.extend(struct.pack("<5f", x, y, depth, .5, .5))
    quad(-1, 1, .5, dict(DEFAULT), 1, (.125, .25, .375, 1))
    quad(-1, 1, .5, dict(DEFAULT, depth_compare=3, depth_write=0,
                          blend_enable=1, blend_src=6, blend_dst=2), 0, (0, 0, 0, 0))
    quad(0, 1, .25, dict(DEFAULT), 0, (0, 0, 0, 0))
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
            print(f"ambient_modulation=PASS pixels={verify(args.path.read_bytes())}")
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
