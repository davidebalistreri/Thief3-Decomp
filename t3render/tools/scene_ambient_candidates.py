"""Read a bounded ambient-color hypothesis for revalidated SceneIR draws.

Program bytes are application-memory observations or pre-Reset hypotheses,
not GPU shader readback.
This adapter does not add lights or legacy shader objects to SceneIR.
"""
import struct

import analyze_scene_bridge as bridge
import build_bridge_runtime_snapshot as runtime
import correlate_runtime_light_candidates as light_candidates
import trace_to_ir


def ambient_color(draw, result, header, frame_begin, constants_budget, program_budget, history_budget, shader_index,
                  *, program_source=None):
    constants = trace_to_ir.observed_vertex_constants(draw, result, header, frame_begin, constants_budget)
    program = trace_to_ir.candidate_vertex_program(draw, result, header, program_budget, history_budget)
    if not constants or constants.get("status") != "sampled" or constants["count"] <= 18 or not program:
        raise ValueError("ambient candidate requires sampled constants and an observed or pre-Reset program")
    words = program["words"]
    binary = struct.pack(f"<{len(words)}I", *words)
    names = shader_index.get(binary, [])
    if len(names) != 1 or names[0].lower() != "ambient11_light_vertex":
        raise ValueError("ambient program hypothesis is missing or ambiguous")
    base = bridge.decode_floats(constants["bits"][:4], 4)
    color = bridge.decode_floats(constants["bits"][18 * 4:19 * 4], 4)
    if base is None or base[2] != 1 or color is None or any(not 0 <= value <= 1 for value in color):
        raise ValueError("ambient constants are missing or unsupported")
    if program_source is not None:
        program_source.append(program["program_source"])
    return color


def from_trace(scene, trace, kernel, render_pass, stage, *, program_sources=None):
    """Use only paired draw samples from the fingerprinted trace and kernel."""
    digest = scene.source.fingerprints.get("trace")
    if scene.source.kind != "runtime" or scene.source.camera_synchronized is not True or not digest:
        raise ValueError("ambient candidate requires synchronized runtime provenance")
    bridge.trace_summary.summarize(trace)
    records = runtime.read_frame(trace, scene.source.frame_id, digest)
    header, frame_begin = bridge.one(records, "header"), bridge.one(records, "frame_begin")
    if not isinstance(header, dict) or not isinstance(frame_begin, dict):
        raise ValueError("trace frame or header missing")
    draws = [r for r in records if r.get("event") == "draw"]
    cb = light_candidates.within_budget(draws, header, "vertex_constant_capture", trace_to_ir.vertex_constant_frame_words)
    pb = light_candidates.within_budget(draws, header, "vertex_program_capture", trace_to_ir.vertex_program_frame_words)
    hb = light_candidates.within_budget(draws, header, "declaration_history", trace_to_ir.pre_reset_frame_words)
    if not (cb and pb):
        raise ValueError("ambient shader or constant observation budget is incomplete")
    index = light_candidates.shader_index(kernel, scene.source.fingerprints.get("kernel"))
    submissions = [s for s in scene.submissions if (s.render_pass, s.stage) == (render_pass, stage)]
    if not submissions:
        raise ValueError("ambient candidate stage is missing")
    result = {}
    for sub in submissions:
        draw = bridge.one(records, "draw", sub.draw_id)
        outcome = bridge.one(records, "draw_result", sub.draw_id)
        source = []
        result[sub.draw_id] = ambient_color(draw, outcome, header, frame_begin, cb, pb, hb, index,
                                           program_source=source)
        if program_sources is not None:
            program_sources[sub.draw_id] = source[0]
    return result
