#!/usr/bin/env python3
"""Exercise camera-independent diffuse bounce from an off-screen emitter."""
import argparse
import math
from pathlib import Path
import struct
import sys

IDENTITY = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
NORMAL_X, NORMAL_Z = math.sqrt(.8), -math.sqrt(.2)


def fixture(blocked=False, emitting=True, shadow_only=False):
    if (type(blocked) is not bool or type(emitting) is not bool or type(shadow_only) is not bool or
            (shadow_only and not blocked)):
        raise ValueError("fixture switches must be boolean")
    data = bytearray(struct.pack("<8sIIff32f4f4fIII", b"T3PBR04\0", 64, 64, 0, 1, *IDENTITY, *IDENTITY,
                                 0, 0, -2, 1, .01, .01, .01, 1, 4, 3 if blocked and not shadow_only else 2, 1))
    data.extend(struct.pack("<I4fI", 128, .05, 10, .005, .5, int(shadow_only)))
    data.extend(struct.pack("<8f", 0, 0, -1, .1, 1, 1, 1, 0))
    for rgba in ((255, 255, 255, 255), (128, 128, 255, 255), (230, 230, 230, 255), (255, 255, 255, 255)):
        data.extend(struct.pack("<III4B", 1, 1, 1, *rgba))

    def draw(points, normal, emission, mask):
        data.extend(struct.pack("<5II3f", 0, 1, 2, 3, 6, mask, *emission))
        for index in (0, 1, 2, 0, 2, 3):
            data.extend(struct.pack("<12f", *points[index], .5, .5, *normal, 0, 0, 0, 0))

    draw(((0, -1, .5), (0, 1, .5), (1, 1, .5), (1, -1, .5)),
         (-NORMAL_X, 0, NORMAL_Z), (0, 0, 0), 1)
    draw(((-2, -1, .5), (-2, 1, .5), (-1.2, 1, .5), (-1.2, -1, .5)),
         (NORMAL_X, 0, NORMAL_Z), (8, 0, 0) if emitting else (0, 0, 0), 0)
    if blocked:
        points=((-.5, -2, -1), (-.5, 2, -1), (-.5, 2, 2), (-.5, -2, 2))
        if shadow_only:
            data.extend(struct.pack("<II",1,6))
            for index in (0,1,2,0,2,3): data.extend(struct.pack("<3f",*points[index]))
        else:
            draw(points,(1, 0, 0),(0, 0, 0),1)
    return bytes(data)


def _pixels(image):
    header = b"P6\n64 64\n255\n"
    if not image.startswith(header) or len(image) != len(header)+64*64*3:
        raise ValueError("invalid world GI fixture image")
    return image[len(header):]


def verify(reference, bounce, blocked, unlit, screen=None):
    base, lit, occluded, off = map(_pixels, (reference, bounce, blocked, unlit))
    offset = (32*64+48)*3
    before, after, shadow, disabled = (tuple(image[offset:offset+3]) for image in (base,lit,occluded,off))
    if after[0] < before[0]+20 or after[0] < after[1]+20:
        raise ValueError(f"off-screen emitter did not bounce red light: {before} -> {after}")
    if abs(shadow[0]-before[0]) > 3:
        raise ValueError(f"world-space blocker did not occlude the bounce: {shadow} vs {before}")
    if any(abs(a-b)>3 for a,b in zip(disabled,before)):
        raise ValueError("unlit geometry emitted indirect light")
    if screen is not None:
        pixels=_pixels(screen)
        if any(abs(a-b)>2 for a,b in zip(base,pixels)):
            raise ValueError("screen-space control unexpectedly observed the off-screen emitter")
    return {"baseline": before, "offscreen_bounce": after, "blocked": shadow, "emitter_off": disabled}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--blocked", action="store_true")
    parser.add_argument("--off", action="store_true")
    parser.add_argument("--shadow-only", action="store_true", help="make the blocker an offline shadow-only caster")
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--blocked-reference", type=Path)
    parser.add_argument("--off-reference", type=Path)
    parser.add_argument("--screen-reference", type=Path)
    args=parser.parse_args()
    try:
        if args.action=="create":
            content=fixture(args.blocked,not args.off,args.shadow_only)
            with args.path.open("xb") as output: output.write(content)
        else:
            if any(x is None for x in (args.reference,args.blocked_reference,args.off_reference)):
                raise ValueError("verification requires baseline, blocked and emitter-off images")
            print("world_space_indirect=PASS",verify(args.reference.read_bytes(),args.path.read_bytes(),
                  args.blocked_reference.read_bytes(),args.off_reference.read_bytes(),
                  args.screen_reference.read_bytes() if args.screen_reference else None))
    except (OSError,ValueError) as exc:
        print(f"error: {exc}",file=sys.stderr); return 1
    return 0


if __name__=="__main__":
    raise SystemExit(main())
