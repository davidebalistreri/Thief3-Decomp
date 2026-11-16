#!/usr/bin/env python3
"""Check metallic PBR response against an independent GGX reference."""
import argparse
import math
from pathlib import Path
import struct
import sys

import scene_pbr_fixture as base
import scene_pbr_gtao_fixture as gtao
import scene_pbr_indirect_fixture as indirect
import scene_pbr_reflection_fixture as reflection

FACTORS = (0, .5, 1, .8)


def converted(old, texture_count, factors, texture_overrides=None):
    data = bytearray(old[:196])
    data[:8] = b"T3PBR05\0"
    data.extend(struct.pack("<I", 0))  # No shadows.
    textures_end = 228+texture_count*16
    data.extend(old[196:textures_end])
    for index, rgba in (texture_overrides or {}).items():
        data[200+32+index*16+12:200+32+index*16+16] = bytes(rgba)
    offset = textures_end
    for factor in factors:
        count = struct.unpack_from("<I", old, offset+16)[0]
        end = offset+20+count*48
        data.extend(old[offset:offset+20])
        data.extend(struct.pack("<4f", 0, 0, 0, factor))
        data.extend(old[offset+20:end])
        offset = end
    if offset != len(old):
        raise ValueError("unexpected base PBR fixture")
    return bytes(data)


def fixture(dielectric=False):
    return converted(base.fixture(), 7, (0,)*4 if dielectric else FACTORS)


def bounce_fixture(dielectric=False):
    return converted(indirect.fixture("facing"), 6, (0, 0 if dielectric else 1))


def reflection_fixture():
    # Green metal receiving a white reflected source, with no direct light.
    return converted(reflection.fixture("smooth"), 7, (1, 0),
                     {0: (20, 220, 20, 255), 1: (255, 255, 255, 255)})


def gtao_fixture(dielectric=False):
    # Keep the occluding block visible even when the metallic receiver is black.
    return converted(gtao.fixture("blocker"), 5, (0 if dielectric else 1, 0), {1: (255, 0, 0, 255)})


def expected(x, y, roughness, ao, normal, metallic):
    p = (x/32-1, 1-y/32, .5)
    n = base._normalize(tuple(c/127.5-1 for c in normal))
    delta = tuple(a-b for a,b in zip((0,0,2.5), p))
    d2 = sum(c*c for c in delta)
    l = base._normalize(delta)
    v = base._normalize(tuple(a-b for a,b in zip((0,0,2), p)))
    h = base._normalize(tuple(a+b for a,b in zip(l,v)))
    dot = lambda a,b: max(0, min(1, sum(x*y for x,y in zip(a,b))))
    nl, nv, nh, vh = dot(n,l), dot(n,v), dot(n,h), dot(v,h)
    rough = max(.04, roughness/255)
    a2 = rough**4
    distribution = a2/max(math.pi*(nh*nh*(a2-1)+1)**2, 1e-8)
    k = (rough+1)**2/8
    geometry = (nv/(nv*(1-k)+k))*(nl/(nl*(1-k)+k))
    radiance = base.INTENSITY/max(d2, .1**2)
    linear = [c/255/12.92 if c/255<=.04045 else ((c/255+.055)/1.055)**2.4 for c in base.ALBEDO]
    hdr = []
    for channel in linear:
        f0 = .04*(1-metallic)+channel*metallic
        f = f0+(1-f0)*(1-vh)**5
        specular = distribution*geometry*f/max(4*nv*nl, 1e-8) if nl and nv else 0
        diffuse = (1-f)*(1-metallic)*channel/math.pi
        hdr.append(channel*(1-metallic)*ao/255 + (diffuse+specular)*radiance*nl)
    return tuple(round(255*(12.92*c if c<=.0031308 else 1.055*c**(1/2.4)-.055))
                 for c in (v/(1+v) for v in hdr))


def verify(image, dielectric=False):
    header = b"P6\n64 64\n255\n"
    if not image.startswith(header) or len(image) != len(header)+64*64*3:
        raise ValueError("invalid metallic fixture image")
    result = {}
    for tile, factor in zip(base.TILES, FACTORS):
        x,y,rough,ao,normal = tile
        factor = 0 if dielectric else factor
        actual = tuple(image[len(header)+(y*64+x)*3:len(header)+(y*64+x)*3+3])
        wanted = expected(x,y,rough,ao,normal,factor)
        if any(abs(a-b)>4 for a,b in zip(actual,wanted)):
            raise ValueError(f"metallic fixture pixel {x},{y} is {actual}, expected {wanted}")
        result[f"{x},{y}"] = {"metallic": factor, "rgb": actual, "expected": wanted}
    return result


def verify_bounce(reference, bounced, dielectric=False):
    if dielectric:
        return indirect.verify(reference, bounced, "facing")
    before, after = indirect._pixels(reference), indirect._pixels(bounced)
    offset = (32*64+35)*3
    a, b = tuple(before[offset:offset+3]), tuple(after[offset:offset+3])
    if any(abs(x-y)>2 for x,y in zip(a,b)):
        raise ValueError(f"metallic receiver gained diffuse bounce: {a} -> {b}")
    return {"metal_receiver_before": a, "metal_receiver_after": b}


def verify_reflection(reference, reflected):
    before, after = indirect._pixels(reference), indirect._pixels(reflected)
    offset = (32*64+24)*3
    a, b = tuple(before[offset:offset+3]), tuple(after[offset:offset+3])
    if b[1] < a[1]+10 or b[1] < max(b[0],b[2])+10:
        raise ValueError(f"tinted metallic reflection missing: {a} -> {b}")
    return {"metal_receiver_before": a, "metal_receiver_after": b}


def verify_gtao(reference, occluded, dielectric=False):
    if dielectric:
        return gtao.verify(reference, occluded, "blocker")
    before, after = gtao._pixels(reference), gtao._pixels(occluded)
    offset = (32*64+35)*3
    a, b = tuple(before[offset:offset+3]), tuple(after[offset:offset+3])
    if any(abs(x-y)>1 for x,y in zip(a,b)):
        raise ValueError(f"GTAO attenuated absent metallic ambient: {a} -> {b}")
    return {"metal_receiver_before": a, "metal_receiver_after": b}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--dielectric", action="store_true")
    parser.add_argument("--case", choices=("brdf", "bounce", "reflection", "gtao"), default="brdf")
    parser.add_argument("--reference", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "create":
            with args.path.open("xb") as stream:
                stream.write(fixture(args.dielectric) if args.case == "brdf" else
                             bounce_fixture(args.dielectric) if args.case == "bounce" else
                             gtao_fixture(args.dielectric) if args.case == "gtao" else reflection_fixture())
        else:
            if args.case != "brdf" and args.reference is None:
                raise ValueError("effect verification requires --reference")
            result = (verify(args.path.read_bytes(), args.dielectric) if args.case == "brdf" else
                      verify_bounce(args.reference.read_bytes(), args.path.read_bytes(), args.dielectric) if args.case == "bounce" else
                      verify_gtao(args.reference.read_bytes(), args.path.read_bytes(), args.dielectric) if args.case == "gtao" else
                      verify_reflection(args.reference.read_bytes(), args.path.read_bytes()))
            print("pbr_metallic=PASS", result)
    except (ValueError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
