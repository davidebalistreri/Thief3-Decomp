#!/usr/bin/env python3
"""Compare observed bridge metadata with an offline SceneIR fixture.

This produces a partial diagnostic report, not a runtime SceneSnapshot. The
probe captures BSP membership and metadata, not complete geometry payloads.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import sys

import analyze_scene_bridge as bridge

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ir"))
from scene_ir import SceneSnapshot  # noqa: E402


def compare_values(offline, runtime):
    if offline is None or runtime is None:
        return "unknown"
    return "match" if offline == runtime else "different"


def compare_f32(offline, runtime, *, bitwise=False):
    if offline is None or runtime is None or len(offline) != len(runtime):
        return "unknown"
    a = [bridge.f32_bits(x) for x in offline]
    b = [bridge.f32_bits(x) for x in runtime]
    if any(x is None for x in a + b):
        return "unknown"
    if bitwise:
        return "match" if a == b else "different"
    # Positive and negative zero are geometrically equivalent, while still
    # reporting the bit-level distinction separately.
    decode = lambda values: [struct.unpack("<f", struct.pack("<I", x))[0] for x in values]
    return "match" if decode(a) == decode(b) else "different"


def vertex_bounds(positions):
    if not positions:
        return None
    return [min(p[i] for p in positions) for i in range(3)] + [max(p[i] for p in positions) for i in range(3)]


def compare_sections(offline, runtime):
    if runtime is None:
        return "unknown"
    if len(offline) != len(runtime):
        return "different"
    unknown = False
    for a, b in zip(offline, runtime):
        if (a["first_index"] != b["first_index"] or a["index_count"] != b["index_count"] or
                a["index_count"] // 3 != b["triangles"]):
            return "different"
        for left, right in ((a["name"], b["name"]), (a["material"], b["material_name"])):
            if left is None or right is None:
                unknown = True
            elif left.lower() != right.lower():
                return "different"
    return "unknown" if unknown else "match"


def compare_bsp_sections(geometry, materials, runtime):
    if runtime is None:
        return "unknown"
    if len(geometry.sections) != len(runtime):
        return "different"
    unknown = False
    for offline, observed in zip(geometry.sections, runtime):
        if (offline.first_index != observed["first_index"] or offline.index_count != observed["index_count"] or
                offline.index_count // 3 != observed["triangles"]):
            return "different"
        material = materials.get(offline.material_id) if offline.material_id is not None else None
        if material is None or observed["material_name"] is None:
            unknown = True
        elif material.lower() != observed["material_name"].lower():
            return "different"
    return "unknown" if unknown else "match"


def compare_bsp_vertex_ranges(geometry, runtime):
    """Check that offline section indices fit in the observed vertex ranges.

    SceneIR does not retain the original first_vertex/vertex_count values, so
    containment is a necessary condition, not an exact range comparison.
    """
    if runtime is None:
        return "unknown"
    if len(geometry.sections) != len(runtime):
        return "different"
    for section, observed in zip(geometry.sections, runtime):
        indices = geometry.indices[section.first_index:section.first_index + section.index_count]
        first, count = observed["first_vertex"], observed["vertex_count"]
        if any(index < first or index >= first + count for index in indices):
            return "different"
    return "match"


def bsp_metadata_comparisons(block, materials, candidate):
    geometry = block.geometry
    counts = candidate["counts"]
    return {
        "vertex_count": compare_values(len(geometry.positions), counts["vertices"]),
        "index_count": compare_values(len(geometry.indices), counts["indices"]),
        "triangle_count": compare_values(len(geometry.indices) // 3, counts["triangles"]),
        "section_count": compare_values(len(geometry.sections), counts["sections"]),
        "bounds": compare_f32(vertex_bounds(geometry.positions), candidate["bounds_t3_units"]),
        "material_sections": compare_bsp_sections(geometry, materials, candidate["sections"]),
        "section_vertex_range_contains_indices": compare_bsp_vertex_ranges(geometry, candidate["sections"]),
    }


def compare_bsp_blocks(scene, materials, runtime_bsp, selected):
    """Find mutual unique metadata candidates across all observed/offline blocks."""
    if len(set(selected)) != len(selected):
        raise ValueError("BSP indices must be distinct")
    blocks = []
    for index in selected:
        found = [block for block in scene.bsp_blocks if block.source_index == index]
        if len(found) != 1:
            raise ValueError(f"BSP source index {index} is missing or ambiguous")
        blocks.append(found[0])
    candidates = runtime_bsp.get("candidates", [])
    all_checks = {(block.id, i): bsp_metadata_comparisons(block, materials, candidate)
                  for block in scene.bsp_blocks for i, candidate in enumerate(candidates)}
    def possible(checks):
        return "different" not in checks.values()
    reports = []
    for block in blocks:
        choices = []
        for i, candidate in enumerate(candidates):
            checks = all_checks[(block.id, i)]
            if not possible(checks):
                continue
            offline_competitors = sum(possible(all_checks[(other.id, i)]) for other in scene.bsp_blocks)
            choices.append({"runtime_source_index": candidate["source_index"],
                            "runtime_list_index": candidate["list_index"],
                            "checks": checks, "possible_offline_blocks": offline_competitors})
        status = "unknown"
        if runtime_bsp.get("candidates_complete") is True:
            if not choices:
                status = "no_metadata_candidate"
            elif len(choices) > 1 or choices[0]["possible_offline_blocks"] > 1:
                status = "ambiguous_metadata_candidates"
            elif all(value == "match" for value in choices[0]["checks"].values()):
                status = "unique_metadata_candidate"
        reports.append({"source_index": block.source_index, "resource_id": block.id,
                        "vertices": len(block.geometry.positions), "indices": len(block.geometry.indices),
                        "vertex_bounds": vertex_bounds(block.geometry.positions), "runtime": status,
                        "runtime_candidates": choices})
    return reports


def camera_position(view):
    """Invert a row-vector view matrix; a singular/non-affine result is unknown."""
    if (view is None or len(view) != 16 or any(not math.isfinite(v) for v in view) or
            any(abs(view[i]) > 1e-6 for i in (3, 7, 11)) or abs(view[15] - 1) > 1e-6):
        return None
    rows = [[float(view[r * 4 + c]) for c in range(4)] + [float(r == c) for c in range(4)]
            for r in range(4)]
    for col in range(4):
        pivot = max(range(col, 4), key=lambda row: abs(rows[row][col]))
        if abs(rows[pivot][col]) < 1e-12:
            return None
        rows[col], rows[pivot] = rows[pivot], rows[col]
        scale = rows[col][col]
        rows[col] = [x / scale for x in rows[col]]
        for r in range(4):
            if r != col:
                scale = rows[r][col]
                rows[r] = [a - scale * b for a, b in zip(rows[r], rows[col])]
    origin = rows[3][4:8]
    if not all(math.isfinite(x) for x in origin) or abs(origin[3]) < 1e-12:
        return None
    return [x / origin[3] for x in origin[:3]]


def compare(scene, records, frame, draw, render_pass, instance_id=None, bsp_indices=()):
    if scene.source.kind != "offline":
        raise ValueError("reference scene must have source=offline")
    if instance_id is None:
        candidates = [i for i in scene.mesh_instances if i.id.endswith(":" + bridge.TARGET_ACTOR)]
    else:
        candidates = [i for i in scene.mesh_instances if i.id == instance_id]
    if len(candidates) != 1:
        raise ValueError("offline actor instance is missing or ambiguous")
    instance = candidates[0]
    meshes = {m.id: m for m in scene.static_mesh_resources}
    materials = {m.id: m.name for m in scene.materials}
    mesh = meshes[instance.resource_id]
    result = bridge.analyze(records, frame, draw, render_pass)
    blocks = compare_bsp_blocks(scene, materials, result["runtime_bsp"], bsp_indices)
    node = result["runtime_mesh_node"]
    counts = node.get("counts") if node.get("status") == "candidate" else None
    runtime_bounds = node.get("bounds_t3_units") if counts else None
    target = result["actor_transform"]
    material_ids = instance.material_ids or tuple(section.material_id for section in mesh.geometry.sections)
    sections = [{"name": section.name, "first_index": section.first_index, "index_count": section.index_count,
                 "material": materials.get(material_id) if material_id is not None else None}
                for section, material_id in zip(mesh.geometry.sections, material_ids)]
    runtime_sections = node.get("sections") if counts else None
    observed = bridge.pick_observation(records, draw) if result["pass"] == "matches" else None
    camera = observed.get("current_camera") if isinstance(observed, dict) else None
    view = bridge.decode_floats(camera.get("derived_matrix_bits"), 16) if isinstance(camera, dict) and camera.get("stable") is True else None
    outer = observed.get("target", {}).get("outer", []) if isinstance(observed, dict) else []
    valid_outer = [x for x in outer if isinstance(x, dict) and x.get("member") is True and x.get("stable") is True
                   and isinstance(x.get("name"), str)]
    map_match = "unknown"
    if len(valid_outer) == 1 and not observed["target"].get("outer_truncated"):
        name = valid_outer[0]["name"]
        name = name.split(":", 1)[1] if name.lower().startswith("loadgame:") else name
        map_match = compare_values(name.lower(), scene.source.map.lower())
    return {
        "status": "partial",
        "frame": frame, "draw": draw, "pass": render_pass,
        "present_interval": result["present_interval"],
        "complete_observation_window": result["complete_observation_window"],
        "offline_map": scene.source.map, "map_name_match": map_match,
        "actor": {"name": bridge.TARGET_ACTOR, "offline_instance_id": instance.id,
                  "offline_identity": "same_name" if instance.id.endswith(":" + bridge.TARGET_ACTOR) else "caller_selected",
                  "runtime_membership": result["actor_membership_and_backlink"],
                  "transform": {"status": compare_f32(instance.transform, target["element_matrix_16"]),
                                "bitwise_status": compare_f32(instance.transform, target["element_matrix_16"], bitwise=True),
                                "offline": instance.transform, "runtime": target["element_matrix_16"]}},
        "mesh": {"offline_resource_id": mesh.id, "runtime_resource_id": None,
                 "resource_id_match": "unknown", "offline_name": mesh.name,
                 "runtime_asset_name": result["runtime_asset"].get("name"),
                 "runtime_node_name": node.get("name"),
                 "asset_property_audit": result["asset_property_audit"],
                 "node_name_matches_asset": node.get("name_matches_asset", "unknown"),
                 "vertex_count": {"status": compare_values(len(mesh.geometry.positions), counts.get("vertices") if counts else None),
                                  "offline": len(mesh.geometry.positions), "runtime": counts.get("vertices") if counts else None},
                 "index_count": {"status": compare_values(len(mesh.geometry.indices), counts.get("indices") if counts else None),
                                 "offline": len(mesh.geometry.indices), "runtime": counts.get("indices") if counts else None},
                 "section_count": {"status": compare_values(len(sections), counts.get("sections") if counts else None),
                                   "offline": len(sections), "runtime": counts.get("sections") if counts else None},
                 "bounds": {"status": compare_f32(vertex_bounds(mesh.geometry.positions), runtime_bounds),
                            "bitwise_status": compare_f32(vertex_bounds(mesh.geometry.positions), runtime_bounds, bitwise=True),
                            "offline_vertex_bounds": vertex_bounds(mesh.geometry.positions), "runtime_header_bounds": runtime_bounds},
                 "offline_skin": instance.skin, "offline_sections": sections,
                 "runtime_material_sections": runtime_sections,
                 "material_sections_match": compare_sections(sections, runtime_sections)},
        "camera": {"runtime_position_t3_units": camera_position(view), "offline_position_t3_units": None,
                   "position_match": "unknown", "viewport_match": result["comparisons"]["camera_viewport_vs_draw_viewport"]},
        "offline_bsp_blocks": blocks,
        "limitations": ["This report is not a runtime SceneSnapshot and does not copy offline geometry into one.",
                        "Matching node names or counts do not establish a qualified resource ID or GPU contents.",
                        "Offline bounds are derived from vertex positions; runtime bounds come from the node header.",
                        "Runtime material names are candidates read from the actor; skin identity remains unknown.",
                        "BSP metadata candidates are matched across all observed members and all scanned offline blocks; this does not prove payload identity.",
                        "SceneIR retains index ranges, but only checks that indices fit the runtime vertex ranges; exact offline vertex ranges are unavailable.",
                        "Camera position is derived from the inverse view, not independently measured.",
                        "Pass, Present and identity observations do not prove direct draw submission."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--scene", required=True, type=Path)
    parser.add_argument("--frame", required=True, type=int)
    parser.add_argument("--draw", required=True, type=int)
    parser.add_argument("--pass", dest="render_pass", required=True, type=int)
    parser.add_argument("--instance", help="explicit offline instance ID")
    parser.add_argument("--bsp", type=int, action="append", default=[])
    args = parser.parse_args(argv)
    try:
        if not bridge.uint(args.frame, 64) or not bridge.uint(args.draw, 64, True) or not bridge.uint(args.render_pass):
            raise ValueError("frame, draw and pass must be unsigned; draw must be nonzero")
        if any(not bridge.uint(index) for index in args.bsp):
            raise ValueError("BSP indices must be unsigned")
        with args.scene.open("rb") as stream:
            raw_scene = stream.read(256 * 1024 * 1024 + 1)
        if len(raw_scene) > 256 * 1024 * 1024:
            raise ValueError("offline scene exceeds the size limit")
        scene = SceneSnapshot.from_json(raw_scene)
        records, digest = bridge.read_selected(args.trace, args.frame, args.draw)
        report = compare(scene, records, args.frame, args.draw, args.render_pass, args.instance, args.bsp)
        report["trace_sha256"] = digest
        print(json.dumps(report, indent=2, allow_nan=False))
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
