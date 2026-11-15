#!/usr/bin/env python3
"""Create and independently verify a synthetic PBR/HDR DX12 image."""
import argparse
import math
from pathlib import Path
import struct
import sys

IDENTITY = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
ALBEDO = (128, 64, 32)
FLAT = (128, 128, 255)
SIDE = (255, 128, 128)
AMBIENT = (1.0, 1.0, 1.0)
INTENSITY = 120.0
TILES = ((16, 16, 64, 255, FLAT), (48, 16, 230, 255, FLAT),
         (16, 48, 64, 64, FLAT), (48, 48, 230, 255, SIDE))


def fixture(extreme=False, unit_scale=1.0):
    textures = [(*ALBEDO, 255), (*FLAT, 255), (*SIDE, 255), (64, 64, 64, 255),
                (230, 230, 230, 255), (255, 255, 255, 255), (64, 64, 64, 255)]
    data = bytearray(struct.pack("<8sIIff32f4f4fIII", b"T3PBR01\0", 64, 64, 0, 1, *IDENTITY, *IDENTITY,
                                 0, 0, 2, unit_scale, *AMBIENT, 1, len(textures), 4, 1))
    light = (-.5, .5, .501, .01, 1, 1, 1, 10000) if extreme else (0, 0, 2.5, .1, 1, 1, 1, INTENSITY)
    data.extend(struct.pack("<8f", *light))
    for rgba in textures:
        data.extend(struct.pack("<III4B", 1, 1, 1, *rgba))
    for bounds, normal, rough, ao in (((-1, 0, 0, 1), 1, 3, 5), ((0, 0, 1, 1), 1, 4, 5),
                                      ((-1, -1, 0, 0), 1, 3, 6), ((0, -1, 1, 0), 2, 4, 5)):
        left, bottom, right, top = bounds
        data.extend(struct.pack("<5I", 0, normal, rough, ao, 6))
        for x, y in ((left,bottom), (left,top), (right,top), (left,bottom), (right,top), (right,bottom)):
            data.extend(struct.pack("<12f", x, y, .5, .5, .5, 0, 0, 1, 1, 0, 0, 1))
    return bytes(data)


def fixture_srgb_filter():
    data = bytearray(struct.pack("<8sIIff32f4f4fIII", b"T3PBR01\0", 64, 64, 0, 1, *IDENTITY, *IDENTITY,
                                 0, 0, 2, 1, 1, 1, 1, 1, 4, 1, 1))
    data.extend(struct.pack("<8f", 0, 0, 2, .1, 1, 1, 1, 0))
    data.extend(struct.pack("<III", 2, 2, 1))
    data.extend(bytes((0, 0, 0, 255, 255, 255, 255, 255,
                       255, 255, 255, 255, 0, 0, 0, 255)))
    for rgba in ((128, 128, 255, 255), (204, 204, 204, 255), (255, 255, 255, 255)):
        data.extend(struct.pack("<III4B", 1, 1, 1, *rgba))
    data.extend(struct.pack("<5I", 0, 1, 2, 3, 6))
    for x, y in ((-1,-1), (-1,1), (1,1), (-1,-1), (1,1), (1,-1)):
        data.extend(struct.pack("<12f", x, y, .5, .5, .5, 0, 0, 1, 0, 0, 0, 0))
    return bytes(data)


def _normalize(v):
    length = math.sqrt(sum(x*x for x in v))
    return tuple(x/length for x in v)


def expected(x, y, roughness, ao, normal, unit_scale=1.0):
    # The half-pixel adjustment makes pixel (x,y) correspond to this world point.
    p = (x/32 - 1, 1-y/32, .5)
    n = _normalize(tuple(x/127.5-1 for x in normal))
    light = tuple(a-b for a, b in zip((0,0,2.5), p))
    view = tuple(a-b for a, b in zip((0,0,2), p))
    dist2 = sum(v*v for v in light)
    l, v = _normalize(light), _normalize(view)
    h = _normalize(tuple(a+b for a,b in zip(l,v)))
    dot = lambda a,b: max(0, min(1, sum(x*y for x,y in zip(a,b))))
    nl, nv, nh, vh = dot(n,l), dot(n,v), dot(n,h), dot(v,h)
    r = max(.04, roughness/255)
    a2 = r**4
    distribution = a2/max(math.pi*(nh*nh*(a2-1)+1)**2, 1e-8)
    k = (r+1)**2/8
    geometry = (nv/(nv*(1-k)+k))*(nl/(nl*(1-k)+k))
    fresnel = .04+.96*(1-vh)**5
    specular = distribution*geometry*fresnel/max(4*nv*nl, 1e-8) if nl and nv else 0
    radiance = INTENSITY/max(dist2*unit_scale**2, .1**2)
    linear_albedo = [c/255/12.92 if c/255<=.04045 else ((c/255+.055)/1.055)**2.4 for c in ALBEDO]
    hdr = [linear_albedo[i]*AMBIENT[i]*ao/255 +
           ((1-fresnel)*linear_albedo[i]/math.pi+specular)*radiance*nl for i in range(3)]
    mapped = [v/(1+v) for v in hdr]
    rgb = tuple(round(255*(12.92*c if c<=.0031308 else 1.055*c**(1/2.4)-.055)) for c in mapped)
    return rgb, hdr


