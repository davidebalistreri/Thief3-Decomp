#!/usr/bin/env python3
"""Summarize shader versions and bounded framing in a local kernel IBT.

No shader words, resource names or game data are emitted. Framing is not shader
semantic validation and does not connect assets to runtime handles.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "assets"))
import ibt
import trace_to_ir


def classify(parts):
    if (len(parts) != 6 or any(len(parts[i]) != 4 for i in (0, 1, 2, 4)) or
            struct.unpack("<I", parts[2])[0] != len(parts[3]) or
            struct.unpack("<I", parts[4])[0] != len(parts[5]) or
            not parts[3].endswith(b"\0") or not parts[5] or len(parts[5]) % 4):
        return "malformed", None, False
    kind = struct.unpack("<I", parts[0])[0]
    if kind not in (0, 1):
        return "malformed", None, False
    version = struct.unpack_from("<I", parts[5])[0]
    if kind == 1:
        return "pixel", version, False
    if len(parts[5]) > 1024 * 4:
        return "vertex", version, False
    words = list(struct.unpack(f"<{len(parts[5]) // 4}I", parts[5]))
    return "vertex", version, trace_to_ir.framed_vertex_program(words)


def inventory(path):
    data = Path(path).read_bytes()
    result = {"schema": "t3render.flesh-shader-inventory.v1", "source_sha256": hashlib.sha256(data).hexdigest(),
              "resources": 0, "malformed": 0, "vertex_versions": {}, "vertex_framed": {},
              "pixel_versions": {}, "limitations": ["Framing does not validate shader semantics or runtime bindings.",
              "The inventory contains no shader bytecode and is not a TDS frame capture."]}
    counts = {"vertex_versions": collections.Counter(), "vertex_framed": collections.Counter(),
              "pixel_versions": collections.Counter()}
    with ibt.IBT(path, data=data) as bundle:
        for resource in bundle.of_type(29):
            result["resources"] += 1
            try:
                kind, version, framed = classify(bundle.split(resource))
            except (EOFError, ValueError, struct.error, IndexError):
                kind, version, framed = "malformed", None, False
            if kind == "malformed":
                result["malformed"] += 1
                continue
            label = f"0x{version:08x}"
            counts[kind + "_versions"][label] += 1
            if kind == "vertex" and framed:
                counts["vertex_framed"][label] += 1
    result.update({key: dict(sorted(value.items())) for key, value in counts.items()})
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", help="local Kernel_GFXALL.ibt")
    parser.add_argument("-o", "--output", help="new JSON inventory; never overwritten")
    args = parser.parse_args(argv)
    try:
        encoded = json.dumps(inventory(args.bundle), indent=2) + "\n"
        if args.output:
            with Path(args.output).open("x", encoding="utf-8") as dest:
                dest.write(encoded)
        else:
            sys.stdout.write(encoded)
    except (OSError, ValueError, struct.error, EOFError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
