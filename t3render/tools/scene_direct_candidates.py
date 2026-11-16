"""Derive approximate point and spot previews from scoped draw samples.

This is not a verified runtime light list. Shader bytes come from bounded
application-memory observations or pre-Reset hypotheses; colors and brightness
come from the fingerprinted offline actor export.
"""
import hashlib
import json
import math
import struct

import analyze_scene_bridge as bridge
import build_bridge_runtime_snapshot as runtime
import correlate_runtime_light_candidates as candidates
import scene_ir_to_d3d12_packet as preview
import scene_legacy_states as legacy_states
import trace_to_ir
from t3common import rotator_matrix


def spot_projector(constants, actor, position):
    """Check the sampled axis and projection against the offline actor rotation."""
    if constants["count"] <= 25:
        raise ValueError("spot candidate projector constants are unavailable")
    direction = bridge.decode_floats(constants["bits"][19 * 4:20 * 4], 4)
    rows = [bridge.decode_floats(constants["bits"][i * 4:(i + 1) * 4], 4) for i in (22, 23, 25)]
    rotation = actor.get("rotation") if isinstance(actor, dict) else None
    if (direction is None or any(row is None or any(abs(x) > 1000000 for x in row) for row in rows) or
            direction[3] != 0 or abs(sum(x*x for x in direction[:3]) - 1) > .001 or
            not isinstance(rotation, (list, tuple)) or len(rotation) != 3 or any(type(x) is not int for x in rotation)):
        raise ValueError("spot candidate axis or projector is invalid")
    forward = rotator_matrix(*rotation)[0]
    if any(abs(forward[i] + direction[i]) > .0001 or abs(rows[2][i] - forward[i]) > .0001 for i in range(3)):
        raise ValueError("spot candidate axis differs from the offline actor")
    def project(row, point):
        return sum(row[i] * point[i] for i in range(3)) + row[3]
    if abs(project(rows[2], position)) > .01:
        raise ValueError("spot candidate projector origin does not match its sampled position")
    center = [position[i] + forward[i] * 100 for i in range(3)]
    depth = project(rows[2], center)
    if depth <= 0 or any(not .48 <= project(row, center) / depth <= .52 for row in rows[:2]):
        raise ValueError("spot candidate projector does not center its axis")
    return rows


def projector_texture_observation(draw, name):
    """Capture metadata, never the transient texture pointer or presumed identity."""
    stages = draw.get("stages") if isinstance(draw, dict) else None
    matches = [s for s in stages if isinstance(s, dict) and s.get("stage") == 3] if isinstance(stages, list) else []
    if len(matches) != 1 or "texture_hr" in matches[0]:
        raise ValueError("spot projector texture observation is unavailable")
    item = matches[0]
    states, texture = item.get("states"), item.get("texture")
    expected = (("11", 3), ("13", 4), ("14", 4), ("15", 0), ("16", 2), ("17", 2), ("18", 0),
                ("20", 0), ("24", 260))
    if (not isinstance(states, dict) or any(type(states.get(k)) is not int or states[k] != v for k, v in expected) or
            not isinstance(texture, dict) or texture.get("type") != 3 or any(
                type(texture.get(key)) is not int or texture[key] <= 0 for key in ("width", "height", "levels", "format"))):
        raise ValueError("unsupported spot projector sampler or texture metadata")
    return {"name": name, **{key: texture[key] for key in ("width", "height", "levels", "format")}}


def normal_sampler_observation(draw, light):
    """Observe stage 0 only for sampled normal-light program hypotheses."""
    if (light["mode"] == "skipped_spot" or light["program_hypothesis"].lower() not in
            ("omnilightnormal11_light_vertex", "spotlightnormal11_light_vertex")):
        return None
    sampler = legacy_states.sampler_candidate(draw, 0)
    if sampler["status"] != "candidate":
        raise ValueError("normal map candidate requires a bound sampled texture")
    return sampler


def normal_shader_basis(constants):
    """Read constants used by the matched candidate normal-light vertex program."""
    if constants.get("count", 0) <= 38:
        raise ValueError("normal shader basis constants are incomplete")
    def reg(index):
        return bridge.decode_floats(constants["bits"][index*4:(index+1)*4], 4)
    base, scale, offset = reg(0), reg(16), reg(38)
    if (base != [0, .5, 1, 2] or scale is None or scale[3] not in (-1, 1) or
            offset is None or any(abs(x) > 1000000 for x in offset[:2])):
        raise ValueError("unsupported normal shader basis constants")
    return {"tangent_scale": scale[3], "uv_offset": offset[:2]}