def verify(image, unit_scale=1.0):
    header = b"P6\n64 64\n255\n"
    if not image.startswith(header) or len(image) != len(header)+64*64*3:
        raise ValueError("invalid PBR fixture image")
    actual = {}
    for x, y, rough, ao, normal in TILES:
        offset = len(header)+(y*64+x)*3
        value = tuple(image[offset:offset+3])
        expected_rgb, hdr = expected(x,y,rough,ao,normal,unit_scale)
        if any(abs(a-b)>4 for a,b in zip(value,expected_rgb)):
            raise ValueError(f"wrong PBR pixel at {x},{y}: {value}, expected {expected_rgb}")
        actual[f"{x},{y}"] = {"rgb": value, "expected": expected_rgb, "reference_hdr": hdr}
    if expected(*TILES[0],unit_scale)[1][0] <= 1 or actual["16,16"]["rgb"][0] <= 188:
        raise ValueError("fixture did not retain HDR energy before tonemapping")
    if actual["16,16"]["rgb"] == actual["48,16"]["rgb"]:
        raise ValueError("roughness did not affect the specular response")
    if unit_scale == 1 and actual["16,16"]["rgb"] == actual["16,48"]["rgb"]:
        raise ValueError("AO did not affect the ambient contribution")
    return actual


def expected_filmic(x, y, roughness, ao, normal, unit_scale=1.0):
    hdr = expected(x, y, roughness, ao, normal, unit_scale)[1]
    values = []
    for c in hdr:
        c = max(0, min(1, c*(2.51*c+.03)/(c*(2.43*c+.59)+.14)))
        values.append(round(255*(12.92*c if c<=.0031308 else 1.055*c**(1/2.4)-.055)))
    return tuple(values)


def verify_filmic(image, unit_scale=1.0):
    header = b"P6\n64 64\n255\n"
    if not image.startswith(header) or len(image) != len(header)+64*64*3:
        raise ValueError("invalid filmic fixture image")
    actual = {}
    for x, y, roughness, ao, normal in TILES:
        offset = len(header)+(y*64+x)*3
        value = tuple(image[offset:offset+3])
        wanted = expected_filmic(x, y, roughness, ao, normal, unit_scale)
        if any(abs(a-b)>4 for a,b in zip(value,wanted)):
            raise ValueError(f"wrong filmic pixel at {x},{y}: {value}, expected {wanted}")
        actual[f"{x},{y}"] = {"rgb": value, "expected": wanted}
    return actual


def verify_extreme(image):
    header = b"P6\n64 64\n255\n"
    if not image.startswith(header) or len(image) != len(header)+64*64*3:
        raise ValueError("invalid PBR fixture image")
    offset = len(header)+(16*64+16)*3
    pixel = tuple(image[offset:offset+3])
    if any(channel < 250 for channel in pixel):
        raise ValueError(f"extreme HDR energy did not produce a finite highlight: {pixel}")
    return pixel


def verify_srgb_filter(image):
    header = b"P6\n64 64\n255\n"
    if not image.startswith(header) or len(image) != len(header)+64*64*3:
        raise ValueError("invalid PBR fixture image")
    offset = len(header)+(32*64+32)*3
    pixel = tuple(image[offset:offset+3])
    # Half black and half white must filter to 0.5 linear, then tone-map to 1/3.
    if any(abs(channel-156) > 2 for channel in pixel):
        raise ValueError(f"sRGB texture was not filtered in linear light: {pixel}")
    return pixel


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("path", type=Path)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--extreme", action="store_true")
    group.add_argument("--half-unit", action="store_true")
    group.add_argument("--srgb-filter", action="store_true")
    parser.add_argument("--filmic", action="store_true", help="verify the filmic display curve")
    args=parser.parse_args()
    try:
        if args.filmic and (args.extreme or args.srgb_filter):
            raise ValueError("filmic verification is available for the standard PBR fixture")
        if args.action=="create":
            contents = fixture_srgb_filter() if args.srgb_filter else fixture(args.extreme, .5 if args.half_unit else 1)
            with args.path.open("xb") as stream: stream.write(contents)
        else:
            pixels = (verify_extreme(args.path.read_bytes()) if args.extreme else
                      verify_srgb_filter(args.path.read_bytes()) if args.srgb_filter else
                      verify_filmic(args.path.read_bytes(), .5 if args.half_unit else 1) if args.filmic else
                      verify(args.path.read_bytes(), .5 if args.half_unit else 1))
            print(f"pbr_hdr_srgb_roughness_ao_normal=PASS pixels={pixels}")
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__=="__main__":
    raise SystemExit(main())
