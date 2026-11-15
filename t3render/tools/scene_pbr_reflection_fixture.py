#!/usr/bin/env python3
"""Create and verify bounded screen-space reflection fixtures."""
import argparse
from pathlib import Path
import struct
import sys

from scene_pbr_indirect_fixture import IDENTITY

KINDS = ("smooth", "medium", "rough", "away")


def fixture(kind):
    if kind not in KINDS:
        raise ValueError("invalid reflection fixture kind")
    data = bytearray(struct.pack("<8sIIff32f4f4fIII", b"T3PBR01\0", 64, 64, 0, 1, *IDENTITY, *IDENTITY,
                                 0, 0, -2, 1, .6, .6, .6, 1, 7, 2, 1))
    data.extend(struct.pack("<8f", 0, 0, -1, .1, 1, 1, 1, 0))
    for rgba in ((0, 0, 0, 255), (255, 10, 10, 255), (128, 128, 255, 255),
                 (255, 255, 255, 255), (26, 26, 26, 255), (255, 255, 255, 255), (128, 128, 128, 255)):
        data.extend(struct.pack("<III4B", 1, 1, 1, *rgba))
    geometry = [((-1, -1, 0, 1), .5, 0, 3 if kind == "rough" else 6 if kind == "medium" else 4,
                 ((-.6 if kind == "away" else .6), 0, -.8)),
                ((0, -1, 1, 1), .35, 1, 3, (0, 0, -1))]
    for (left, bottom, right, top), z, albedo, roughness, normal in geometry:
        data.extend(struct.pack("<5I", albedo, 2, roughness, 5, 6))
        for x, y in ((left, bottom), (left, top), (right, top),
                     (left, bottom), (right, top), (right, bottom)):
            data.extend(struct.pack("<12f", x, y, z, .5, .5, *normal, 0, 0, 0, 0))
    return bytes(data)


def _pixels(image):
    header = b"P6\n64 64\n255\n"
    if not image.startswith(header) or len(image) != len(header)+64*64*3:
        raise ValueError("invalid reflection fixture image")
    return image[len(header):]


def verify(reference, reflected, kind):
    if kind not in KINDS:
        raise ValueError("invalid reflection fixture kind")
    base, result = _pixels(reference), _pixels(reflected)
    def pixel(image, x, y):
        offset = (y*64+x)*3
        return tuple(image[offset:offset+3])
    before, after = pixel(base, 24, 32), pixel(result, 24, 32)
    if kind in ("smooth", "medium"):
        if after[0] < before[0]+3 or after[0] < after[1]+3:
            raise ValueError(f"colored reflection missing: {before} -> {after}")
        if kind == "medium" and after[0] >= 30:
            raise ValueError(f"roughness did not reduce the reflection: {after}")
    elif any(abs(a-b)>1 for a,b in zip(base,result)):
        raise ValueError(f"unexpected reflection for {kind}")
    return {"receiver_before": before, "receiver_after": after}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--kind", choices=KINDS, default="smooth")
    parser.add_argument("--reference", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "create":
            with args.path.open("xb") as stream:
                stream.write(fixture(args.kind))
        else:
            if args.reference is None:
                raise ValueError("--reference is required for verification")
            print("screen_space_reflection=PASS", verify(args.reference.read_bytes(), args.path.read_bytes(), args.kind))
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
