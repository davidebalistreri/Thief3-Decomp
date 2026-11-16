#!/usr/bin/env python3
"""Compare a Present-scoped diagnostic with an offline SceneIR light fixture.

The viewer record is a producer attestation, not independent proof of game
execution or complete light coverage. No result is promoted to SceneIR.
"""
import argparse
import hashlib
import io
import json
import math
from pathlib import Path
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ir"))
from scene_ir import SceneSnapshot  # noqa: E402

MAX_LOG_BYTES = 96 * 1024 * 1024
MAX_LINE_BYTES = 1024 * 1024
MAX_LIGHTS = 32
RAW_SCALARS = ("field_58", "field_5c", "field_60", "field_64", "field_74_bits", "field_78_bits",
               "field_7c_bits", "field_88_bits", "field_8c_bits", "field_90_bits")
RAW_VECTORS = ("field_24_bits", "field_34_bits", "field_68_bits")


def strict_json(value):
    def unique(pairs):
        result = {}
        for key, item in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key: {key}")
            result[key] = item
        return result
    def invalid(value):
        raise ValueError(f"non-finite JSON number: {value}")
    return json.loads(value, object_pairs_hook=unique, parse_constant=invalid)


def unsigned(value, name, maximum=(1 << 64) - 1):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError(f"{name}: invalid unsigned integer")
    return value


def finite_vector(value, length, name):
    if not isinstance(value, list) or len(value) != length:
        raise ValueError(f"{name}: wrong vector length")
    if any(isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x) for x in value):
        raise ValueError(f"{name}: non-finite value")
    return value


def camera_position(view):
    m = finite_vector(view, 16, "view")
    if any(abs(m[i]) > 1e-6 for i in (3, 7, 11)) or abs(m[15] - 1) > 1e-6:
        return None
    a, b, c, d, e, f, g, h, i = (m[j] for j in (0, 1, 2, 4, 5, 6, 8, 9, 10))
    cof = (e*i-f*h, f*g-d*i, d*h-e*g, c*h-b*i, a*i-c*g, b*g-a*h, b*f-c*e, c*d-a*f, a*e-b*d)
    det = a*cof[0] + b*cof[1] + c*cof[2]
    if not math.isfinite(det) or abs(det) < 1e-12:
        return None
    position = [-sum(m[12+row] * cof[col*3+row] for row in range(3)) / det for col in range(3)]
    return position if all(math.isfinite(x) and abs(x) <= 1_000_000 for x in position) else None


def select_frame(log, session, frame, allow_synthetic=False):
    if len(log) > MAX_LOG_BYTES:
        raise ValueError("viewer log exceeds the diagnostic size limit")
    if not isinstance(session, str) or not session or len(session) > 512:
        raise ValueError("session: invalid identity")
    unsigned(frame, "frame")
    found = []
    for line in io.BytesIO(log):
        line = line.rstrip(b"\r\n")
        if len(line) > MAX_LINE_BYTES:
            raise ValueError("viewer log line exceeds the diagnostic size limit")
        event = strict_json(line)
        if not isinstance(event, dict):
            raise ValueError("viewer log entry is not an object")
        if event.get("event") == "frame" and event.get("session_id") == session and event.get("frame") == frame:
            found.append(event)
    if len(found) != 1:
        raise ValueError("expected exactly one frame for the requested session and frame ID")
    record = found[0]
    permitted = ("live_process", "synthetic") if allow_synthetic else ("live_process",)
    if record.get("source") not in permitted:
        raise ValueError("frame source is not an allowed live observation")
    if record["source"] == "live_process" and not re.fullmatch(r"live:[0-9a-f]{32}", session):
        raise ValueError("live process session ID has an invalid format")
    return record


