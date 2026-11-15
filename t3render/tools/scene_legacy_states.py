"""Translate bounded legacy render-state observations to neutral draw state.

This adapter is optional and diagnostic. SceneIR remains the source of geometry,
visibility and provenance. No COM objects, addresses or D3D enums enter it.
"""
import hashlib
import math
import struct

import analyze_scene_bridge as bridge
import build_bridge_runtime_snapshot as runtime
import build_scoped_scene_snapshot as scoped

# The serialized vocabulary is defined in scene_packet.hpp, independently of D3D.
BLEND_FACTORS = {1: 0, 2: 1, 3: 2, 4: 3, 5: 4, 6: 5, 7: 8, 8: 9, 9: 6, 10: 7}
CULL = {1: 0, 2: 2, 3: 1}  # D3D8 culls CW/CCW; packet culls front/back (CW front).
DEFAULT = {"depth_enable": 1, "depth_write": 1, "depth_compare": 2, "cull": 1,
           "blend_enable": 0, "blend_src": 1, "blend_dst": 0, "blend_op": 0,
           "alpha_compare": 0, "alpha_ref": 0, "color_mask": 15}
FIELDS = tuple(DEFAULT)


def validate(state):
    choices = {"depth_enable": range(2), "depth_write": range(2), "depth_compare": range(1, 9),
               "cull": range(3), "blend_enable": range(2), "blend_src": range(10), "blend_dst": range(10),
               "blend_op": range(5), "alpha_compare": range(9), "alpha_ref": range(256), "color_mask": range(16)}
    if not isinstance(state, dict) or set(state) != set(choices) or any(
            type(state[k]) is not int or state[k] not in allowed for k, allowed in choices.items()):
        raise ValueError("invalid draw state")
    return state


def translate(states):
    if not isinstance(states, dict):
        raise ValueError("missing captured render states")
    def value(number, allowed):
        item = states.get(str(number))
        if type(item) is not int or item not in allowed:
            raise ValueError(f"missing or unsupported render state {number}")
        return item
    depth = value(7, (0, 1))
    write = value(14, (0, 1))
    test = value(15, (0, 1))
    blend = value(27, (0, 1))
    fog = value(28, (0, 1))
    lighting = value(137, (0, 1))
    if fog or lighting:
        raise ValueError("fixed-function fog or lighting is not implemented")
    return {"depth_enable": depth, "depth_write": write,
            "depth_compare": value(23, range(1, 9)), "cull": CULL[value(22, CULL)],
            "blend_enable": blend, "blend_src": BLEND_FACTORS[value(19, BLEND_FACTORS)],
            "blend_dst": BLEND_FACTORS[value(20, BLEND_FACTORS)], "blend_op": value(171, range(1, 6)) - 1,
            "alpha_compare": value(25, range(1, 9)) if test else 0,
            "alpha_ref": value(24, range(256)) if test else 0,
            "color_mask": value(168, range(16))}


def sampler_candidate(draw, stage):
    """Read a stage's sampler and texture metadata, without a resource identity."""
    entries = draw.get("stages")
    if not isinstance(entries, list):
        raise ValueError("missing texture stages")
    selected = [item for item in entries if isinstance(item, dict) and type(item.get("stage")) is int and item["stage"] == stage]
    if len(selected) != 1:
        raise ValueError("sampler stage missing or ambiguous")
    entry = selected[0]
    if "texture" not in entry:
        raise ValueError("texture observation missing")
    if "texture_hr" in entry:
        if type(entry["texture_hr"]) is not int or entry["texture_hr"] >= 0:
            raise ValueError("invalid texture getter result")
        return {"status": "unavailable", "stage": stage}
    texture = entry["texture"]
    if texture is None:
        return {"status": "unbound", "stage": stage}
    state = entry.get("states")
    if not isinstance(state, dict) or any(type(state.get(key)) is not int or state[key] != expected
            for key, expected in (("13", 1), ("14", 1), ("16", 2), ("17", 2), ("18", 2), ("20", 0), ("24", 0))):
        raise ValueError("unsupported sampler state or texture transform")
    bits = state.get("19")
    if type(bits) is not int or not 0 <= bits <= 0xffffffff:
        raise ValueError("invalid sampler LOD bias")
    bias = struct.unpack("<f", struct.pack("<I", bits))[0]
    if not math.isfinite(bias) or not -16 <= bias < 16:
        raise ValueError("sampler LOD bias exceeds renderer bounds")
    if not isinstance(texture, dict) or texture.get("type") != 3 or any(
            type(texture.get(key)) is not int or texture[key] <= 0 for key in ("width", "height", "levels", "format")):
        raise ValueError("unsupported texture metadata")
    return {"status": "candidate", "stage": stage, "lod_bias": bias,
            "width": texture["width"], "height": texture["height"],
            "levels": texture["levels"], "format": texture["format"]}


def from_trace(scene, offline, offline_bytes, trace, render_pass, stage, *, sampler_stage=None, samplers=None):
    """Rebuild the scoped snapshot before pairing any state to its draw."""
    digest = scene.source.fingerprints.get("trace")
    offline_digest = hashlib.sha256(offline_bytes).hexdigest()
    if not digest or scene.source.fingerprints.get("offline_scene") != offline_digest:
        raise ValueError("scene lacks matching trace or offline fixture fingerprints")
    bridge.trace_summary.summarize(trace)
    records = runtime.read_frame(trace, scene.source.frame_id, digest)
    anchors = [s for s in scene.submissions if s.target_kind == "mesh_instance" and
               (s.render_pass, s.stage) == (render_pass, stage)]
    if not anchors:
        raise ValueError("state matching requires a scoped mesh anchor")
    rebuilt, _ = scoped.build(offline, records, scene.source.frame_id, render_pass, stage,
                              anchors[0].draw_id, digest, offline_digest)
    if rebuilt.to_dict() != scene.to_dict():
        raise ValueError("scene does not match the revalidated scoped capture")
    result = {}
    for submission in scene.submissions:
        if (submission.render_pass, submission.stage) == (render_pass, stage):
            draw = bridge.one(records, "draw", submission.draw_id)
            if draw is None:
                raise ValueError("captured draw missing or ambiguous")
            result[submission.draw_id] = translate(draw.get("render_states"))
            if sampler_stage is not None:
                if samplers is None or type(sampler_stage) is not int or not 0 <= sampler_stage < 8:
                    raise ValueError("invalid sampler stage")
                samplers[submission.draw_id] = sampler_candidate(draw, sampler_stage)
    return result
