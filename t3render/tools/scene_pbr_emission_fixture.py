#!/usr/bin/env python3
"""Verify emissive HDR radiance and its diffuse screen-space bounce."""
import argparse
from pathlib import Path
import struct
import sys

import scene_pbr_indirect_fixture as indirect


def fixture(enabled=True):
    if type(enabled) is not bool:
        raise ValueError("invalid emission fixture switch")
    original = indirect.fixture("facing")
    header = bytearray(original[:196])
    header[:8] = b"T3PBR04\0"
    struct.pack_into("<3f", header, 168, .01, .01, .01)
    light = bytearray(original[196:228])
    struct.pack_into("<f", light, 28, 0)
    offset = 228
    textures = bytearray()
    for _ in range(6):
        width, height, count = struct.unpack_from("<III", original, offset)
        if (width, height, count) != (1, 1, 1):
            raise ValueError("unexpected fixture texture")
        textures.extend(original[offset:offset+16])
        offset += 16
    textures[12:16] = bytes((0, 0, 0, 255))
    data = header + struct.pack("<I4fI", 128, .05, 10, .005, .5, 0) + light + textures
    for draw in range(2):
        count = struct.unpack_from("<I", original, offset+16)[0]
        end = offset+20+count*48
        data.extend(original[offset:offset+20])
        data.extend(struct.pack("<I3f", 0, *(8, 3.2, .64) if enabled and draw == 0 else (0, 0, 0)))
        data.extend(original[offset+20:end])
        offset = end
    if offset != len(original):
        raise ValueError("unexpected fixture geometry")
    return bytes(data)


def verify(reference, bounced, off_reference):
    indirect.verify(reference, bounced, "facing")
    base, off = indirect._pixels(reference), indirect._pixels(off_reference)
    source = (32*64+16)*3
    if base[source] < off[source]+50:
        raise ValueError("emissive surface did not add HDR radiance")
    return {"emitter_on": tuple(base[source:source+3]), "emitter_off": tuple(off[source:source+3])}


def verify_off(reference, bounced):
    before, after = indirect._pixels(reference), indirect._pixels(bounced)
    if any(abs(a-b)>1 for a,b in zip(before, after)):
        raise ValueError("disabled emission created an indirect bounce")
    return {"max_channel_difference": max(abs(a-b) for a,b in zip(before,after))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--off", action="store_true")
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--off-reference", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "create":
            with args.path.open("xb") as stream:
                stream.write(fixture(not args.off))
        else:
            if args.reference is None or (not args.off and args.off_reference is None):
                raise ValueError("--reference and, for enabled emission, --off-reference are required")
            result = (verify_off(args.reference.read_bytes(), args.path.read_bytes()) if args.off else
                      verify(args.reference.read_bytes(), args.path.read_bytes(), args.off_reference.read_bytes()))
            print("emission_and_diffuse_bounce=PASS", result)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