def validate_record(record):
    for key in ("frame", "map_epoch", "device_epoch", "continuity", "render_pass", "stage"):
        unsigned(record.get(key), key)
    if record["frame"] == (1 << 64) - 1:
        raise ValueError("frame cannot have a valid light generation")
    if not record["map_epoch"] or not record["device_epoch"] or not record["continuity"]:
        raise ValueError("frame has no active map/device epoch or continuity")
    finite_vector(record.get("projection"), 16, "projection")
    camera = camera_position(record.get("view"))
    viewport = record.get("viewport")
    if not isinstance(viewport, dict) or any(k not in viewport for k in ("x", "y", "width", "height", "min_depth", "max_depth")):
        raise ValueError("viewport: missing fields")
    for key in ("x", "y", "width", "height"):
        unsigned(viewport[key], "viewport." + key, (1 << 32) - 1)
    if not viewport["width"] or not viewport["height"] or viewport["x"] + viewport["width"] > (1 << 32) - 1 or viewport["y"] + viewport["height"] > (1 << 32) - 1:
        raise ValueError("viewport: invalid extent")
    depths = finite_vector([viewport["min_depth"], viewport["max_depth"]], 2, "viewport depth")
    if depths[0] > depths[1]:
        raise ValueError("viewport: invalid depth")
    if record.get("light_coverage") != "partial" or record.get("light_raw_available") is not True:
        raise ValueError("frame lacks partial, Present-associated raw light diagnostics")
    if record.get("light_details_truncated") is not False:
        raise ValueError("light details were truncated or unavailable")
    details, raw = record.get("light_details"), record.get("light_raw")
    if not isinstance(details, list) or len(details) > MAX_LIGHTS or not isinstance(raw, dict):
        raise ValueError("invalid light diagnostics")
    if unsigned(record.get("visual_lights"), "visual_lights", MAX_LIGHTS) != len(details):
        raise ValueError("light detail count does not match the frame")
    items = raw.get("items")
    if not isinstance(items, list) or len(items) != len(details):
        raise ValueError("raw light count does not match the frame")
    listed = unsigned(raw.get("listed"), "listed", 128)
    skipped = unsigned(raw.get("skipped"), "skipped", 128)
    if len(items) + skipped != listed or type(raw.get("truncated")) is not bool:
        raise ValueError("invalid raw list coverage")
    by_id = {}
    for detail in details:
        if not isinstance(detail, dict) or not isinstance(detail.get("id"), str) or detail["id"] in by_id:
            raise ValueError("duplicate or invalid light detail ID")
        unsigned(detail.get("generation"), "generation")
        if detail["generation"] != record["frame"] + 1 or detail.get("binding") != "candidate":
            raise ValueError("light detail is not a candidate")
        by_id[detail["id"]] = detail
    out = []
    seen = set()
    required = {"id", "generation", *RAW_SCALARS, *RAW_VECTORS}
    for item in items:
        if (not isinstance(item, dict) or set(item) != required or not isinstance(item.get("id"), str) or
                item["id"] not in by_id or item["id"] in seen):
            raise ValueError("raw light identity or fields are inconsistent")
        seen.add(item["id"])
        detail = by_id[item["id"]]
        if unsigned(item["generation"], "raw generation") != detail["generation"]:
            raise ValueError("raw light generation differs from frame")
        for field in RAW_SCALARS:
            unsigned(item[field], field, (1 << 32) - 1)
        for field in RAW_VECTORS:
            if not isinstance(item[field], list) or len(item[field]) != 3:
                raise ValueError(f"{field}: wrong vector length")
            for value in item[field]:
                unsigned(value, field, (1 << 32) - 1)
        position = [struct.unpack("<f", struct.pack("<I", bits))[0] for bits in item["field_34_bits"]]
        valid = all(math.isfinite(x) and abs(x) <= 1_000_000 for x in position)
        if valid:
            observed = finite_vector(detail.get("position"), 3, "position")
            if detail.get("position_evidence") != "candidate" or any(
                    struct.unpack("<I", struct.pack("<f", observed[i]))[0] != item["field_34_bits"][i] for i in range(3)):
                raise ValueError("position differs from its raw float bits")
        elif detail.get("position") is not None or detail.get("position_evidence") != "unknown":
            raise ValueError("invalid raw position was presented as known")
        out.append((item, position if valid else None))
    return camera, out, {"listed": listed, "skipped": skipped, "truncated": raw["truncated"]}


