#!/usr/bin/env python3
"""Verify scoped runtime mesh/BSP draws against an offline SceneIR fixture.

Only same-frame, balanced call scopes and unique semantic matches count.
When lock shadows are available they are checked separately from scope proof.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

import analyze_scene_bridge as bridge
import build_bridge_runtime_snapshot as builder
import compare_bridge_fixture as comparison
import scene_ir_to_d3d12_packet as packet
import trace_to_ir

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ir"))
from scene_ir import SceneSnapshot  # noqa: E402


def section_matches_draw(offline, runtime, draw):
    """Compare draw arguments to the selected section, without claiming payload bytes."""
    return (draw.get("call") == "DrawIndexedPrimitive" and draw.get("primitive_type") == 4 and
            draw.get("min_vertex_index") == runtime.get("first_vertex") and
            draw.get("num_vertices") == runtime.get("vertex_count") and
            draw.get("start_index") == runtime.get("first_index") and
            draw.get("primitive_count") == runtime.get("triangles") and
            offline.first_index == runtime.get("first_index") and
            offline.index_count == runtime.get("index_count") and
            runtime.get("index_count") == runtime.get("triangles", -1) * 3)


def verify(scene, records, frame, render_pass, trace_sha256, scene_sha256):
    if scene.source.kind != "offline":
        raise ValueError("reference scene must be offline")
    header = bridge.one(records, "header")
    if not isinstance(header, dict):
        raise ValueError("missing trace header")
    draws = [x for x in records if x.get("event") == "draw"]
    results = {x["op_id"]: x for x in records if x.get("event") == "draw_result"}
    sizes = [trace_to_ir.buffered_frame_bytes(x) for x in draws]
    limit = header.get("buffer_capture", {}).get("frame_limit_bytes")
    budget = type(limit) is int and all(type(x) is int for x in sizes) and sum(sizes) <= limit
    instances = [x for x in scene.mesh_instances if x.id.endswith(":" + bridge.TARGET_ACTOR)]
    if len(instances) != 1:
        raise ValueError("offline target actor missing or ambiguous")
    mesh = next((x for x in scene.static_mesh_resources if x.id == instances[0].resource_id), None)
    if mesh is None or len([x for x in scene.static_mesh_resources if x.name.casefold() == mesh.name.casefold()]) != 1:
        raise ValueError("offline target mesh missing or ambiguous")

    verified = []
    comparisons = {}
    for draw in draws:
        op = draw.get("op_id")
        if op not in results or not draw.get("submission_ticket"):
            continue
        proof = bridge.submission_for_draw(records, op, render_pass)
        if proof.get("status") != "matched":
            continue
        ticket = proof["observation_ticket"]
        observations = [x for x in records if x.get("event") == "scene_bridge_observation" and x.get("ticket") == ticket]
        if len(observations) != 1:
            continue
        observation = observations[0]
        # Compare metadata using exactly the observation independently selected
        # by the balanced submission proof. No temporal ownership inference.
        selected = [x for x in records if x.get("event") != "scene_bridge_observation" or x is observation]
        report = comparison.compare(scene, selected, frame, op, render_pass,
                                    bsp_indices=[x.source_index for x in scene.bsp_blocks])
        if (report["complete_observation_window"] != "synchronized" or report["map_name_match"] != "match"):
            continue
        section = proof["section"]
        camera = observation["current_camera"]
        if proof["kind"] == "static_mesh":
            check = report["mesh"]
            if (report["actor"]["transform"]["status"] != "match" or
                    check["asset_property_audit"] != "consistent" or check["node_name_matches_asset"] is not True or
                    check["material_sections_match"] != "match" or
                    any(check[k]["status"] != "match" for k in ("vertex_count", "index_count", "section_count", "bounds"))):
                continue
            runtime_sections = check["runtime_material_sections"]
            if section >= len(mesh.geometry.sections) or section >= len(runtime_sections):
                continue
            if not section_matches_draw(mesh.geometry.sections[section], runtime_sections[section], draw):
                continue
            shadow = builder.verified_section(mesh.geometry, mesh.geometry.sections[section], runtime_sections[section], draw,
                                              results[op], header, budget, report["actor"]["transform"]["runtime"], camera,
                                              draw["viewport"], draw["thread_id"])
            verified.append({"kind": "static_mesh", "draw": op, "section": section, "stage": proof["stage"],
                             "same_frame_lock_shadow": "matched" if shadow else "unverified"})
            comparisons["mesh"] = {"vertex_count": check["vertex_count"], "index_count": check["index_count"],
                                   "section_count": check["section_count"], "bounds": check["bounds"],
                                   "material_sections_match": check["material_sections_match"],
                                   "transform": report["actor"]["transform"], "camera": report["camera"]}
        else:
            matching = [x for x in report["offline_bsp_blocks"] if x["runtime"] == "unique_metadata_candidate" and
                        x["runtime_candidates"][0]["runtime_source_index"] == proof["runtime_index"]]
            if len(matching) != 1:
                continue
            block_report = matching[0]
            block = next(x for x in scene.bsp_blocks if x.source_index == block_report["source_index"])
            candidates = [x for x in observation["bsp"]["candidates"] if x["source_index"] == proof["runtime_index"]]
            if len(candidates) != 1 or section >= len(block.geometry.sections):
                continue
            candidate = candidates[0]
            runtime_sections = candidate["sections"]
            if section >= len(runtime_sections):
                continue
            world = bridge.decode_floats(candidate["transform_bits"], 16)
            if world is None or not section_matches_draw(block.geometry.sections[section], runtime_sections[section], draw):
                continue
            shadow = builder.verified_section(block.geometry, block.geometry.sections[section], runtime_sections[section],
                                              draw, results[op], header, budget, world, camera,
                                              draw["viewport"], draw["thread_id"])
            verified.append({"kind": "bsp", "draw": op, "section": section, "stage": proof["stage"],
                             "runtime_index": proof["runtime_index"], "offline_index": block.source_index,
                             "same_frame_lock_shadow": "matched" if shadow else "unverified"})
            comparisons[f"bsp:{block.source_index}"] = block_report
    mesh_sections = {x["section"] for x in verified if x["kind"] == "static_mesh"}
    mesh_stages = {}
    bsp_sections = {}
    for item in verified:
        if item["kind"] == "static_mesh":
            mesh_stages.setdefault(item["stage"], set()).add(item["section"])
        else:
            bsp_sections.setdefault((item["stage"], item["runtime_index"], item["offline_index"]), set()).add(item["section"])
    stages = sorted(stage for stage, sections in mesh_stages.items() if {1, 2}.issubset(sections) and
                    any(key[0] == stage and len(value) >= 2 for key, value in bsp_sections.items()))
    complete = bool(stages) and bool(scene.source.fingerprints.get("map"))
    return {"status": "validated" if complete else "partial", "frame": frame, "pass": render_pass,
            "trace_sha256": trace_sha256, "offline_scene_sha256": scene_sha256,
            "resource_key": {"type": "static_mesh", "name": mesh.name,
                             "map": scene.source.map, "map_sha256": scene.source.fingerprints.get("map"),
                             "bundle_sha256": scene.source.fingerprints.get("bundle"),
                             "fingerprint_source": "offline_fixture; runtime map name and unique asset matched"}
                            if mesh_sections and scene.source.fingerprints.get("map") else None,
            "validated_stages": stages, "scoped_verified_draws": verified, "comparisons": comparisons,
            "validation_scope": "direct_section_submission_and_offline_metadata",
            "limitations": ["Resource fingerprints are supplied by the offline fixture, not observed from the runtime manager.",
                            "Repeated runtime reads cannot exclude ABA or unobserved writes.",
                            "Matching draw arguments do not prove GPU payload bytes; lock shadows are not GPU readbacks.",
                            "The BSP scanner is heuristic and its vertex ranges are checked by index containment."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--scene", type=Path, required=True)
    parser.add_argument("--frame", type=int, required=True)
    parser.add_argument("--pass", dest="render_pass", type=int, required=True)
    parser.add_argument("--reference-trace", type=Path, help="optional separate frame with captured lock shadows")
    parser.add_argument("--reference-frame", type=int)
    parser.add_argument("--reference-draw", type=int)
    parser.add_argument("--reference-pass", type=int, default=0)
    parser.add_argument("--reference-bsp", type=int, help="offline BSP index to compare in both captures")
    args = parser.parse_args(argv)
    try:
        if not bridge.uint(args.frame, 64) or not bridge.uint(args.render_pass):
            raise ValueError("frame and pass must be unsigned")
        reference_values = (args.reference_trace, args.reference_frame, args.reference_draw)
        if any(x is not None for x in reference_values) and not all(x is not None for x in reference_values):
            raise ValueError("reference trace, frame and draw must be supplied together")
        if args.reference_trace is not None and (not bridge.uint(args.reference_frame, 64) or
                not bridge.uint(args.reference_draw, 64, True) or not bridge.uint(args.reference_pass) or
                (args.reference_bsp is not None and not bridge.uint(args.reference_bsp))):
            raise ValueError("reference frame, draw and pass must be unsigned")
        if args.reference_bsp is not None and args.reference_trace is None:
            raise ValueError("reference BSP requires a reference trace")
        before = args.trace.stat()
        bridge.trace_summary.summarize(args.trace)
        digest = hashlib.sha256()
        with args.trace.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
            finished = os.fstat(stream.fileno())
        trace_sha256 = digest.hexdigest()
        records = builder.read_frame(args.trace, args.frame, trace_sha256)
        after = args.trace.stat()
        identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns)
        if len({identity(s) for s in (before, opened, finished, after)}) != 1:
            raise ValueError("trace changed while validating")
        raw = packet.read_limited(args.scene, packet.MAX_INPUT)
        scene = SceneSnapshot.from_json(raw)
        scene_digest = hashlib.sha256(raw).hexdigest()
        result = verify(scene, records, args.frame, args.render_pass, trace_sha256, scene_digest)
        if args.reference_trace is not None:
            if result["status"] != "validated":
                raise ValueError("current frame lacks a complete scoped mesh and BSP proof")
            bsp_groups = {}
            for entry in result["scoped_verified_draws"]:
                if entry["kind"] == "bsp" and entry["stage"] in result["validated_stages"]:
                    bsp_groups.setdefault(entry["offline_index"], set()).add(entry["section"])
            candidates = sorted(index for index, sections in bsp_groups.items() if len(sections) >= 2)
            if not candidates:
                raise ValueError("current frame lacks a two-section BSP")
            index = args.reference_bsp if args.reference_bsp is not None else candidates[0]
            if index not in candidates:
                raise ValueError("reference BSP has no two-section scoped proof in the current frame")
            selected, reference_digest = bridge.read_selected(args.reference_trace, args.reference_frame, args.reference_draw)
            reference_records = builder.read_frame(args.reference_trace, args.reference_frame, reference_digest)
            _, manifest = builder.build(scene, scene_digest, selected, reference_records, reference_digest,
                                        args.reference_frame, args.reference_draw, args.reference_pass, [index], [1, 2])
            result["separate_lock_shadow_reference"] = {
                "frame": args.reference_frame, "trace_sha256": reference_digest, "same_frame": False,
                "mesh_sections": manifest["mesh_sections"], "bsp": manifest["bsp"],
                "direct_draw_ownership": manifest["direct_draw_ownership"],
                "scope": manifest["shadow_scope"]}
        print(json.dumps(result, indent=2, allow_nan=False))
        return 0 if result["status"] == "validated" else 2
    except (OSError, ValueError, TypeError, KeyError, IndexError, bridge.trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
