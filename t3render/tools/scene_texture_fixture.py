#!/usr/bin/env python3
"""Create and check texture, UV, filtering, mips, wrap and depth on DX12."""
import argparse
from pathlib import Path
import struct
import sys

IDENTITY = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
EXPECTED = {(16, 16): (255, 0, 0), (48, 16): (0, 255, 0),
            (16, 48): (0, 0, 255), (48, 48): (255, 0, 0), (32, 32): (128, 128, 64),
            (32, 8): (255, 0, 255)}


def fixture():
    data = bytearray(struct.pack("<8sIIff32fII", b"T3SCN01\0", 64, 64, 0, 1, *IDENTITY, *IDENTITY, 2, 7))
    data.extend(struct.pack("<III", 2, 2, 1))
    data.extend(bytes((255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255)))
    data.extend(struct.pack("<III", 2, 2, 2))
    data.extend(bytes((255, 255, 255, 255)) * 4 + bytes((255, 0, 255, 255)))
    for left, bottom, right, top, depth, uv in ((-1, 0, 0, 1, .5, (.25, .25)), (0, 0, 1, 1, .5, (.75, .25)),
            (-1, -1, 0, 0, .5, (.25, .75)), (0, -1, 1, 0, .5, (1.25, .25)),
            (-.25, -.25, .25, .25, .25, (.5, .5)), (-.25, -.25, .25, .25, .75, (.75, .75))):
        bl, tl, tr, br = ((left, bottom), (left, top), (right, top), (right, bottom))
        data.extend(struct.pack("<II", 0, 6))
        for x, y in (bl, tl, tr, bl, tr, br):
            data.extend(struct.pack("<5f", x, y, depth, *uv))
    data.extend(struct.pack("<II", 1, 6))
    for x, y, u, v in ((-.25, .5, 0, 0), (-.25, 1, 0, 32), (.25, 1, 32, 32),
                        (-.25, .5, 0, 0), (.25, 1, 32, 32), (.25, .5, 32, 0)):
        data.extend(struct.pack("<5f", x, y, .1, u, v))
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
            print(f"texture_uv_wrap_filter_mips_depth=PASS pixels={verify(args.path.read_bytes())}")
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
