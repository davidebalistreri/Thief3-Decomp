#!/usr/bin/env python3
"""Exercise spot cone direction, falloff and independence from an omni light."""
import argparse
import math
from pathlib import Path
import struct
import sys

import scene_pbr_shadow_fixture as base


def fixture(kind, two_lights=False, shadow_caster=None):
    if kind not in ("point", "spot", "away"):
        raise ValueError("unsupported spot fixture")
    if shadow_caster is not None and type(shadow_caster) is not bool:
        raise ValueError("shadow caster must be a boolean or absent")
    original = base.fixture(4, shadow=shadow_caster is not None, two_lights=two_lights,
                            offline_caster=shadow_caster)
    if kind == "point":
        return original
    count = 2 if two_lights else 1
    header = bytearray(original[:196]); header[:8] = b"T3PBR06\0"
    result = header + struct.pack("<I", int(shadow_caster is not None))
    light_offset = 196
    if shadow_caster is not None:
        result.extend(original[196:220])
        light_offset = 220
    for i in range(count):
        result.extend(original[light_offset+i*32:light_offset+(i+1)*32])
        if i == 0:
            direction = (0, 0, 1 if kind == "spot" else -1)
            result.extend(struct.pack("<5f", *direction, math.cos(math.radians(12)), math.cos(math.radians(25))))
        else:
            result.extend(struct.pack("<5f", 0, 0, 0, 0, 0))
    result.extend(original[light_offset+count*32:])
    return bytes(result)


def verify(point, spot, away, point_mixed, spot_mixed):
    point, spot, away, point_mixed, spot_mixed = map(base._pixels, (point, spot, away, point_mixed, spot_mixed))
    def pixel(image, x):
        offset = (32*64+x)*3
        return tuple(image[offset:offset+3])
    center, middle, outside = (pixel(spot, x) for x in (32, 46, 58))
    ref_center, ref_middle, ref_outside = (pixel(point, x) for x in (32, 46, 58))
    if min(ref_center[0], ref_middle[0], ref_outside[0]) < 10:
        raise ValueError("point-light reference is too dark")
    if any(abs(a-b) > 3 for a, b in zip(center, ref_center)):
        raise ValueError("spot axis does not retain point-light energy")
    if not .35 < middle[0]/ref_middle[0] < .9:
        raise ValueError("spot cone lacks a smooth transition")
    if outside[0] >= ref_outside[0]*.5 or pixel(away, 32)[0] >= ref_center[0]*.5:
        raise ValueError("light escaped outside the spot cone or behind its direction")
    mixed, reference = pixel(spot_mixed, 58), pixel(point_mixed, 58)
    if mixed[0] >= reference[0]*.5 or abs(mixed[2]-reference[2]) > 3:
        raise ValueError("spot cone changed the independent omni light")
    return {"axis": center, "transition": middle, "outside": outside, "mixed_outside": mixed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--kind", choices=("point", "spot", "away"), default="spot")
    parser.add_argument("--two-lights", action="store_true")
    parser.add_argument("--shadow-caster", choices=("on", "off"),
                        help="use shadow maps with or without a shadow-only blocker")
    parser.add_argument("--point", type=Path)
    parser.add_argument("--away", type=Path)
    parser.add_argument("--point-mixed", type=Path)
    parser.add_argument("--spot-mixed", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "create":
            content = fixture(args.kind, args.two_lights,
                              None if args.shadow_caster is None else args.shadow_caster == "on")
            with args.path.open("xb") as stream:
                stream.write(content)
        else:
            if any(path is None for path in (args.point, args.away, args.point_mixed, args.spot_mixed)):
                raise ValueError("verification requires point, away and both mixed-light images")
            result = verify(args.point.read_bytes(), args.path.read_bytes(), args.away.read_bytes(),
                            args.point_mixed.read_bytes(), args.spot_mixed.read_bytes())
            print(f"pbr_spot_direction_cone_independent_omni=PASS {result}")
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
