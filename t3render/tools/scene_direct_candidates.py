"""Derive explicitly approximate point-light previews from scoped draw samples.

This is not a runtime light list. Shader identity is a pre-Reset hypothesis;
colors and brightness come from the fingerprinted offline actor export.
"""
import hashlib
import json
import math
import struct

import analyze_scene_bridge as bridge
import build_bridge_runtime_snapshot as runtime
import correlate_runtime_light_candidates as candidates
import scene_ir_to_d3d12_packet as preview
import trace_to_ir


def direct_light(draw, outcome, header, frame_begin, cb, pb, hb, shader_index, lights, actors, gain):
    constants = trace_to_ir.observed_vertex_constants(draw, outcome, header, frame_begin, cb)
    program = trace_to_ir.pre_reset_vertex_program(draw, outcome, header, pb, hb)
    if not constants or constants.get("status") != "sampled" or constants["count"] <= 21 or not program:
        raise ValueError("direct candidate requires sampled constants and a pre-Reset program")
    words = program["words"]
    names = shader_index.get(struct.pack(f"<{len(words)}I", *words), [])
    if len(names) != 1 or names[0].lower() not in candidates.PROGRAM_KINDS:
        raise ValueError("direct program hypothesis is missing or unsupported")
    kind = candidates.PROGRAM_KINDS[names[0].lower()]
    position = bridge.decode_floats(constants["bits"][80:84], 4)
    coefficients = bridge.decode_floats(constants["bits"][84:88], 4)
    if (position is None or position[3] != 1 or any(abs(x) > 1000000 for x in position[:3]) or
            coefficients is None or coefficients[1] != coefficients[2] or not 0 < coefficients[1] <= 1):
        raise ValueError("direct candidate constants are missing or unsupported")
    matches = [light for light in lights if light.position is not None and
               math.dist(light.position, position[:3]) <= candidates.TOLERANCE_T3]
    if len(matches) != 1 or matches[0].kind != kind:
        raise ValueError("direct candidate light position or kind is ambiguous")
    light = matches[0]
    result = {"offline_light_id": light.id, "program_hypothesis": names[0], "kind": kind}
    if kind == "spot":
        return {**result, "mode": "skipped_spot"}
    actor_name = light.id.rsplit(":", 1)[-1]
    actor = actors.get(actor_name)
    source = actor.get("light") if isinstance(actor, dict) else None
    energy = source.get("energy") if isinstance(source, dict) and source.get("kind") == "omni" else None
    if (isinstance(energy, bool) or not isinstance(energy, (int, float)) or not math.isfinite(energy) or
            not 0 <= energy <= 1 or light.color is None or any(not 0 <= x <= 1 for x in light.color)):
        raise ValueError("offline light color or approximate energy is unavailable")
    return {**result, "mode": "point_approximation", "position": position[:3],
            "inverse_radius_candidate": coefficients[1], "offline_color": list(light.color),
            "offline_energy": energy, "gain": gain}


def from_trace(scene, offline, offline_bytes, trace, kernel, actors_path, render_pass, stage, gain):
    if (isinstance(gain, bool) or not isinstance(gain, (int, float)) or
            not math.isfinite(gain) or not 0 <= gain <= 4):
        raise ValueError("direct preview gain must be between 0 and 4")
    digest = scene.source.fingerprints.get("trace")
    if (scene.source.kind != "runtime" or scene.source.camera_synchronized is not True or not digest or
            offline.source.kind != "offline" or scene.source.map != offline.source.map or
            scene.source.fingerprints.get("offline_scene") != hashlib.sha256(offline_bytes).hexdigest() or
            any(scene.source.fingerprints.get(role) != value for role, value in offline.source.fingerprints.items())):
        raise ValueError("direct candidate requires matching runtime and offline provenance")
    raw = preview.read_limited(actors_path, preview.MAX_INPUT)
    if hashlib.sha256(raw).hexdigest() != offline.source.fingerprints.get("actors"):
        raise ValueError("offline actor export fingerprint does not match the scene")
    export = json.loads(raw)
    items = export.get("actors") if isinstance(export, dict) else None
    if not isinstance(items, list) or any(not isinstance(a, dict) or not isinstance(a.get("name"), str) for a in items):
        raise ValueError("invalid offline actor export")
    actors = {a["name"]: a for a in items}
    if len(actors) != len(items):
        raise ValueError("offline actor names are ambiguous")
    bridge.trace_summary.summarize(trace)
    records = runtime.read_frame(trace, scene.source.frame_id, digest)
    header, frame_begin = bridge.one(records, "header"), bridge.one(records, "frame_begin")
    if not isinstance(header, dict) or not isinstance(frame_begin, dict):
        raise ValueError("trace frame or header missing")
    draws = [r for r in records if r.get("event") == "draw"]
    cb = candidates.within_budget(draws, header, "vertex_constant_capture", trace_to_ir.vertex_constant_frame_words)
    pb = candidates.within_budget(draws, header, "vertex_program_capture", trace_to_ir.vertex_program_frame_words)
    hb = candidates.within_budget(draws, header, "declaration_history", trace_to_ir.pre_reset_frame_words)
    if not (cb and pb and hb):
        raise ValueError("direct shader or constant observation budget is incomplete")
    index = candidates.shader_index(kernel, scene.source.fingerprints.get("kernel"))
    submissions = [s for s in scene.submissions if (s.render_pass, s.stage) == (render_pass, stage)]
    if not submissions:
        raise ValueError("direct candidate stage is missing")
    result = {}
    for sub in submissions:
        draw = bridge.one(records, "draw", sub.draw_id)
        outcome = bridge.one(records, "draw_result", sub.draw_id)
        result[sub.draw_id] = direct_light(draw, outcome, header, frame_begin, cb, pb, hb, index,
                                          offline.lights, actors, gain)
    return result
