#!/usr/bin/env python3
"""Exercise API-neutral blend, alpha, depth and cull state on the SceneIR DX12 path."""
import argparse
from pathlib import Path
import struct
import sys

from scene_legacy_states import DEFAULT, FIELDS, validate
from scene_texture_fixture import IDENTITY

EXPECTED = {(16, 16): (160, 48, 64), (48, 16): (96, 160, 128),
            (16, 48): (32, 96, 193), (48, 48): (0, 255, 0),
            (32, 8): (255, 0, 0), (4, 4): (255, 48, 64),
            (8, 30): (255, 0, 255), (56, 30): (96, 160, 128),
            (8, 40): (255, 0, 255), (56, 56): (255, 0, 255), (48, 4): (0, 255, 0)}


def fixture():
    textures = [(64, 96, 128, 255), (255, 0, 0, 128), (32, 64, 0, 255),
                (64, 128, 192, 255), (255, 255, 255, 0), (0, 255, 0, 255), (255, 0, 255, 255)]
    draws = []
    def quad(bounds, z, texture, state=None, reverse=False):
        left, bottom, right, top = bounds
        bl, tl, tr, br = ((left, bottom), (left, top), (right, top), (right, bottom))
        vertices = (bl, tl, tr, bl, tr, br) if not reverse else (bl, tr, tl, bl, br, tr)
        style = dict(DEFAULT, **(state or {}))
        validate(style)
        draws.append((texture, z, vertices, style))
    quad((-1, -1, 1, 1), .5, 0)
    same_depth = {"depth_compare": 3, "depth_write": 0, "color_mask": 7}
    quad((-1, 0, 0, 1), .5, 1, dict(same_depth, blend_enable=1, blend_src=4, blend_dst=5))
    quad((0, 0, 1, 1), .5, 2, dict(same_depth, blend_enable=1, blend_src=1, blend_dst=1))
    quad((-1, -1, 0, 0), .5, 3, dict(same_depth, blend_enable=1, blend_src=6, blend_dst=2))
    quad((0, -1, 1, 0), .25, 4, {"alpha_compare": 5, "alpha_ref": 127})
    quad((0, -1, 1, 0), .4, 5)  # succeeds only when the transparent quad discarded without writing depth
    quad((-.25, .5, .25, 1), .25, 1, {"alpha_compare": 5, "alpha_ref": 127})
    quad((.5, .5, 1, 1), .25, 5, same_depth)  # unequal depth must reject this draw
    quad((-1, .75, -.75, 1), .5, 1, dict(same_depth, color_mask=1))
    quad((-.875, -.125, -.625, .125), .25, 6, {"cull": 0}, reverse=True)
    quad((.625, -.125, .875, .125), .25, 6, {"cull": 1}, reverse=True)
    quad((-.875, -.375, -.625, -.125), .25, 6, {"cull": 2}, reverse=True)
    quad((.625, -.875, .875, -.625), .2, 1, {"depth_write": 0})
    quad((.625, -.875, .875, -.625), .3, 6)  # must pass because the prior draw did not write depth
    quad((.4, .75, .6, 1), .9, 5, {"depth_enable": 0})
    data = bytearray(struct.pack("<8sIIff32fII", b"T3SCN02\0", 64, 64, 0, 1, *IDENTITY, *IDENTITY,
                                 len(textures), len(draws)))
    for rgba in textures:
        data.extend(struct.pack("<III4B", 1, 1, 1, *rgba))
    for texture, z, vertices, state in draws:
        data.extend(struct.pack("<13I", texture, 6, *(state[field] for field in FIELDS)))
        for x, y in vertices:
            data.extend(struct.pack("<5f", x, y, z, .5, .5))
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
            print(f"blend_alpha_depth_cull=PASS pixels={verify(args.path.read_bytes())}")
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
