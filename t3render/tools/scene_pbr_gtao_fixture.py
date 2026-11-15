#!/usr/bin/env python3
"""Create and verify horizon-based ambient-occlusion GPU fixtures."""
import argparse
from pathlib import Path
import struct
import sys

from scene_pbr_indirect_fixture import IDENTITY

KINDS = ("flat", "tilted", "blocker", "direct")


def fixture(kind):
    if kind not in KINDS:
        raise ValueError("invalid ambient-occlusion fixture kind")
    blocked = kind in ("blocker", "direct")
    ambient = 0 if kind == "direct" else .3
    data = bytearray(struct.pack("<8sIIff32f4f4fIII", b"T3PBR01\0", 64, 64, 0, 1, *IDENTITY, *IDENTITY,
                                 0, 0, -2, 1, ambient, ambient, ambient, 1, 5, 2 if blocked else 1, 1))
    data.extend(struct.pack("<8f", 0, 0, -1, .1, 1, 1, 1, 3 if kind == "direct" else 0))
    for rgba in ((255, 255, 255, 255), (0, 0, 0, 255), (128, 128, 255, 255),
                 (230, 230, 230, 255), (255, 255, 255, 255)):
        data.extend(struct.pack("<III4B", 1, 1, 1, *rgba))
    normal = (.6, 0, -.8) if kind == "tilted" else (0, 0, -1)
    geometry = [((-1, -1, 1, 1), .5, 0, normal)]
    if blocked:
        geometry.append(((-.08, -1, .08, 1), .2, 1, (0, 0, -1)))
    for (left, bottom, right, top), z, texture, normal in geometry:
        data.extend(struct.pack("<5I", texture, 2, 3, 4, 6))
        for x, y in ((left, bottom), (left, top), (right, top),
                     (left, bottom), (right, top), (right, bottom)):
            height = z + .75*x if kind == "tilted" else z
            data.extend(struct.pack("<12f", x, y, height, .5, .5, *normal, 0, 0, 0, 0))
    return bytes(data)


def _pixels(image):
    header = b"P6\n64 64\n255\n"
    if not image.startswith(header) or len(image) != len(header)+64*64*3:
        raise ValueError("invalid ambient-occlusion fixture image")
    return image[len(header):]


def verify(reference, occluded, kind):
    if kind not in KINDS:
        raise ValueError("invalid ambient-occlusion fixture kind")
    base, result = _pixels(reference), _pixels(occluded)
    def pixel(image, x, y):
        offset = (y*64+x)*3
        return tuple(image[offset:offset+3])
    before, after = pixel(base, 35, 32), pixel(result, 35, 32)
    if kind == "blocker":
        if after[0] > before[0]-3:
            raise ValueError(f"contact occlusion missing: {before} -> {after}")
        far, far_before = pixel(result, 50, 32), pixel(base, 50, 32)
        if any(abs(a-b)>2 for a, b in zip(far, far_before)):
            raise ValueError("ambient occlusion exceeded the screen-space radius")
    elif any(abs(a-b)>1 for a, b in zip(base, result)):
        raise ValueError(f"{kind} unexpectedly changed without ambient occlusion")
    return {"receiver_before": before, "receiver_after": after}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--kind", choices=KINDS, default="blocker")
    parser.add_argument("--reference", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "create":
            with args.path.open("xb") as stream:
                stream.write(fixture(args.kind))
        else:
            if args.reference is None:
                raise ValueError("--reference is required for verification")
            print("horizon_ambient_occlusion=PASS", verify(args.reference.read_bytes(), args.path.read_bytes(), args.kind))
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
