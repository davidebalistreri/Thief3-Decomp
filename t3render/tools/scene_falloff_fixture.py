#!/usr/bin/env python3
"""GPU-check optional point/spot falloff and old-packet compatibility."""
import argparse
from pathlib import Path
import struct
import sys

from scene_legacy_states import DEFAULT, FIELDS
from scene_texture_fixture import IDENTITY


def fixture(version=11):
    if version not in (10, 11):
        raise ValueError("fixture supports versions 10 and 11")
    data = bytearray(struct.pack("<8sIIff32fII", f"T3SCN{version:02d}".encode() + b"\0",
                                 64, 64, 0, 1, *IDENTITY, *IDENTITY, 1, 5))
    data.extend(struct.pack("<III4B", 1, 1, 1, 255, 255, 255, 255))

    def quad(left, right, state, mode, ambient, light, rows=(0,) * 12):
        data.extend(struct.pack("<13IfI4f8f12fI", 0, 6, *(state[field] for field in FIELDS),
                                0, mode, *ambient, *light, *rows, 0))
        for x, y in ((left, -1), (left, 1), (right, 1), (left, -1), (right, 1), (right, -1)):
            data.extend(struct.pack("<12f", x, y, .5, .5, .5, 0, 0, 1, 0, 0, 0, 0))

    quad(-1, 1, dict(DEFAULT), 1, (.125, .125, .125, 1), (0,) * 8)
    additive = dict(DEFAULT, depth_compare=3, depth_write=0, blend_enable=1, blend_src=1, blend_dst=1, color_mask=7)
    light = (0, 0, 100, .005, 1, 1, 1, .5)
    rows = (0, 0, -.5, 50, 0, 0, -.5, 50, 0, 0, -1, 100)
    for i in range(4):
        mode = 2 if i < 2 else 5
        quad(-1 + i*.5, -1 + (i+1)*.5, additive, mode, (0, 0, 0, i % 2), light,
             rows if mode == 5 else (0,) * 12)
    return bytes(data)


def verify(image, version=11):
    header = b"P6\n64 64\n255\n"
    if not image.startswith(header) or len(image) != len(header) + 64*64*3:
        raise ValueError("invalid fixture image")
    for x, expected in ((8, 64), (24, 96 if version == 11 else 64),
                        (40, 64), (56, 96 if version == 11 else 64)):
        offset = len(header) + (32 * 64 + x) * 3
        if any(abs(value - expected) > 2 for value in image[offset:offset+3]):
            raise ValueError(f"wrong falloff fixture pixel at {x},32")
    return 4


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--version", type=int, choices=(10, 11), default=11)
    args = parser.parse_args()
    try:
        if args.action == "create":
            with args.path.open("xb") as stream:
                stream.write(fixture(args.version))
        else:
            print(f"light_falloff=PASS pixels={verify(args.path.read_bytes(), args.version)}")
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
