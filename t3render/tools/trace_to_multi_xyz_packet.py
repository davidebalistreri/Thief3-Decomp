#!/usr/bin/env python3
"""Convert an explicitly attested multi-draw camera fixture for DX12 replay.

Only observed, successful, serial XYZ|DIFFUSE UP triangles are accepted.
Unobserved D3D8 operations are still possible: this is not a TDS frame reader.
"""

import argparse
from pathlib import Path
import sys

import trace_summary
import trace_to_up_packet as single


MAX_DRAWS = 32


def convert(path, *, synthetic_fixture=False):
    single.require(synthetic_fixture is True,
                   "--synthetic-fixture is required; general frame replay is not supported")
    records = single.validated_records(path)
    events = [record.get("event") for record in records]
    single.require(len(records) >= 10 and events[:4] == ["header", "frame_begin", "clear", "clear_result"]
                   and events[-2:] == ["frame_end", "capture_end"], "invalid scene event sequence")
    middle = records[4:-2]
    single.require(len(middle) % 2 == 0 and 2 <= len(middle) // 2 <= MAX_DRAWS,
                   "scene must contain between 2 and 32 draws")
    count = len(middle) // 2
    single.require([r.get("event") for r in middle] == ["draw", "draw_result"] * count,
                   "scene requires ordered draw/result pairs with no extra events")
    header, begin, clear, clear_result = records[:4]
    end, capture_end = records[-2:]
    single.require(single.integer(end.get("draws")) == count, "frame draw count does not match scene")
    last_id = single.integer(clear.get("op_id"), minimum=1)
    single.require(last_id is not None, "invalid clear operation identifier")
    packets = []
    for draw, result in zip(middle[::2], middle[1::2]):
        draw_id = single.integer(draw.get("op_id"), minimum=1)
        single.require(draw_id is not None and draw_id > last_id, "draw operation order is invalid")
        last_id = draw_id
        end_for_draw = {**end, "draws": 1}
        packet = single._convert_records([header, begin, clear, clear_result, draw, result,
                                          end_for_draw, capture_end]).splitlines()
        single.require(packet[0] == "T3R_XYZ_V1", "scene requires XYZ camera draws")
        packets.append(packet)
    return "\n".join(["T3R_MULTI_XYZ_V1", packets[0][1], packets[0][2], str(count)] +
                     [line for packet in packets for line in packet[3:]]) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", help="complete diagnostic JSONL trace")
    parser.add_argument("--synthetic-fixture", action="store_true",
                        help="attest that only the isolated multi-draw fixture produced this frame")
    parser.add_argument("-o", "--output", help="new output packet (default: stdout)")
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
    print("Synthetic fixture only: unobserved D3D8 operations remain outside validation.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
