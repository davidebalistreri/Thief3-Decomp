"""Apply explicit, semantic visual controls to offline light hypotheses.

These are visual calibration controls, not observations of runtime light state.
"""
import json
import math

import scene_ir_to_d3d12_packet as preview

MAX_OVERRIDES = 1024
MAX_MULTIPLIER = 4.0


def parse(raw):
    data = json.loads(raw, object_pairs_hook=preview._pairs, parse_constant=preview._invalid_number)
    if (not isinstance(data, dict) or
            data.get("schema") != "t3.scene.light-preview-overrides" or type(data.get("version")) is not int or
            data["version"] not in (1, 2) or
            set(data) != ({"schema", "version", "lights"} if data["version"] == 1 else
                         {"schema", "version", "lights", "receivers"}) or
            not isinstance(data["lights"], list) or
            (data["version"] == 2 and not isinstance(data["receivers"], list)) or
            not 1 <= len(data["lights"]) + (len(data["receivers"]) if data["version"] == 2 else 0) <= MAX_OVERRIDES):
        raise ValueError("invalid light preview override document")
    result = {}
    for item in data["lights"]:
        if (not isinstance(item, dict) or not {"light_id", "rgb_multiplier"} <= set(item) or
                set(item) - {"light_id", "rgb_multiplier", "falloff_mix"}):
            raise ValueError("invalid light preview override entry")
        identity, rgb = item["light_id"], item["rgb_multiplier"]
        if (not isinstance(identity, str) or not identity.startswith("light:") or len(identity) <= 6 or len(identity) > 1024 or
                identity in result or not isinstance(rgb, list) or len(rgb) != 3 or any(
                    isinstance(v, bool) or not isinstance(v, (int, float)) or
                    not 0 <= v <= MAX_MULTIPLIER or not math.isfinite(v) for v in rgb)):
            raise ValueError("invalid or duplicate light preview override")
        value = {"preview_rgb_multiplier": list(rgb)}
        if "falloff_mix" in item:
            blend = item["falloff_mix"]
            if (isinstance(blend, bool) or not isinstance(blend, (int, float)) or
                    not 0 <= blend <= 1 or not math.isfinite(blend)):
                raise ValueError("invalid light preview falloff mix")
            value["preview_falloff_mix"] = blend
        result[identity] = value
    if data["version"] == 2:
        for item in data["receivers"]:
            if not isinstance(item, dict) or set(item) != {"light_id", "target_id", "rgb_multiplier"}:
                raise ValueError("invalid receiver preview override entry")
            light, target, rgb = item["light_id"], item["target_id"], item["rgb_multiplier"]
            if (not isinstance(light, str) or not light.startswith("light:") or not 6 < len(light) <= 1024 or
                    not isinstance(target, str) or not (target.startswith("actor:") or target.startswith("bsp:")) or
                    len(target.split(":")) < 4 or any(not part for part in target.split(":")) or
                    len(target) > 1024 or (light, target) in result or
                    not isinstance(rgb, list) or len(rgb) != 3 or any(
                        isinstance(v, bool) or not isinstance(v, (int, float)) or
                        not 0 <= v <= MAX_MULTIPLIER or not math.isfinite(v) for v in rgb)):
                raise ValueError("invalid or duplicate receiver preview override")
            result[(light, target)] = {"preview_receiver_rgb_multiplier": list(rgb)}
    return result


def apply(candidates, overrides, targets=None):
    """Return new candidates; reject IDs without an enabled direct contribution."""
    if not isinstance(candidates, dict) or not isinstance(overrides, dict) or not overrides:
        raise ValueError("light preview overrides require direct candidates")
    if any(isinstance(key, tuple) for key in overrides) and (
            not isinstance(targets, dict) or not candidates.keys() <= targets.keys()):
        raise ValueError("receiver overrides require semantic submission targets")
    used, result = set(), {}
    for draw_id, item in candidates.items():
        if not isinstance(item, dict):
            raise ValueError("invalid direct candidate")
        identity = item.get("offline_light_id")
        value = overrides.get(identity)
        receiver_key = (identity, targets[draw_id]) if targets is not None and draw_id in targets else None
        receiver = overrides.get(receiver_key)
        result[draw_id] = dict(item)
        if item.get("mode") in ("point_approximation", "spot_approximation"):
            if value is not None:
                result[draw_id].update(value)
                used.add(identity)
            if receiver is not None:
                result[draw_id].update(receiver)
                used.add(receiver_key)
    if used != set(overrides):
        raise ValueError("light preview override does not match an enabled direct candidate")
    return result
