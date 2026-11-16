"""Place explicitly uncertain light candidates in a scoped SceneIR snapshot.

Positions are sampled shader constants; IDs are offline candidates and kinds
depend on application-memory shader candidates. Runtime light objects and
their properties are not observed.
"""
from collections import defaultdict
import math
from pathlib import Path

import scene_direct_candidates as direct
from scene_ir import SceneSnapshot
from t3common import resolve_map


LIMITATION = ("Candidate lights use sampled draw constants and fingerprinted offline identities; "
              "their kinds depend on pre-Reset shader hypotheses. Runtime light objects, "
              "colors, intensity, radius and enabled state are not observed.")
OBSERVED_LIMITATION = ("Candidate lights use sampled draw constants and fingerprinted offline identities; "
                       "their kinds depend on same-frame application-memory shader candidates, not GPU readback. "
                       "Runtime light objects, colors, intensity, radius and enabled state are not observed.")
LIMITATIONS = (LIMITATION, OBSERVED_LIMITATION)


def augment(scene, observations, render_pass, stage):
    """Add one candidate per consistently sampled offline light identity."""
    if (scene.version != 1 or scene.source.kind != "runtime" or scene.source.camera_synchronized is not True or
            scene.lights or any(value in scene.source.limitations for value in LIMITATIONS)):
        raise ValueError("light candidates require an unaugmented synchronized scoped snapshot")
    submissions = {s.draw_id: s for s in scene.submissions if (s.render_pass, s.stage) == (render_pass, stage)}
    if not submissions or not isinstance(observations, dict) or set(observations) != set(submissions):
        raise ValueError("light candidates require observations for every scoped draw")
    groups = defaultdict(list)
    sources = set()
    for draw_id, item in observations.items():
        if not isinstance(item, dict):
            raise ValueError("invalid light candidate")
        key, kind, position = item.get("offline_light_id"), item.get("kind"), item.get("position")
        source = item.get("program_source", "pre_reset_hypothesis")
        if (not isinstance(key, str) or not key.startswith("light:") or kind not in ("omni", "spot") or
                source not in ("pre_reset_hypothesis", "same_frame_application_memory") or
                item.get("mode") != ("point_approximation" if kind == "omni" else "spot_approximation") or
                not isinstance(position, (tuple, list)) or len(position) != 3 or any(
                    isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x) or abs(x) > 1000000
                    for x in position)):
            raise ValueError("invalid light candidate identity, kind or position")
        groups[key].append((draw_id, kind, tuple(position), source))
        sources.add(source)
    lights, evidence = [], []
    for key, samples in sorted(groups.items()):
        if len({(kind, position) for _, kind, position, _ in samples}) != 1:
            raise ValueError("light candidate has inconsistent sampled positions or kinds")
        if len(samples) < 2:
            continue
        _, kind, position, _ = samples[0]
        identity = "candidate:" + key
        lights.append({"id": identity, "kind": kind, "position": position,
                       "color": None, "radius": None, "intensity": None, "enabled": None})
        entry = {"id": identity, "supporting_draws": sorted(draw for draw, _, _, _ in samples)}
        if any(source == "same_frame_application_memory" for _, _, _, source in samples):
            entry["program_sources"] = sorted({source for _, _, _, source in samples})
        evidence.append(entry)
    if not lights:
        raise ValueError("no multiply observed light candidates")
    data = scene.to_dict()
    if "pre_reset_hypothesis" in sources:
        data["source"]["limitations"].append(LIMITATION)
    if "same_frame_application_memory" in sources:
        data["source"]["limitations"].append(OBSERVED_LIMITATION)
    data["lights"] = lights
    return SceneSnapshot.from_dict(data), evidence


def from_trace(scene, offline, offline_bytes, trace, actors, game, render_pass, stage):
    if actors is None or game is None:
        raise ValueError("light candidates require the fingerprinted actor export and game directory")
    local = resolve_map(Path(game), scene.source.map, ".ibt")
    kernel = local.parent / "Kernel_GFXALL.ibt"
    observations = direct.from_trace(scene, offline, offline_bytes, trace, kernel, actors,
                                     render_pass, stage, 1.0, preview_spots=True)
    return augment(scene, observations, render_pass, stage)
