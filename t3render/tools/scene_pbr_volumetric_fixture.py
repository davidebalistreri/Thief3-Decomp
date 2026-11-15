#!/usr/bin/env python3
"""Verify fog extinction, light scattering and shadowed participating media."""
import argparse
from pathlib import Path
import struct
import sys

import scene_pbr_gtao_fixture as gtao

KINDS = ("unlit", "lit", "shadow")


def fixture(kind):
    if kind not in KINDS:
        raise ValueError("invalid volumetric fixture kind")
    original = gtao.fixture("flat")
    header = bytearray(original[:196])
    header[:8] = b"T3PBR03\0"
    struct.pack_into("<3f", header, 168, .02, .02, .02)
    light = bytearray(original[196:228])
    struct.pack_into("<8f", light, 0, .4, 0, -.5, .1, 1, 1, 1, 0 if kind == "unlit" else 8)
    texture_end = 228+5*16
    textures = original[228:texture_end]
    draw = bytearray(original[texture_end:])
    # The receiver faces away from the light: the baseline is ambient only.
    for n in range(6):
        struct.pack_into("<3f", draw, 20+n*48+20, 0, 0, 1)
    caster = b""
    if kind == "shadow":
        caster = struct.pack("<II", 1, 6)
        for y, z in ((-1, -2), (1, -2), (1, .6), (-1, -2), (1, .6), (-1, .6)):
            caster += struct.pack("<3f", .2, y, z)
    return (bytes(header) + struct.pack("<I4fI", 128, .05, 10, .005, .5, int(bool(caster))) +
            bytes(light) + textures + bytes(draw[:20]) + struct.pack("<I", 0) + bytes(draw[20:]) + caster)


def _pixels(image):
    header = b"P6\n64 64\n255\n"
    if not image.startswith(header) or len(image) != len(header)+64*64*3:
        raise ValueError("invalid volumetric fixture image")
    return image[len(header):]


def verify(unlit_base, unlit_fog, lit_base, lit_fog, shadow_fog):
    images = list(map(_pixels, (unlit_base, unlit_fog, lit_base, lit_fog, shadow_fog)))
    offset = (32*64+32)*3
    values = [tuple(image[offset:offset+3]) for image in images]
    base, extinction, same_base, scatter, shadow = values
    if any(abs(a-b)>1 for a,b in zip(base,same_base)):
        raise ValueError("fixture direct lighting changed the ambient-only receiver")
    if extinction[0] >= base[0]-1:
        raise ValueError(f"fog extinction missing: {base} -> {extinction}")
    if scatter[0] <= extinction[0]+3:
        raise ValueError(f"light scattering missing: {extinction} -> {scatter}")
    if shadow[0] >= scatter[0]-3:
        raise ValueError(f"shadow caster did not occlude scattering: {scatter} -> {shadow}")
    return {"base": base, "extinction": extinction, "scattering": scatter, "shadowed_scattering": shadow}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--kind", choices=KINDS, default="lit")
    parser.add_argument("--unlit-base", type=Path)
    parser.add_argument("--unlit-fog", type=Path)
    parser.add_argument("--lit-base", type=Path)
    parser.add_argument("--lit-fog", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "create":
            with args.path.open("xb") as stream:
                stream.write(fixture(args.kind))
        else:
            if any(x is None for x in (args.unlit_base, args.unlit_fog, args.lit_base, args.lit_fog)):
                raise ValueError("four unlit/lit reference images are required")
            result = verify(args.unlit_base.read_bytes(), args.unlit_fog.read_bytes(), args.lit_base.read_bytes(),
                            args.lit_fog.read_bytes(), args.path.read_bytes())
            print("volumetric_extinction_scattering_shadow=PASS", result)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
