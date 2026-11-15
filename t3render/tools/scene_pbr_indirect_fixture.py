#!/usr/bin/env python3
"""Create and verify synthetic one-bounce diffuse screen-space fixtures."""
import argparse
import math
from pathlib import Path
import struct
import sys

IDENTITY = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
KINDS = ("facing", "flat", "blocker", "unlit")


def fixture(kind):
    if kind not in KINDS:
        raise ValueError("invalid indirect fixture kind")
    draws = 3 if kind == "blocker" else 2
    ambient = .1 if kind == "unlit" else 0
    data = bytearray(struct.pack("<8sIIff32f4f4fIII", b"T3PBR01\0", 64, 64, 0, 1, *IDENTITY, *IDENTITY,
                                 0, 0, -2, 1, ambient, ambient, ambient, 1, 6, draws, 1))
    data.extend(struct.pack("<8f", 1, 0, -.5, .1, 1, 1, 1, 0 if kind == "unlit" else 8))
    for rgba in ((255, 40, 20, 255), (255, 255, 255, 255), (0, 0, 0, 255),
                 (128, 128, 255, 255), (230, 230, 230, 255), (255, 255, 255, 255)):
        data.extend(struct.pack("<III4B", 1, 1, 1, *rgba))
    slope = math.sqrt(.8), -math.sqrt(.2)
    source = (0, 0, -1) if kind == "flat" else (slope[0], 0, slope[1])
    geometry = [((-1, -1, 0, 1), .5, 0, source), ((0, -1, 1, 1), .5, 1, (-slope[0], 0, slope[1]))]
    if kind == "blocker":
        geometry.append(((-.08, -1, .08, 1), .2, 2, (0, 0, -1)))
    for (left, bottom, right, top), z, texture, normal in geometry:
        data.extend(struct.pack("<5I", texture, 3, 4, 5, 6))
        for x, y in ((left, bottom), (left, top), (right, top),
                     (left, bottom), (right, top), (right, bottom)):
            data.extend(struct.pack("<12f", x, y, z, .5, .5, *normal, 0, 0, 0, 0))
    return bytes(data)


def _pixels(image):
    header = b"P6\n64 64\n255\n"
    if not image.startswith(header) or len(image) != len(header)+64*64*3:
        raise ValueError("invalid indirect fixture image")
    return image[len(header):]


def verify(reference, indirect, kind):
    if kind not in KINDS:
        raise ValueError("invalid indirect fixture kind")
    base, result = _pixels(reference), _pixels(indirect)
    def pixel(image, x, y):
        offset = (y*64+x)*3
        return tuple(image[offset:offset+3])
    before, after = pixel(base, 35, 32), pixel(result, 35, 32)
    if kind == "facing":
        if after[0] < before[0]+5 or after[0] < after[1]+5:
            raise ValueError(f"colored diffuse bounce missing: {before} -> {after}")
        if any(abs(a-b)>2 for a, b in zip(pixel(base, 50, 32), pixel(result, 50, 32))):
            raise ValueError("bounce exceeded the screen-space sampling range")
    elif kind in ("flat", "blocker"):
        if max(abs(a-b) for a, b in zip(before, after)) > 2:
            raise ValueError(f"unexpected diffuse bounce for {kind}: {before} -> {after}")
        if kind == "flat" and pixel(base, 16, 32)[0] < 30:
            raise ValueError("flat source was not directly illuminated")
    elif any(abs(a-b)>1 for a, b in zip(base, result)):
        raise ValueError("ambient-only lighting must not create a diffuse bounce")
    return {"receiver_before": before, "receiver_after": after}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--kind", choices=KINDS, default="facing")
    parser.add_argument("--reference", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "create":
            with args.path.open("xb") as stream:
                stream.write(fixture(args.kind))
        else:
            if args.reference is None:
                raise ValueError("--reference is required for verification")
            print("screen_space_diffuse_bounce=PASS", verify(args.reference.read_bytes(), args.path.read_bytes(), args.kind))
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
