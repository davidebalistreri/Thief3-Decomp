#!/usr/bin/env python3
"""GPU-check interpolation, clamping and source-alpha fog composition."""
import argparse
from pathlib import Path
import struct
import sys

from scene_legacy_states import DEFAULT, FIELDS
from scene_texture_fixture import IDENTITY

EXPECTED = {(8, 32): (32, 64, 96), (24, 32): (88, 48, 72),
            (40, 32): (199, 16, 24), (56, 32): (255, 0, 0)}


def fixture():
    body = bytearray(struct.pack("<8sIIff32fII", b"T3SCN08\0", 64, 64, 0, 1,
                                 *IDENTITY, *IDENTITY, 1, 2))
    body.extend(struct.pack("<III4B", 1, 1, 1, 255, 255, 255, 255))

    def quad(state, mode, color, light, fog):
        body.extend(struct.pack("<13IfI4f8f12fI", 0, 6, *(state[field] for field in FIELDS), 0, mode,
                                *color, *light, *((0,) * 12), 0))
        for x, y in ((-1, -1), (-1, 1), (1, 1), (-1, -1), (1, 1), (1, -1)):
            body.extend(struct.pack("<8f", x, y, .5, .5, .5, (x + .5) if fog else 0, 0, 0))
    quad(dict(DEFAULT), 1, (.125, .25, .375, 1), (0,) * 8, False)
    quad(dict(DEFAULT, depth_compare=3, depth_write=0, blend_enable=1, blend_src=4, blend_dst=5, color_mask=7),
         6, (0,) * 4, (0, 0, 0, 0, 1, 0, 0, 0), True)
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
            print(f"fog_interpolation_blending=PASS pixels={verify(args.path.read_bytes())}")
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
