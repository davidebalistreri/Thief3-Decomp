#!/usr/bin/env python3
"""Correlate scoped draw constants with offline light hardpoints, without claiming live light identity.

Only the existing synchronized capture and fingerprinted assets are read. This
does not hook TDS or emit runtime addresses, shader bytes, or a runtime light.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "assets"))
sys.path.insert(0, str(ROOT / "t3render" / "ir"))

from ibt import IBT  # noqa: E402
from scene_ir import SceneSnapshot  # noqa: E402
import analyze_scene_bridge as bridge  # noqa: E402
import build_bridge_runtime_snapshot as runtime  # noqa: E402
import build_scoped_scene_snapshot as scoped  # noqa: E402
import inventory_flesh_shaders as shaders  # noqa: E402
import scene_ir_to_d3d12_packet as packet  # noqa: E402
import trace_to_ir  # noqa: E402

TOLERANCE_T3 = 0.01
PROGRAM_KINDS = {"omnilightnormal11_light_vertex": "omni", "omnilight11_light_vertex": "omni",
                 "spotlightnormal11_light_vertex": "spot", "spotlight11_light_vertex": "spot"}


def shader_index(path, expected_digest):
    data = packet.read_limited(path, packet.MAX_INPUT)
    if not expected_digest or hashlib.sha256(data).hexdigest() != expected_digest:
        raise ValueError("kernel bundle fingerprint does not match offline fixture")
    index = {}
    with IBT(path, data=data) as bundle:
        for resource in bundle.of_type(29):
            try:
                parts = bundle.split(resource)
                kind, _, framed = shaders.classify(parts)
                if kind == "malformed":
                    raise ValueError("malformed shader resource in kernel bundle")
                if kind == "vertex" and framed:
                    index.setdefault(parts[5], []).append(resource.name)
            except (ValueError, EOFError, IndexError, struct.error) as exc:
                raise ValueError("cannot validate kernel shader index") from exc
    return index


def within_budget(draws, header, capability, count_words):
    limit = (header.get(capability) or {}).get("words_per_frame")
    sizes = [count_words(draw, header) for draw in draws]
    return type(limit) is int and limit > 0 and all(type(n) is int and n >= 0 for n in sizes) and sum(sizes) <= limit


def correlate(lights, observations):
    """Return unique positional matches supported by at least two bed sections."""
    groups, rejected = {}, {"no_unique_position": 0, "kind_mismatch": 0, "inconsistent_sample": 0}
    for obs in observations:
        possible = [(math.dist(light.position, obs["position"]), light) for light in lights if light.position is not None]
        possible = [(distance, light) for distance, light in possible if distance <= TOLERANCE_T3]
        if len(possible) != 1:
            rejected["no_unique_position"] += 1
            continue
        distance, light = possible[0]
        if light.kind != obs["kind"]:
            rejected["kind_mismatch"] += 1
            continue
        existing = groups.get(light.id)
        if existing is None:
            existing = {"offline_light_id": light.id, "offline_kind": light.kind,
                        "offline_position": light.position, "sampled_position": obs["position"],
                        "sampled_position_bits": obs["position_bits"], "distance_t3_units": distance,
                        "sections": set(), "draws": [], "programs": set(), "register21_y_values": set()}
            groups[light.id] = existing
        elif existing["sampled_position_bits"] != obs["position_bits"]:
            rejected["inconsistent_sample"] += 1
            existing["inconsistent"] = True
            continue
        existing["sections"].add(obs["section"])
        existing["draws"].append(obs["draw"])
        existing["programs"].add(obs["program"])
        existing["register21_y_values"].add(obs["coefficient"])
    matched = []
    for group in groups.values():
        if group.get("inconsistent") or len(group["sections"]) < 2:
            continue
        del group["sampled_position_bits"]
        group["sections"] = sorted(group["sections"])
        group["draws"] = sorted(group["draws"])
        group["programs"] = sorted(group["programs"])
        group["register21_y_values"] = sorted(group["register21_y_values"])
        matched.append(group)
    return sorted(matched, key=lambda item: item["offline_light_id"]), rejected


def analyze(scene, offline, offline_bytes, records, trace_digest, index, render_pass, stage):
    offline_digest = hashlib.sha256(offline_bytes).hexdigest()
    if scene.source.kind != "runtime" or scene.source.fingerprints.get("trace") != trace_digest or \
            scene.source.fingerprints.get("offline_scene") != offline_digest:
        raise ValueError("scene, trace and offline fixture fingerprints do not match")
    anchors = [s for s in scene.submissions if s.target_kind == "mesh_instance" and
               (s.render_pass, s.stage) == (render_pass, stage)]
    if not anchors:
        raise ValueError("scoped mesh submissions are missing")
    rebuilt, _ = scoped.build(offline, records, scene.source.frame_id, render_pass, stage,
                              anchors[0].draw_id, trace_digest, offline_digest)
    if rebuilt.to_dict() != scene.to_dict():
        raise ValueError("stage snapshot does not match the revalidated scoped capture")
    header, frame_begin = bridge.one(records, "header"), bridge.one(records, "frame_begin")
    if not isinstance(header, dict) or not isinstance(frame_begin, dict):
        raise ValueError("trace frame or header missing")
    draws = [r for r in records if r.get("event") == "draw"]
    constants_budget = within_budget(draws, header, "vertex_constant_capture", trace_to_ir.vertex_constant_frame_words)
    program_budget = within_budget(draws, header, "vertex_program_capture", trace_to_ir.vertex_program_frame_words)
    history_budget = within_budget(draws, header, "declaration_history", trace_to_ir.pre_reset_frame_words)
    if not (constants_budget and program_budget and history_budget):
        raise ValueError("shader or constant observation budget is incomplete")
    observations = []
    for sub in anchors:
        draw = bridge.one(records, "draw", sub.draw_id)
        result = bridge.one(records, "draw_result", sub.draw_id)
        constants = trace_to_ir.observed_vertex_constants(draw, result, header, frame_begin, constants_budget)
        program = trace_to_ir.pre_reset_vertex_program(draw, result, header, program_budget, history_budget)
        if not constants or constants.get("status") != "sampled" or constants["count"] <= 21 or not program:
            continue
        words = program["words"]
        binary = struct.pack(f"<{len(words)}I", *words)
        names = index.get(binary, [])
        if len(names) != 1 or names[0].lower() not in PROGRAM_KINDS:
            continue
        bits = constants["bits"][20 * 4:21 * 4]
        value = bridge.decode_floats(bits, 4)
        coeff = bridge.decode_floats(constants["bits"][21 * 4:22 * 4], 4)
        if value is None or coeff is None or value[3] != 1 or coeff[1] != coeff[2] or coeff[1] <= 0:
            continue
        observations.append({"draw": sub.draw_id, "section": sub.section, "position": value[:3],
                             "position_bits": tuple(bits[:3]), "kind": PROGRAM_KINDS[names[0].lower()],
                             "program": names[0], "coefficient": coeff[1]})
    matches, rejected = correlate(offline.lights, observations)
    return {"schema": "t3.scene.light-candidates", "version": 1,
            "status": "candidates" if matches else "none", "frame": scene.source.frame_id,
            "pass": render_pass, "stage": stage, "trace_sha256": trace_digest,
            "offline_scene_sha256": offline_digest, "kernel_sha256": offline.source.fingerprints.get("kernel"),
            "selection": "scoped_bed_submissions", "position_tolerance_t3_units": TOLERANCE_T3,
            "observations": len(observations), "matches": matches, "rejected": rejected,
            "limitations": ["Offline actor/light IDs and properties are candidates; no runtime light object or actor was observed.",
                            "Register 20 positions are sampled shader constants, not a semantic bridge light list or GPU readback.",
                            "Shader names are exact matches to pre-Reset program hypotheses, not proof of the active GPU program.",
                            "The register-21 coefficient is recorded without asserting its light-radius meaning or calibration.",
                            "Offline light color, enabled state, radius and intensity are not claimed as runtime state."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--scene", required=True, type=Path, help="scoped runtime stage snapshot")
    parser.add_argument("--offline-scene", required=True, type=Path)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--pass", dest="render_pass", required=True, type=int)
    parser.add_argument("--stage", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if not bridge.uint(args.render_pass) or not bridge.uint(args.stage):
            raise ValueError("invalid pass or stage")
        scene = SceneSnapshot.from_json(packet.read_limited(args.scene, packet.MAX_INPUT))
        raw = packet.read_limited(args.offline_scene, packet.MAX_INPUT)
        offline = SceneSnapshot.from_json(raw)
        bridge.trace_summary.summarize(args.trace)
        digest = scene.source.fingerprints.get("trace")
        records = runtime.read_frame(args.trace, scene.source.frame_id, digest)
        index = shader_index(args.kernel, offline.source.fingerprints.get("kernel"))
        report = analyze(scene, offline, raw, records, digest, index, args.render_pass, args.stage)
        with args.output.open("x", encoding="utf-8") as dest:
            json.dump(report, dest, indent=2, allow_nan=False)
            dest.write("\n")
        print(json.dumps(report, indent=2, allow_nan=False))
        return 0 if report["matches"] else 2
    except (OSError, ValueError, TypeError, KeyError, IndexError, EOFError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
