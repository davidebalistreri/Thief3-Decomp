#!/usr/bin/env python3
"""Build a bounded runtime-selected SceneIR snapshot with asset-backed geometry.

The bridge supplies the frame, selection, camera, transform and material
observations. The offline fixture supplies geometry only after independent
metadata and lock-shadow comparisons. A shadow match is not a direct link
between an observed list entry and a draw or a GPU readback.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

import analyze_scene_bridge as bridge
import compare_bridge_fixture as comparison
import correlate_trace_actor as actor_trace
import scene_ir_to_d3d12_packet as packet
import trace_to_ir

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ir"))
from scene_ir import SceneSnapshot  # noqa: E402

IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def read_frame(path, frame, expected_digest):
    """Read only one frame and its header from the previously validated trace."""
    digest = hashlib.sha256()
    records = []
    with path.open("rb") as stream:
        for line in stream:
            digest.update(line)
            item = json.loads(line, object_pairs_hook=bridge.trace_summary._object,
                              parse_constant=bridge.trace_summary._constant)
            if item.get("event") == "header" or item.get("frame") == frame:
                records.append(item)
    if digest.hexdigest() != expected_digest:
        raise ValueError("trace changed after validation")
    return records


def verified_section(geometry, offline_section, runtime_section, draw, result, header,
                     within_budget, expected_world, camera, reference_viewport, thread):
    """Return true only for the supported packed format and matching asset fields."""
    if (draw.get("call") != "DrawIndexedPrimitive" or draw.get("primitive_type") != 4 or
            draw.get("thread_id") != thread or
            draw.get("min_vertex_index") != runtime_section["first_vertex"] or
            draw.get("num_vertices") != runtime_section["vertex_count"] or
            draw.get("start_index") != runtime_section["first_index"] or
            draw.get("primitive_count") != runtime_section["triangles"] or
            offline_section.first_index != runtime_section["first_index"] or
            offline_section.index_count != runtime_section["index_count"] or
            runtime_section["index_count"] != runtime_section["triangles"] * 3 or
            draw.get("viewport") != reference_viewport):
        return False
    transforms = draw.get("transforms") or {}
    world = transforms.get("world") or []
    if (not world or comparison.compare_f32(expected_world, world[0]) != "match" or
            bridge.compare_matrix(camera.get("derived_matrix_bits"), transforms.get("view")) != "equal_bits" or
            bridge.compare_matrix(camera.get("projection_bits"), transforms.get("projection")) != "equal_bits"):
        return False
    declaration = trace_to_ir.observed_declaration(draw, result, header)
    if (declaration["status"] != "candidate" or
            not actor_trace.matches_packed_declaration(declaration["words"])):
        return False
    payload = trace_to_ir.buffered_data(draw, result, header, True, within_budget)
    vertex, index = payload["vertex"], payload["index"]
    if (vertex["status"] != "captured" or index["status"] != "captured" or
            vertex["stride_bytes"] != 32 or index["index_format"] != "uint16" or geometry.uv0 is None):
        return False
    first, count = runtime_section["first_vertex"], runtime_section["vertex_count"]
    if first + count > len(geometry.positions):
        return False
    vertices, indices = bytes.fromhex(vertex["data"]), bytes.fromhex(index["data"])
    try:
        expected_indices = geometry.indices[offline_section.first_index:offline_section.first_index + offline_section.index_count]
        return (len(vertices) == count * 32 and
                all(vertices[i * 32:i * 32 + 12] == struct.pack("<3f", *geometry.positions[first + i]) and
                    vertices[i * 32 + 16:i * 32 + 24] == struct.pack("<2f", *geometry.uv0[first + i])
                    for i in range(count)) and
                indices == struct.pack(f"<{len(expected_indices)}H", *expected_indices))
    except (OverflowError, struct.error):
        return False


def find_section_draws(geometry, runtime_sections, section_indices, records, camera, reference, world):
    headers = [r for r in records if r.get("event") == "header"]
    if len(headers) != 1:
        raise ValueError("trace header missing or ambiguous")
    draws = [r for r in records if r.get("event") == "draw"]
    results = {r["op_id"]: r for r in records if r.get("event") == "draw_result"}
    sizes = [trace_to_ir.buffered_frame_bytes(draw) for draw in draws]
    limit = headers[0].get("buffer_capture", {}).get("frame_limit_bytes")
    within_budget = type(limit) is int and all(type(n) is int for n in sizes) and sum(sizes) <= limit
    reference_observation = bridge.pick_observation(records, reference["op_id"])
    if reference_observation is None or reference_observation.get("current_camera") != camera:
        raise ValueError("reference draw is not paired with the sampled probe observation")
    selected = []
    for i in section_indices:
        if i < 0 or i >= len(geometry.sections) or i >= len(runtime_sections):
            raise ValueError("selected section is out of range")
        matching = [draw["op_id"] for draw in draws if draw.get("op_id") in results and
                    bridge.pick_observation(records, draw["op_id"]) is reference_observation and
                    verified_section(geometry, geometry.sections[i], runtime_sections[i], draw,
                                     results[draw["op_id"]], headers[0], within_budget, world, camera,
                                     reference["viewport"], reference["thread_id"])]
        if not matching:
            raise ValueError(f"section {i} lacks a verified same-frame lock shadow")
        selected.append({"section": i, "draw": min(matching), "matching_draws": len(matching)})
    return selected


def build(scene, scene_digest, selected, frame_records, trace_digest, frame, draw, render_pass,
          bsp_indices, mesh_sections):
    if not bsp_indices or not mesh_sections or len(set(mesh_sections)) != len(mesh_sections):
        raise ValueError("select distinct mesh sections and at least one BSP block")
    report = comparison.compare(scene, selected, frame, draw, render_pass, bsp_indices=bsp_indices)
    analysis = bridge.analyze(selected, frame, draw, render_pass)
    mesh_check = report["mesh"]
    if (report["complete_observation_window"] != "synchronized" or report["map_name_match"] != "match" or
            analysis["pass"] != "matches" or analysis["actor_uniqueness"] != "one_in_complete_observed_scans" or
            report["actor"]["runtime_membership"] != "matched" or
            report["actor"]["transform"]["status"] != "match" or
            analysis["runtime_asset"].get("matches_expected_name") is not True or
            mesh_check["asset_property_audit"] != "consistent" or
            mesh_check["node_name_matches_asset"] is not True or
            any(mesh_check[key]["status"] != "match" for key in ("vertex_count", "index_count", "section_count", "bounds")) or
            mesh_check["material_sections_match"] != "match" or
            analysis["comparisons"]["camera_derived_vs_draw_view"] != "equal_bits" or
            analysis["comparisons"]["camera_projection_vs_draw_projection"] != "equal_bits"):
        raise ValueError("runtime actor, asset, camera or synchronized window is insufficiently verified")
    instances = [item for item in scene.mesh_instances if item.id == report["actor"]["offline_instance_id"]]
    mesh = next(item for item in scene.static_mesh_resources if item.id == instances[0].resource_id)
    if len([item for item in scene.static_mesh_resources if item.name.casefold() == mesh.name.casefold()]) != 1:
        raise ValueError("offline mesh name is ambiguous")
    observation = bridge.pick_observation(selected, draw)
    reference = bridge.one(frame_records, "draw", draw)
    camera = observation.get("current_camera")
    view = bridge.decode_floats(camera.get("derived_matrix_bits"), 16)
    projection = bridge.decode_floats(camera.get("projection_bits"), 16)
    viewport = camera.get("viewport_bits")
    depths = bridge.decode_floats(viewport[4:], 2) if isinstance(viewport, list) and len(viewport) == 6 else None
    if view is None or projection is None or depths is None or reference is None:
        raise ValueError("runtime camera is incomplete")
    mesh_proof = find_section_draws(mesh.geometry, mesh_check["runtime_material_sections"], mesh_sections,
                                    frame_records, camera, reference, analysis["actor_transform"]["element_matrix_16"])
    bsp_proofs = []
    for item in report["offline_bsp_blocks"]:
        if item["runtime"] != "unique_metadata_candidate":
            raise ValueError(f"BSP {item['source_index']} has no unique runtime metadata candidate")
        runtime_index = item["runtime_candidates"][0]["runtime_source_index"]
        candidates = [v for v in analysis["runtime_bsp"]["candidates"] if v["source_index"] == runtime_index]
        if len(candidates) != 1 or comparison.compare_f32(IDENTITY, candidates[0]["transform"]) != "match":
            raise ValueError("BSP runtime transform or membership is ambiguous")
        block = next(b for b in scene.bsp_blocks if b.source_index == item["source_index"])
        proofs = find_section_draws(block.geometry, candidates[0]["sections"], range(len(block.geometry.sections)),
                                    frame_records, camera, reference, IDENTITY)
        bsp_proofs.append({"offline_source_index": block.source_index, "runtime_source_index": runtime_index,
                           "sections": proofs})
    data = scene.to_dict()
    data["source"] = {"kind": "runtime", "map": scene.source.map, "frame_id": frame,
                      "camera_synchronized": True,
                      "fingerprints": {**scene.source.fingerprints, "trace": trace_digest, "offline_scene": scene_digest},
                      "limitations": ["Runtime selection, camera, mesh transform and material names are probe observations; geometry and resource IDs are asset-backed offline candidates.",
                                      "Resource ID qualification and direct ownership of the matching draws by the sampled objects are unproven.",
                                      "A last preceding probe observation is temporal evidence, not proof that a draw executed in that pass or call.",
                                      "Position, UV and indices are verified only for the sections listed in the sidecar; unlisted mesh sections remain unverified.",
                                      "Lock shadows are not GPU readbacks; BSP metadata matching is not direct payload identity.",
                                      "BSP source_index and block_id are from the offline scanner; runtime indices are in the sidecar.",
                                      "Skin, light state, near/far and full runtime scene coverage are unknown.",
                                      "The camera viewport is sampled from the probe and may differ from the matching draw viewport."]}
    data["camera"] = {"source": {"kind": "runtime", "frame_id": frame, "fingerprint": trace_digest},
                      "view": view, "projection": projection,
                      "viewport": dict(zip(("x", "y", "width", "height", "min_depth", "max_depth"), viewport[:4] + depths)),
                      "near": None, "far": None}
    data["static_mesh_resources"] = [item for item in data["static_mesh_resources"] if item["id"] == mesh.id]
    data["mesh_instances"] = [item for item in data["mesh_instances"] if item["id"] == instances[0].id]
    data["mesh_instances"][0]["transform"] = analysis["actor_transform"]["element_matrix_16"]
    data["mesh_instances"][0]["skin"] = None
    data["bsp_blocks"] = [item for item in data["bsp_blocks"] if item["source_index"] in bsp_indices]
    used = {s["material_id"] for b in data["bsp_blocks"] for s in b["geometry"]["sections"]}
    used.update(s["material_id"] for m in data["static_mesh_resources"] for s in m["geometry"]["sections"])
    used.update(data["mesh_instances"][0]["material_ids"])
    data["materials"] = [item for item in data["materials"] if item["id"] in used]
    data["lights"] = []
    snapshot = SceneSnapshot.from_dict(data)
    manifest = {"schema": "t3.scene.runtime-evidence", "version": 1, "frame": frame, "pass": render_pass,
                "reference_draw": draw, "trace_sha256": trace_digest, "offline_scene_sha256": scene_digest,
                "selection": "runtime_bridge", "camera": "runtime_bridge", "transform": "runtime_bridge",
                "geometry": "offline_asset_subset_verified_against_lock_shadows", "direct_draw_ownership": "unknown",
                "shadow_scope": "same_present_thread_and_last_preceding_probe_observation",
                "runtime_qualified_resource_id": "unknown", "mesh_sections": mesh_proof, "bsp": bsp_proofs}
    return snapshot, manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--scene", required=True, type=Path)
    parser.add_argument("--frame", required=True, type=int)
    parser.add_argument("--draw", required=True, type=int)
    parser.add_argument("--pass", dest="render_pass", required=True, type=int)
    parser.add_argument("--bsp", required=True, type=int, action="append", help="offline scanner index")
    parser.add_argument("--mesh-section", required=True, type=int, action="append")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if (not bridge.uint(args.frame, 64) or not bridge.uint(args.draw, 64, True) or
                not bridge.uint(args.render_pass) or any(not bridge.uint(i) for i in args.bsp + args.mesh_section)):
            raise ValueError("invalid frame, draw, pass or section index")
        raw = packet.read_limited(args.scene, packet.MAX_INPUT)
        scene = SceneSnapshot.from_json(raw)
        selected, digest = bridge.read_selected(args.trace, args.frame, args.draw)
        frame_records = read_frame(args.trace, args.frame, digest)
        snapshot, manifest = build(scene, hashlib.sha256(raw).hexdigest(), selected, frame_records,
                                   digest, args.frame, args.draw, args.render_pass, args.bsp, args.mesh_section)
        body = snapshot.to_json(indent=2) + "\n"
        manifest["snapshot_sha256"] = hashlib.sha256(body.encode("utf-8")).hexdigest()
        packet.write_pair(args.output, body, manifest)
        print(json.dumps(manifest, indent=2))
    except (OSError, ValueError, TypeError, KeyError, IndexError, bridge.trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