def compare(record, scene, log_sha256, fixture_sha256):
    if scene.source.kind != "offline" or scene.source.frame_id is not None or not scene.source.fingerprints.get("map"):
        raise ValueError("a fingerprinted offline scene fixture is required")
    session, origin = record.get("session_id"), record.get("source")
    if not isinstance(session, str) or not session or len(session)>512 or origin not in ("live_process", "synthetic"):
        raise ValueError("invalid frame provenance")
    if origin == "live_process" and not re.fullmatch(r"live:[0-9a-f]{32}", session):
        raise ValueError("live process session ID has an invalid format")
    if (scene.conventions.length_unit, scene.conventions.axes, scene.conventions.up_axis) != ("t3_unit", "t3_xyz", "z"):
        raise ValueError("offline fixture does not use T3 coordinates and units")
    camera, samples, coverage = validate_record(record)
    map_name, fingerprint = scene.source.map, scene.source.fingerprints["map"]
    if record.get("map") != map_name or record.get("map_sha256") != fingerprint:
        raise ValueError("viewer catalog does not match the offline map fingerprint")
    runtime_prefix = f"light-actor:{map_name}:{fingerprint}:"
    offline_prefix = f"light:{map_name}:{fingerprint}:"
    offline = {}
    for light in scene.lights:
        if not light.id.startswith(offline_prefix):
            raise ValueError("offline light ID does not match the fixture fingerprint")
        offline.setdefault(light.id[len(offline_prefix):].lower(), []).append(light)
    parsed, names = [], {}
    for item, position in samples:
        if not item["id"].startswith(runtime_prefix):
            raise ValueError("runtime light ID does not match the offline map fingerprint")
        name, sep, number_text = item["id"][len(runtime_prefix):].rpartition(":")
        if not sep or not name or not re.fullmatch(r"[ -~]+", name) or ":" in name or not re.fullmatch(r"0|[1-9][0-9]*", number_text):
            raise ValueError("invalid runtime FName identity")
        number = int(number_text)
        unsigned(number, "FName number", 65535)
        display = name + (f"__{number-1}" if number else "")
        names[display.lower()] = names.get(display.lower(), 0) + 1
        parsed.append((item, position, display))
    result = []
    for item, position, display in parsed:
        candidates = offline.get(display.lower(), [])
        ambiguous = names[display.lower()] != 1 or len(candidates) > 1
        match = candidates[0] if len(candidates) == 1 and not ambiguous else None
        distance = None
        if match and position is not None and match.position is not None:
            distance = math.dist(position, match.position)
        result.append({"runtime_id": item["id"], "offline_id_candidate": match.id if match else None,
                       "identity": "ambiguous" if ambiguous else "candidate_name_match" if match else "unmatched",
                       "runtime_position_candidate": position,
                       "offline_position": list(match.position) if match and match.position is not None else None,
                       "distance_t3_units": distance,
                       "raw_words": {key: item[key] for key in (*RAW_SCALARS, *RAW_VECTORS)}})
    return {"schema": "t3.light_diagnostic_comparison", "version": 1,
            "observation": {"source": record["source"], "session_id": record["session_id"],
                            "frame": record["frame"], "render_pass": record["render_pass"], "stage": record["stage"],
                            "map_epoch": record["map_epoch"], "device_epoch": record["device_epoch"],
                            "continuity": record["continuity"], "viewer_log_sha256": log_sha256},
            "offline_fixture": {"source": "offline", "map": map_name, "map_sha256": fingerprint,
                                "scene_sha256": fixture_sha256},
            "camera_position_candidate": camera, "coverage": coverage, "lights": result,
            "limitations": ["The viewer record is a producer attestation, not independent proof of TDS execution.",
                            "Sampling is within the Present interval, not necessarily at Present itself.",
                            "Name-based associations and positions are candidates; global identity and complete visibility are unproven.",
                            "Raw words do not establish physical color, intensity, enabled state, radius or cone.",
                            "The map fingerprint and reference positions are from an offline fixture."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("viewer_log", type=Path)
    parser.add_argument("offline_scene", type=Path)
    parser.add_argument("--session", required=True)
    parser.add_argument("--frame", required=True, type=int)
    parser.add_argument("--synthetic-fixture", action="store_true", help="allow explicitly synthetic test records")
    parser.add_argument("--output", type=Path, help="create a new diagnostic JSON file")
    args = parser.parse_args(argv)
    try:
        if args.viewer_log.stat().st_size > MAX_LOG_BYTES:
            raise ValueError("viewer log exceeds the diagnostic size limit")
        log = args.viewer_log.read_bytes()
        fixture = args.offline_scene.read_bytes()
        record = select_frame(log, args.session, args.frame, args.synthetic_fixture)
        result = compare(record, SceneSnapshot.from_json(fixture), hashlib.sha256(log).hexdigest(), hashlib.sha256(fixture).hexdigest())
        rendered = json.dumps(result, indent=2, allow_nan=False) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as output:
                output.write(rendered)
        else:
            print(rendered, end="")
    except (OSError, ValueError, UnicodeError, OverflowError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
