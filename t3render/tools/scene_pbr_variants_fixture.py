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
    key = KEY.encode("ascii")
    return struct.pack("<8sIII", b"T3VAR01\0", len(key), len(original), len(replacement)) + key + original + replacement


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "create-original", "create-replacement", "verify"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--replacement", type=Path)
    parser.add_argument("--restored", type=Path)
    parser.add_argument("--reference-original", type=Path)
    parser.add_argument("--reference-replacement", type=Path)
    args = parser.parse_args()
    try:
        if args.action != "verify":
            content = bundle() if args.action == "create" else packets()[0 if args.action == "create-original" else 1]
            with args.path.open("xb") as stream:
                stream.write(content)
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
