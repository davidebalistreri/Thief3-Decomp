"""Build a sampled distance-fog preview without claiming runtime pixel color.

Vertex program bytes are observed in application memory or are a pre-Reset
hypothesis. Fog color is from offline LevelInfo, not a pixel-shader constant.
"""
import math
import struct

import analyze_scene_bridge as bridge
import build_bridge_runtime_snapshot as runtime
import compare_bridge_fixture as comparison
import correlate_runtime_light_candidates as candidates
import scene_direct_candidates as direct
import trace_to_ir


def offline_fog(actors):
    levels = [actor for actor in actors.values() if actor.get("cls") == "LevelInfo"]
    if len(levels) != 1 or not isinstance(levels[0].get("properties"), dict):
        raise ValueError("offline LevelInfo is missing or ambiguous")
    properties = levels[0]["properties"]
    start, end = properties.get("DistanceFogStart"), properties.get("DistanceFogEnd")
    if (properties.get("bDistanceFog") is not True or any(
            isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x) for x in (start, end)) or
            not 0 <= start < end <= 1000000):
        raise ValueError("offline distance fog range is unavailable")
    raw = properties.get("DistanceFogColor")
    if not isinstance(raw, list):
        raise ValueError("offline fog color is unavailable")
    color = {}
    for entry in raw:
        if (not isinstance(entry, dict) or entry.get("name") not in ("R", "G", "B") or
                entry.get("type") != "Byte" or type(entry.get("value")) is not int or
                not 0 <= entry["value"] <= 255 or entry["name"] in color):
            raise ValueError("offline fog color is malformed")
        color[entry["name"]] = entry["value"]
    if set(color) != {"R", "G", "B"}:
        raise ValueError("offline fog color is incomplete")
    return {"offline_level_id": levels[0]["name"], "start": start, "end": end,
            "offline_color": [color[key] / 255.0 for key in ("R", "G", "B")]}


def fog_for_draw(draw, outcome, header, frame_begin, cb, pb, hb, shader_index, offline, camera):
    constants = trace_to_ir.observed_vertex_constants(draw, outcome, header, frame_begin, cb)
    program = trace_to_ir.candidate_vertex_program(draw, outcome, header, pb, hb)
    if not constants or constants.get("status") != "sampled" or constants["count"] <= 28 or not program:
        raise ValueError("fog candidate requires sampled constants and an observed or pre-Reset program")
    words = program["words"]
    names = shader_index.get(struct.pack(f"<{len(words)}I", *words), [])
    if len(names) != 1 or names[0].lower() != "fog11_light_vertex":
        raise ValueError("fog program hypothesis is missing or ambiguous")
    center = bridge.decode_floats(constants["bits"][26*4:27*4], 4)
    fog_range = bridge.decode_floats(constants["bits"][28*4:29*4], 4)
    if (center is None or center[3] != 1 or camera is None or math.dist(center[:3], camera) > .01 or
            fog_range is None or not math.isclose(fog_range[0], offline["start"], abs_tol=.001) or
            not math.isclose(fog_range[1], offline["end"], abs_tol=.001) or
            fog_range[2] <= 0 or not math.isclose(fog_range[2], 1 / (offline["end"] - offline["start"]), rel_tol=1e-6)):
        raise ValueError("fog camera or distance range does not match the offline fixture")
    return {"program_hypothesis": names[0], "program_source": program["program_source"],
            "offline_level_id": offline["offline_level_id"],
            "camera_position": center[:3], "start": fog_range[0], "inverse_range": fog_range[2],
            "offline_color": offline["offline_color"]}


def from_trace(scene, offline, offline_bytes, trace, kernel, actors_path, render_pass, stage):
    actors = direct.offline_actors(scene, offline, offline_bytes, actors_path)
    fog = offline_fog(actors)
    digest = scene.source.fingerprints.get("trace")
    bridge.trace_summary.summarize(trace)
    records = runtime.read_frame(trace, scene.source.frame_id, digest)
    header, frame_begin = bridge.one(records, "header"), bridge.one(records, "frame_begin")
    if not isinstance(header, dict) or not isinstance(frame_begin, dict):
        raise ValueError("trace frame or header missing")
    draws = [row for row in records if row.get("event") == "draw"]
    cb = candidates.within_budget(draws, header, "vertex_constant_capture", trace_to_ir.vertex_constant_frame_words)
    pb = candidates.within_budget(draws, header, "vertex_program_capture", trace_to_ir.vertex_program_frame_words)
    hb = candidates.within_budget(draws, header, "declaration_history", trace_to_ir.pre_reset_frame_words)
    if not (cb and pb):
        raise ValueError("fog shader or constant observation budget is incomplete")
    index = candidates.shader_index(kernel, scene.source.fingerprints.get("kernel"))
    submissions = [s for s in scene.submissions if (s.render_pass, s.stage) == (render_pass, stage)]
    if not submissions:
        raise ValueError("fog candidate stage is missing")
    camera = comparison.camera_position(scene.camera.view)
    result = {}
    for sub in submissions:
        draw = bridge.one(records, "draw", sub.draw_id)
        outcome = bridge.one(records, "draw_result", sub.draw_id)
        result[sub.draw_id] = fog_for_draw(draw, outcome, header, frame_begin, cb, pb, hb, index, fog, camera)
    return result
