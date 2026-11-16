#!/usr/bin/env python3
"""Synthetic D3D12 control for resident semantic variant switching."""
import argparse
from pathlib import Path
import struct
import sys

import scene_pbr_fixture as base

KEY = "asset:fixture"


def packets():
    original = base.fixture()
    replacement = bytearray(original)
    replacement[196+32+12:196+32+16] = bytes((16, 128, 240, 255))
    return original, bytes(replacement)


def bundle():
    original, replacement = packets()
    return _bundle(original, replacement)


def _bundle(original, replacement):
    key = KEY.encode("ascii")
    return struct.pack("<8sIII", b"T3VAR01\0", len(key), len(original), len(replacement)) + key + original + replacement


def subset_packets():
    # The four-draw base fixture covers one quadrant per draw. Remove the
    # second quadrant without changing any of the other resident resources.
    offset, draw_size = 196 + 32 + 7 * 16, 20 + 6 * 48
    result = []
    for packet in packets():
        if len(packet) != offset + 4 * draw_size or struct.unpack_from("<I", packet, 188)[0] != 4:
            raise ValueError("unexpected base fixture layout")
        raw = bytearray(packet[:offset] + packet[offset:offset + draw_size] + packet[offset + 2 * draw_size:])
        struct.pack_into("<I", raw, 188, 3)
        result.append(bytes(raw))
    return tuple(result)


def subset_bundle():
    return _bundle(*subset_packets())


def verify(original, replacement, restored, reference_original=None, reference_replacement=None):
    header = b"P6\n64 64\n255\n"
    if any(not image.startswith(header) or len(image) != len(header)+64*64*3
           for image in (original, replacement, restored)):
        raise ValueError("invalid variant fixture image")
    if original != restored:
        raise ValueError("restored original differs from initial image")
    if original == replacement:
        raise ValueError("variant switch produced no visible change")
    if reference_original is not None and original != reference_original:
        raise ValueError("resident original differs from standalone render")
    if reference_replacement is not None and replacement != reference_replacement:
        raise ValueError("resident replacement differs from standalone render")
    return {"changed_pixels": sum(original[i:i+3] != replacement[i:i+3]
                                  for i in range(len(header), len(original), 3))}


def verify_subset(base_original, base_replacement, subset_original, subset_replacement,
                  reference_original, reference_replacement):
    header = b"P6\n64 64\n255\n"
    images = (base_original, base_replacement, subset_original, subset_replacement,
              reference_original, reference_replacement)
    if any(not image.startswith(header) or len(image) != len(header) + 64 * 64 * 3 for image in images):
        raise ValueError("invalid subset fixture image")
    if subset_original != reference_original or subset_replacement != reference_replacement:
        raise ValueError("resident subset differs from standalone render")
    for before, after in ((base_original, subset_original), (base_replacement, subset_replacement)):
        for y in range(64):
            for x in range(64):
                i = len(header) + (y * 64 + x) * 3
                if (before[i:i+3] != after[i:i+3]) != (x >= 32 and y < 32):
                    raise ValueError("subset changed pixels outside the removed quadrant")
    return {"changed_pixels": 1024}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "create-original", "create-replacement", "verify",
                                           "create-subset", "create-subset-original", "create-subset-replacement",
                                           "verify-subset"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--replacement", type=Path)
    parser.add_argument("--restored", type=Path)
    parser.add_argument("--reference-original", type=Path)
    parser.add_argument("--reference-replacement", type=Path)
    parser.add_argument("--subset-original", type=Path)
    parser.add_argument("--subset-replacement", type=Path)
    args = parser.parse_args()
    try:
        if args.action.startswith("create"):
            content = {"create": bundle, "create-original": lambda: packets()[0],
                       "create-replacement": lambda: packets()[1], "create-subset": subset_bundle,
                       "create-subset-original": lambda: subset_packets()[0],
                       "create-subset-replacement": lambda: subset_packets()[1]}[args.action]()
            with args.path.open("xb") as stream:
                stream.write(content)
        elif args.action == "verify-subset":
            if any(path is None for path in (args.replacement, args.subset_original, args.subset_replacement,
                                             args.reference_original, args.reference_replacement)):
                raise ValueError("subset verification requires both variants and standalone references")
            result = verify_subset(*(path.read_bytes() for path in (args.path, args.replacement,
                                   args.subset_original, args.subset_replacement,
                                   args.reference_original, args.reference_replacement)))
            print("resident_subset=PASS", result)
        else:
            if args.replacement is None or args.restored is None:
                raise ValueError("verification requires replacement and restored images")
            result = verify(args.path.read_bytes(), args.replacement.read_bytes(), args.restored.read_bytes(),
                            args.reference_original.read_bytes() if args.reference_original else None,
                            args.reference_replacement.read_bytes() if args.reference_replacement else None)
            print("resident_variant_switch=PASS", result)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
