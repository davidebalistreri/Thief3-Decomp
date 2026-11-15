#!/usr/bin/env python3
"""Create and verify a subpixel texture-sampling temporal fixture."""
import argparse
from pathlib import Path
import struct
import sys

from scene_pbr_indirect_fixture import IDENTITY
import scene_temporal_resolve as temporal


def fixture():
    body = bytearray(struct.pack("<8sIIff32f4f4fIII", b"T3PBR01\0", 64, 64, 0, 1, *IDENTITY, *IDENTITY,
                                 0, 0, 2, 1, 1, 1, 1, 1, 4, 1, 1))
    body.extend(struct.pack("<8f", 0, 0, 2, .1, 1, 1, 1, 0))
    body.extend(struct.pack("<III", 2, 2, 1))
    body.extend(bytes((0, 0, 0, 255, 255, 255, 255, 255,
                       0, 0, 0, 255, 255, 255, 255, 255)))
    for rgba in ((128, 128, 255, 255), (204, 204, 204, 255), (255, 255, 255, 255)):
        body.extend(struct.pack("<III4B", 1, 1, 1, *rgba))
    body.extend(struct.pack("<5I", 0, 1, 2, 3, 6))
    for x, y, u in ((-1,-1,0), (-1,1,0), (1,1,16), (-1,-1,0), (1,1,16), (1,-1,16)):
        body.extend(struct.pack("<12f", x, y, .5, u, .5, 0, 0, 1, 0, 0, 0, 0))
    return bytes(body)


def _pixels(raw):
    header = b"P6\n64 64\n255\n"
    if not raw.startswith(header) or len(raw) != len(header)+64*64*3:
        raise ValueError("invalid temporal fixture image")
    return raw[len(header):]


def verify(reference, jittered, resolved):
    reference, jittered, resolved = map(_pixels, (reference, jittered, resolved))
    indices = [(y*64+x)*3 for y in range(2, 62) for x in range(2, 62)]
    before = sum(abs(jittered[i]-reference[i]) for i in indices)/len(indices)
    after = sum(abs(resolved[i]-reference[i]) for i in indices)/len(indices)
    if before < 1 or after >= before*.75:
        raise ValueError(f"temporal resolve did not improve the jittered texture: {before:.3f} -> {after:.3f}")
    return {"jittered_mean_error": before, "resolved_mean_error": after}


def reference(samples, packet):
    if len(samples) != 4:
        raise ValueError("four subpixel samples are required")
    frames = [temporal.parse(raw, packet) for raw in samples]
    if sorted(frame["jitter"] for frame in frames) != [(-.375, 0), (-.125, 0), (.125, 0), (.375, 0)]:
        raise ValueError("fixture requires the four specified horizontal jitter offsets")
    for frame in frames[1:]:
        temporal.compatible(frames[0], frame)
    count = frames[0]["width"]*frames[0]["height"]
    colors = [temporal._decode(frame["raw"], temporal.HEADER.size, count) for frame in frames]
    out = bytearray(frames[0]["raw"])
    for pixel in range(count):
        value = [sum(color[pixel*4+k] for color in colors)/4 for k in range(4)]
        struct.pack_into("<4e", out, temporal.HEADER.size+pixel*8, *value)
    return temporal.image(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "reference", "verify"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--jittered", type=Path)
    parser.add_argument("--packet", type=Path)
    parser.add_argument("--sample", type=Path, action="append", default=[])
    args = parser.parse_args()
    try:
        if args.action == "create":
            with args.path.open("xb") as stream:
                stream.write(fixture())
        elif args.action == "reference":
            if args.packet is None:
                raise ValueError("--packet is required")
            contents = reference([p.read_bytes() for p in args.sample], args.packet.read_bytes())
            with args.path.open("xb") as stream:
                stream.write(contents)
        else:
            if args.reference is None or args.jittered is None:
                raise ValueError("--reference and --jittered are required")
            print("temporal_subpixel_sampling=PASS", verify(args.reference.read_bytes(), args.jittered.read_bytes(), args.path.read_bytes()))
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
