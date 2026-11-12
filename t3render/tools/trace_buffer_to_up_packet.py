#!/usr/bin/env python3
"""Convert one attested synthetic indexed-buffer trace into a DX12 UP packet.

The proxy observations can be incomplete or change before the driver uses
them. This converter is restricted to the isolated, known fixture; it is not
a general TDS frame replay or proof of captured GPU input.
"""

import argparse
import copy
import json
from pathlib import Path
import re
import struct
import sys
import tempfile

import trace_summary
import trace_to_up_packet as up


def _data(part, size, resource, offset):
    up.require(isinstance(part, dict), "buffer observation is missing")
    up.require(part.get("status") == "captured" and part.get("encoding") == "hex",
               "buffer observation is not fully captured")
    up.require(up.integer(part.get("byte_count")) == size and up.integer(part.get("byte_offset")) == offset,
               "buffer observation range differs from fixture")
    up.require(isinstance(resource, str) and re.fullmatch(r"0x[0-9a-fA-F]{1,8}", resource) is not None and
               int(resource[2:], 16) != 0 and part.get("resource") == resource,
               "buffer observation and binding identities differ")
    text = part.get("data")
    up.require(isinstance(text, str) and len(text) == size * 2 and re.fullmatch(r"[0-9a-fA-F]+", text),
               "buffer observation has incomplete or invalid bytes")
    return bytes.fromhex(text)


def convert(path, *, synthetic_fixture=False):
    up.require(synthetic_fixture is True, "--synthetic-fixture is required")
    records = up.validated_records(path)
    expected = ["header", "frame_begin", "clear", "clear_result", "draw", "draw_result", "frame_end", "capture_end"]
    up.require([record.get("event") for record in records] == expected,
               "fixture requires exactly one ordered clear and draw")
    capability = records[0].get("buffer_capture")
    up.require(isinstance(capability, dict) and capability.get("requested") is True and
               capability.get("encoding") == "hex" and
               up.integer(capability.get("component_limit_bytes")) == 256 * 1024 and
               up.integer(capability.get("frame_limit_bytes")) == 8 * 1024 * 1024,
               "buffer observation capability is missing or unsupported")
    draw, draw_result = records[4], records[5]
    up.require(draw.get("call") == "DrawIndexedPrimitive" and
               all(up.integer(draw.get(k)) == v for k, v in {
                   "primitive_type": 4, "primitive_count": 1, "min_vertex_index": 1,
                   "num_vertices": 3, "start_index": 2, "vertex_shader": 0x44, "pixel_shader": 0,
               }.items()), "unsupported indexed fixture draw")
    up.require(draw_result.get("buffer_stable") is True, "buffer draw window is missing or unstable")

    streams = draw.get("streams")
    up.require(isinstance(streams, list), "vertex streams are missing")
    stream0 = [item for item in streams if isinstance(item, dict) and up.integer(item.get("stream")) == 0]
    up.require(len(stream0) == 1, "fixture requires one stream 0")
    stream = stream0[0]
    index = draw.get("indices")
    vertex_desc = stream.get("description")
    up.require(up.hresult_succeeded(stream.get("hr")) and up.integer(stream.get("stride")) == 20 and
               isinstance(vertex_desc, dict) and up.integer(vertex_desc.get("size")) == 100 and
               up.integer(vertex_desc.get("fvf")) == 0x44,
               "invalid stream binding")
    up.require(isinstance(index, dict) and up.hresult_succeeded(index.get("hr")) and
               up.integer(index.get("base_vertex_index")) == 1,
               "invalid index binding")
    desc = index.get("description")
    up.require(isinstance(desc, dict) and up.integer(desc.get("format")) == 101 and
               up.integer(desc.get("size")) == 10,
               "fixture requires 16-bit indices")

    observed = draw.get("buffer_data")
    up.require(isinstance(observed, dict) and observed.get("source") == "application_lock_shadow",
               "buffer shadow observation is missing")
    vb, ib = observed.get("vertex"), observed.get("index")
    raw_vb = _data(vb, 60, stream.get("buffer"), 40)
    raw_ib = _data(ib, 6, index.get("buffer"), 4)
    up.require(all(up.integer(vb.get(k)) == v for k, v in {
        "stream": 0, "first_vertex": 2, "vertex_count": 3, "stride": 20,
    }.items()), "vertex observation metadata differs from fixture")
    up.require(all(up.integer(ib.get(k)) == v for k, v in {"index_count": 3, "index_format": 101}.items()),
               "index observation metadata differs from fixture")
    indices = struct.unpack("<3H", raw_ib)
    vertices = bytearray()
    for value in indices:
        up.require(1 <= value < 4, "index is outside the declared vertex range")
        offset = (1 + value) * 20 - 40
        up.require(0 <= offset and offset + 20 <= len(raw_vb), "indexed vertex is outside observed bytes")
        vertices.extend(raw_vb[offset:offset + 20])

    # Reuse the existing fixture-specific fixed-function, result, viewport and
    # vertex checks on a private synthetic UP equivalent. The original indexed
    # call and its observed bytes were checked above.
    derived = copy.deepcopy(records)
    equivalent = derived[4]
    equivalent.pop("buffer_data", None)
    equivalent.update(call="DrawPrimitiveUP", vertex_stride=20,
                      up_data={"vertex": {"status": "captured", "first_vertex": 0,
                               "vertex_count": 3, "stride": 20, "byte_offset": 0,
                               "byte_count": 60, "encoding": "hex", "data": vertices.hex()}, "index": None})
    derived[5].pop("buffer_stable", None)
    with tempfile.TemporaryDirectory(prefix="t3render-buffer-trace-") as directory:
        snapshot = Path(directory) / "equivalent.jsonl"
        snapshot.write_text("".join(json.dumps(record, allow_nan=False) + "\n" for record in derived), encoding="utf-8")
        return up.convert(snapshot, synthetic_fixture=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", help="complete diagnostic JSONL from the synthetic fixture")
    parser.add_argument("--synthetic-fixture", action="store_true", required=True,
                        help="attest the isolated fixture and its unobserved state")
    parser.add_argument("-o", "--output", help="new UP packet (default: stdout)")
    args = parser.parse_args(argv)
    try:
        packet = convert(args.trace, synthetic_fixture=args.synthetic_fixture)
        if args.output:
            with Path(args.output).open("x", encoding="ascii", newline="\n") as output:
                output.write(packet)
        else:
            sys.stdout.write(packet)
    except (OSError, UnicodeError, ValueError, trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print("Synthetic fixture only: observed buffer bytes do not establish general TDS replay.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
