#!/usr/bin/env python3
"""Exercise six point-shadow faces, soft edges and isolated light layers."""
import argparse
import math
from pathlib import Path
import struct
import sys

AXES = (
    ((0, 0, -1), (0, 1, 0), (1, 0, 0)), ((0, 0, 1), (0, 1, 0), (-1, 0, 0)),
    ((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((1, 0, 0), (0, 0, 1), (0, -1, 0)),
    ((1, 0, 0), (0, 1, 0), (0, 0, 1)), ((-1, 0, 0), (0, 1, 0), (0, 0, -1)),
    ((math.sqrt(.5), 0, -math.sqrt(.5)), (0, 1, 0), (math.sqrt(.5), 0, math.sqrt(.5))),
)
IDENTITY = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)


def fixture(face, shadow=True, two_lights=False, asymmetric=False, hard=False, short_range=False, behind=False,
            offline_caster=None):
    if type(face) is not int or not 0 <= face < len(AXES):
        raise ValueError("face must be in 0..6 (6 is a cubemap seam)")
    if offline_caster is not None and (not shadow or type(offline_caster) is not bool):
        raise ValueError("offline caster fixture requires shadow maps")
    right, up, forward = AXES[face]
    def world(x, y, z):
        return tuple(x*right[i]+y*up[i]+z*forward[i] for i in range(3))
    view = tuple(v for row in range(3) for v in (right[row], up[row], forward[row], 0)) + (0, 0, 0, 1)
    count = 2 if two_lights else 1
    magic = b"T3PBR03\0" if offline_caster is not None else b"T3PBR02\0" if shadow else b"T3PBR01\0"
    body = bytearray(struct.pack("<8sIIff32f4f4fIII", magic,
                                 64, 64, 0, 1, *view, *IDENTITY, *world(0, 0, -2), 1,
                                 .02, .02, .02, 1, 4, 1 if offline_caster is not None else 2, count))
    if shadow:
        body.extend(struct.pack("<I4f", 128, .05, 1 if short_range else 10, .005, 0 if hard else .5))
        if offline_caster is not None:
            body.extend(struct.pack("<I", int(offline_caster)))
    colors = ((1, 0, 0), (0, 0, 1)) if two_lights else ((1, 1, 1),)
    for color in colors:
        body.extend(struct.pack("<8f", *world(0, 0, -.7), .2, *color, 3))
    for rgba in ((180, 180, 180, 255), (128, 128, 255, 255), (204, 204, 204, 255), (255, 255, 255, 255)):
        body.extend(struct.pack("<III4B", 1, 1, 1, *rgba))
    blocker = (.1, .06, .3, .18) if asymmetric else (-.2, -.2, .2, .2)
    geometry = [((-1, -1, 1, 1), .7, (1 << count)-1)]
    if offline_caster is None:
        geometry.append((blocker, 1.2 if behind else -.1, 1))
    for (left, bottom, right_edge, top), z, mask in geometry:
        body.extend(struct.pack("<5I", 0, 1, 2, 3, 6))
        if shadow:
            body.extend(struct.pack("<I", mask))
        for x, y in ((left, bottom), (left, top), (right_edge, top),
                     (left, bottom), (right_edge, top), (right_edge, bottom)):
            body.extend(struct.pack("<12f", *world(x, y, z), .5, .5, *world(0, 0, -1), 0, 0, 0, 0))
    if offline_caster:
        left, bottom, right_edge, top = blocker
        body.extend(struct.pack("<II", 1, 6))
        for x, y in ((left, bottom), (left, top), (right_edge, top),
                     (left, bottom), (right_edge, top), (right_edge, bottom)):
            body.extend(struct.pack("<3f", *world(x, y, 1.2 if behind else -.1)))
    return bytes(body)


def _pixels(image):
    header = b"P6\n64 64\n255\n"
    if not image.startswith(header) or len(image) != len(header)+64*64*3:
        raise ValueError("invalid shadow fixture image")
    return image[len(header):]


def verify(reference, shadowed, two_lights=False):
    base, test = _pixels(reference), _pixels(shadowed)
    def pixel(data, x, y):
        offset = (y*64+x)*3
        return tuple(data[offset:offset+3])
    center, original = pixel(test, 32, 32), pixel(base, 32, 32)
    outside, outside_base = pixel(test, 6, 32), pixel(base, 6, 32)
    if any(abs(a-b)>3 for a,b in zip(outside, outside_base)):
        raise ValueError(f"unoccluded point changed: {outside}, expected {outside_base}")
    if two_lights:
        if center[0] >= original[0]*.5 or abs(center[2]-original[2]) > 3 or center[2] < center[0]*2:
            raise ValueError(f"light-specific caster mask failed: {center}, reference {original}")
    elif center[0] >= original[0]*.5:
        raise ValueError(f"occluded point remained lit: {center}, reference {original}")
    ratios = []
    for x in range(8, 56):
        a, b = pixel(base, x, 32)[0], pixel(test, x, 32)[0]
        if a > 10:
            ratios.append(b/a)
    if not any(.55 < ratio < .9 for ratio in ratios):
        raise ValueError("shadow penumbra has no intermediate visibility")
    return {"center": center, "unshadowed_center": original, "outside": outside,
            "intermediate_samples": sum(.55 < ratio < .9 for ratio in ratios)}


def verify_asymmetric(reference, shadowed):
    base, test = _pixels(reference), _pixels(shadowed)
    def pixel(data, x, y):
        offset = (y*64+x)*3
        return tuple(data[offset:offset+3])
    center, original = pixel(test, 47, 23), pixel(base, 47, 23)
    mirror, mirror_base = pixel(test, 17, 41), pixel(base, 17, 41)
    if center[0] >= original[0]*.6:
        raise ValueError(f"offset occluder is not shadowed: {center}, reference {original}")
    if any(abs(a-b)>3 for a,b in zip(mirror, mirror_base)):
        raise ValueError(f"offset shadow landed on the wrong side: {mirror}, reference {mirror_base}")
    return {"center": center, "unshadowed_center": original, "mirror": mirror}


def verify_hard(reference, hard, soft):
    base, hard_pixels, soft_pixels = map(_pixels, (reference, hard, soft))
    def middle(image):
        return sum(.55 < image[(32*64+x)*3]/base[(32*64+x)*3] < .9
                   for x in range(8, 56) if base[(32*64+x)*3] > 10)
    center = (32*64+32)*3
    if hard_pixels[center] >= base[center]*.5 or middle(hard_pixels) != 0 or middle(soft_pixels) == 0:
        raise ValueError("hard/soft shadow controls did not produce distinct penumbrae")
    return {"hard_intermediate": middle(hard_pixels), "soft_intermediate": middle(soft_pixels)}


def verify_short_range(reference, shadowed, reason="geometry beyond shadow range"):
    base, test = _pixels(reference), _pixels(shadowed)
    if any(abs(a-b)>2 for a,b in zip(base,test)):
        raise ValueError(f"{reason} should remain unshadowed")
    return {"max_channel_difference": max(abs(a-b) for a,b in zip(base,test))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--face", type=int, default=4)
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--two-lights", action="store_true")
    parser.add_argument("--asymmetric", action="store_true")
    parser.add_argument("--hard", action="store_true")
    parser.add_argument("--short-range", action="store_true")
    parser.add_argument("--behind", action="store_true")
    parser.add_argument("--offline-caster", action="store_true",
                        help="use packet v3 with a shadow-only blocker; --baseline omits it")
    parser.add_argument("--soft-reference", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "create":
            with args.path.open("xb") as stream:
                stream.write(fixture(args.face, not args.baseline or args.offline_caster, args.two_lights, args.asymmetric,
                                     args.hard, args.short_range, args.behind,
                                     offline_caster=not args.baseline if args.offline_caster else None))
        else:
            if args.reference is None:
                raise ValueError("--reference is required for verification")
            if args.hard:
                if args.soft_reference is None:
                    raise ValueError("--soft-reference is required for hard shadow verification")
                result = verify_hard(args.reference.read_bytes(), args.path.read_bytes(), args.soft_reference.read_bytes())
            elif args.short_range or args.behind:
                reason = "blocker behind receiver" if args.behind else "geometry beyond shadow range"
                result = verify_short_range(args.reference.read_bytes(), args.path.read_bytes(), reason)
            elif args.asymmetric:
                result = verify_asymmetric(args.reference.read_bytes(), args.path.read_bytes())
            else:
                result = verify(args.reference.read_bytes(), args.path.read_bytes(), args.two_lights)
            print(f"point_shadow_faces_softness_layers=PASS {result}")
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