def direct_light(draw, outcome, header, frame_begin, cb, pb, hb, shader_index, lights, actors, gain,
                 *, preview_spots=False, projector_candidate=None, preview_shader_basis=False):
    constants = trace_to_ir.observed_vertex_constants(draw, outcome, header, frame_begin, cb)
    program = trace_to_ir.candidate_vertex_program(draw, outcome, header, pb, hb)
    if not constants or constants.get("status") != "sampled" or constants["count"] <= 21 or not program:
        raise ValueError("direct candidate requires sampled constants and an observed or pre-Reset program")
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
    result = {"offline_light_id": light.id, "program_hypothesis": names[0],
              "program_source": program["program_source"], "kind": kind}
    if kind == "spot" and not preview_spots:
        return {**result, "mode": "skipped_spot"}
    actor_name = light.id.rsplit(":", 1)[-1]
    actor = actors.get(actor_name)
    source = actor.get("light") if isinstance(actor, dict) else None
    energy = source.get("energy") if isinstance(source, dict) and source.get("kind") == kind else None
    if (isinstance(energy, bool) or not isinstance(energy, (int, float)) or not math.isfinite(energy) or
            not 0 <= energy <= 1 or light.color is None or any(not 0 <= x <= 1 for x in light.color)):
        raise ValueError("offline light color or approximate energy is unavailable")
    result.update({"mode": "spot_approximation" if kind == "spot" else "point_approximation",
                   "position": position[:3], "inverse_radius_candidate": coefficients[1],
                   "offline_color": list(light.color), "offline_energy": energy, "gain": gain})
    if preview_shader_basis and names[0].lower() in ("omnilightnormal11_light_vertex", "spotlightnormal11_light_vertex"):
        result["shader_basis_candidate"] = normal_shader_basis(constants)
    if kind == "spot":
        result["projector_rows"] = spot_projector(constants, actor, position[:3])
        if projector_candidate is not None:
            result["projector_candidate"] = projector_texture_observation(draw, projector_candidate)
    return result


def offline_actors(scene, offline, offline_bytes, actors_path):
    """Validate the fixture and actor export before deriving offline properties."""
    digest = scene.source.fingerprints.get("trace")
    if (scene.source.kind != "runtime" or scene.source.camera_synchronized is not True or not digest or
            offline.source.kind != "offline" or scene.source.map != offline.source.map or
            scene.source.fingerprints.get("offline_scene") != hashlib.sha256(offline_bytes).hexdigest() or
            any(scene.source.fingerprints.get(role) != value for role, value in offline.source.fingerprints.items())):
        raise ValueError("runtime and offline provenance do not match")
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
    return actors


def from_trace(scene, offline, offline_bytes, trace, kernel, actors_path, render_pass, stage, gain,
               *, preview_spots=False, projector_candidate=None, preview_normal_maps=False, preview_shader_basis=False):
    if (isinstance(gain, bool) or not isinstance(gain, (int, float)) or
            not math.isfinite(gain) or not 0 <= gain <= 4):
        raise ValueError("direct preview gain must be between 0 and 4")
    if projector_candidate is not None and (not preview_spots or not isinstance(projector_candidate, str) or
                                             not projector_candidate or len(projector_candidate) > 128):
        raise ValueError("projector texture candidate requires an enabled spotlight preview")
    if preview_shader_basis and not preview_normal_maps:
        raise ValueError("shader normal basis requires normal map candidates")
    actors = offline_actors(scene, offline, offline_bytes, actors_path)
    digest = scene.source.fingerprints.get("trace")
    bridge.trace_summary.summarize(trace)
    records = runtime.read_frame(trace, scene.source.frame_id, digest)
    header, frame_begin = bridge.one(records, "header"), bridge.one(records, "frame_begin")
    if not isinstance(header, dict) or not isinstance(frame_begin, dict):
        raise ValueError("trace frame or header missing")
    draws = [r for r in records if r.get("event") == "draw"]
    cb = candidates.within_budget(draws, header, "vertex_constant_capture", trace_to_ir.vertex_constant_frame_words)
    pb = candidates.within_budget(draws, header, "vertex_program_capture", trace_to_ir.vertex_program_frame_words)
    hb = candidates.within_budget(draws, header, "declaration_history", trace_to_ir.pre_reset_frame_words)
    if not (cb and pb):
        raise ValueError("direct shader or constant observation budget is incomplete")
    index = candidates.shader_index(kernel, scene.source.fingerprints.get("kernel"))
    submissions = [s for s in scene.submissions if (s.render_pass, s.stage) == (render_pass, stage)]
    if not submissions:
        raise ValueError("direct candidate stage is missing")
    result = {}
    for sub in submissions:
        draw = bridge.one(records, "draw", sub.draw_id)
        outcome = bridge.one(records, "draw_result", sub.draw_id)
        light = direct_light(draw, outcome, header, frame_begin, cb, pb, hb, index,
                             offline.lights, actors, gain, preview_spots=preview_spots,
                             projector_candidate=projector_candidate, preview_shader_basis=preview_shader_basis)
        if preview_normal_maps:
            normal_sampler = normal_sampler_observation(draw, light)
            if normal_sampler is not None:
                light["normal_sampler_candidate"] = normal_sampler
        result[sub.draw_id] = light
    return result
