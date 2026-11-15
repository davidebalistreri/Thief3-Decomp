#!/usr/bin/env python3
"""Build a partial SceneIR v1 from verified scoped submissions and offline assets.

This is an offline capture processor, not a live bridge or GPU readback. Only
sections whose direct submission was verified in the selected Present interval
and which share the anchor's camera and viewport are included. Mesh transforms
must match the anchor; BSP geometry must be in identity-transformed world space.
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
import verify_bridge_submission as verifier

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ir"))
from scene_ir import SceneSnapshot  # noqa: E402


CANDIDATE_LIMITATION = ("Additional mesh actors have scoped runtime ownership and transforms, but their mesh, "
                        "geometry and materials are offline candidates, not observed runtime bindings.")
TRANSLATION_TOLERANCE_T3 = 0.001
MATRIX_TOLERANCE = 0.000001


def candidate_transform_matches(offline, runtime):
    """Allow small T3 float/trigonometric differences; retain the runtime matrix."""
    if runtime is None or len(offline) != 16 or len(runtime) != 16:
        return False
    return all(abs(a - b) <= (TRANSLATION_TOLERANCE_T3 if index in (12, 13, 14) else MATRIX_TOLERANCE)
               for index, (a, b) in enumerate(zip(offline, runtime)))


def candidate_mesh_draws(scene, records, render_pass, stage):
    """Match submitted actors to offline assets without claiming their live mesh binding."""
    instances = {}
    for instance in scene.mesh_instances:
        instances.setdefault(instance.id.rsplit(":", 1)[-1], []).append(instance)
    meshes = {mesh.id: mesh for mesh in scene.static_mesh_resources}
    result = []
    for draw in (r for r in records if r.get("event") == "draw"):
        proof = bridge.submission_for_draw(records, draw.get("op_id"), render_pass, allow_asset_candidates=True)
        if proof.get("kind") != "static_mesh_candidate" or proof.get("stage") != stage:
            continue
        matches = instances.get(proof["actor"], [])
        if len(matches) != 1 or matches[0].resource_id not in meshes:
            continue
        instance = matches[0]
        observations = [r for r in records if r.get("event") == "scene_bridge_observation" and
                        r.get("ticket") == proof["observation_ticket"]]
        if len(observations) != 1:
            continue
        observation = observations[0]
        entries = [e for e in observation.get("entries", []) if isinstance(e, dict) and
                   bridge.fullname(e.get("owner_name"), e.get("owner_fname")) == proof["actor"]]
        if len(entries) != 1:
            continue
        transform = bridge.decode_floats(entries[0].get("object_matrix_bits"), 16)
        if not candidate_transform_matches(instance.transform, transform):
            continue
        geometry = meshes[instance.resource_id].geometry
        index = proof["section"]
        if index >= len(geometry.sections):
            continue
        section = geometry.sections[index]
        first, count = draw.get("min_vertex_index"), draw.get("num_vertices")
        indices = geometry.indices[section.first_index:section.first_index + section.index_count]
        if (draw.get("call") != "DrawIndexedPrimitive" or type(draw.get("primitive_type")) is not int or
                draw.get("primitive_type") != 4 or not bridge.uint(draw.get("start_index")) or
                not bridge.uint(draw.get("primitive_count"), nonzero=True) or
                not bridge.uint(first) or not bridge.uint(count, nonzero=True) or not indices or
                first + count > len(geometry.positions) or
                draw.get("start_index") != section.first_index or
                draw.get("primitive_count") != section.index_count // 3 or
                any(i < first or i >= first + count for i in indices)):
            continue
        result.append({"kind": "static_mesh_candidate", "draw": draw["op_id"], "section": index,
                       "stage": stage, "target_id": instance.id, "resource_id": instance.resource_id,
                       "same_frame_lock_shadow": "unverified"})
    return result


def build(scene, records, frame, render_pass, stage, anchor_draw, trace_digest, scene_digest,
          *, include_asset_candidates=False):
    report = verifier.verify(scene, records, frame, render_pass, trace_digest, scene_digest)
    if report["status"] != "validated" or stage not in report["validated_stages"]:
        raise ValueError("selected frame, pass and stage lack validated mesh and BSP submissions")
    verified = [item for item in report["scoped_verified_draws"] if item["stage"] == stage]
    anchors = [item for item in verified if item["kind"] == "static_mesh" and item["draw"] == anchor_draw]
    if len(anchors) != 1:
        raise ValueError("anchor must be a verified mesh submission in the selected stage")
    if include_asset_candidates:
        verified.extend(candidate_mesh_draws(scene, records, render_pass, stage))
        verified.sort(key=lambda item: item["draw"])

    def observation(draw_id):
        proof = bridge.submission_for_draw(records, draw_id, render_pass,
                                           allow_asset_candidates=include_asset_candidates)
        if proof.get("status") != "matched" or proof.get("stage") != stage:
            raise ValueError("submission proof changed")
        found = [item for item in records if item.get("event") == "scene_bridge_observation" and
                 item.get("ticket") == proof["observation_ticket"]]
        if len(found) != 1:
            raise ValueError("submission observation is missing or ambiguous")
        return found[0]

    anchor = bridge.one(records, "draw", anchor_draw)
    observed = observation(anchor_draw)
    camera = observed["current_camera"]
    view_bits, projection_bits = camera["derived_matrix_bits"], camera["projection_bits"]
    view = bridge.decode_floats(view_bits, 16)
    projection = bridge.decode_floats(projection_bits, 16)
    matrix_bits = observed["target"]["entry"]["object_matrix_bits"]
    transform = bridge.decode_floats(matrix_bits, 16)
    viewport = anchor.get("viewport") if anchor else None
    if view is None or projection is None or transform is None or not isinstance(viewport, dict):
        raise ValueError("anchor camera or transform is incomplete")
    instance = next(x for x in scene.mesh_instances if x.id.endswith(":" + bridge.TARGET_ACTOR))
    blocks = {block.source_index: block for block in scene.bsp_blocks}
    submissions = []
    instance_transforms = {instance.id: transform}
    for item in verified:
        draw = bridge.one(records, "draw", item["draw"])
        probe = observation(item["draw"])
        current = probe["current_camera"]
        if (current.get("derived_matrix_bits") != view_bits or current.get("projection_bits") != projection_bits or
                draw.get("viewport") != viewport or draw.get("thread_id") != anchor.get("thread_id")):
            continue
        if item["kind"] == "static_mesh":
            if probe["target"]["entry"].get("object_matrix_bits") != matrix_bits:
                continue
            kind, target_id = "mesh_instance", instance.id
        elif item["kind"] == "static_mesh_candidate":
            entries = [e for e in probe["entries"] if isinstance(e, dict) and
                       bridge.fullname(e.get("owner_name"), e.get("owner_fname")) == item["target_id"].rsplit(":", 1)[-1]]
            if len(entries) != 1:
                continue
            candidate_transform = bridge.decode_floats(entries[0].get("object_matrix_bits"), 16)
            if candidate_transform is None:
                continue
            kind, target_id = "mesh_instance", item["target_id"]
            if target_id in instance_transforms and instance_transforms[target_id] != candidate_transform:
                raise ValueError("candidate actor transform changed between submissions")
            instance_transforms[target_id] = candidate_transform
        else:
            candidates = [x for x in probe["bsp"]["candidates"] if x["source_index"] == item["runtime_index"]]
            if (len(candidates) != 1 or comparison.compare_f32(
                    builder.IDENTITY, bridge.decode_floats(candidates[0]["transform_bits"], 16)) != "match"):
                continue  # v1 BSP geometry is in world space, without a transform field.
            kind, target_id = "bsp_block", blocks[item["offline_index"]].id
        submissions.append({"target_kind": kind, "target_id": target_id, "section": item["section"],
                            "render_pass": render_pass, "stage": stage, "draw_id": item["draw"],
                            "lock_shadow": item["same_frame_lock_shadow"]})
    if not any(s["draw_id"] == anchor_draw for s in submissions) or not any(s["target_kind"] == "bsp_block" for s in submissions):
        raise ValueError("no same-camera mesh and BSP submissions remain")

    data = scene.to_dict()
    data["version"] = 1
    data["submissions"] = submissions
    data["source"] = {"kind": "runtime", "map": scene.source.map, "frame_id": frame,
                      "camera_synchronized": True,
                      "fingerprints": {**scene.source.fingerprints, "trace": trace_digest, "offline_scene": scene_digest},
                      "limitations": [
                          "Partial selection of verified same-camera submissions, not the complete visible scene.",
                          "Geometry and qualified IDs come from the offline fixture after runtime metadata matching; runtime resource fingerprints are unknown.",
                          "Only listed sections were directly submitted in this pass and stage; unlisted sections have no visibility claim.",
                          "Lock-shadow status is per draw; matching shadows are not GPU readbacks.",
                          "BSP IDs and source indices come from the heuristic offline scanner; metadata matching does not prove payload identity.",
                          "Viewport is from the matching draws; probe viewport depth can differ. Near/far, skin and runtime lights are unknown.",
                          "Repeated runtime reads cannot exclude ABA or unobserved writes."]}
    if include_asset_candidates:
        data["source"]["limitations"].append(CANDIDATE_LIMITATION)
        data["source"]["limitations"].append(
            "Offline actor transforms matched within 0.001 T3 units of translation and 0.000001 in other matrix entries.")
    data["camera"] = {"source": {"kind": "runtime", "frame_id": frame, "fingerprint": trace_digest},
                      "view": view, "projection": projection,
                      "viewport": {"x": viewport["x"], "y": viewport["y"], "width": viewport["width"],
                                   "height": viewport["height"], "min_depth": viewport["min_z"], "max_depth": viewport["max_z"]},
                      "near": None, "far": None}
    selected_instances = {s["target_id"] for s in submissions if s["target_kind"] == "mesh_instance"}
    candidate_ids = sorted({item["target_id"] for item in verified if item["kind"] == "static_mesh_candidate" and
                            item["target_id"] in selected_instances})
    if include_asset_candidates:
        data["source"]["limitations"].append("Offline asset candidate actors: " + (", ".join(candidate_ids) or "none"))
    data["mesh_instances"] = [x for x in data["mesh_instances"] if x["id"] in selected_instances]
    selected_resources = {x["resource_id"] for x in data["mesh_instances"]}
    data["static_mesh_resources"] = [x for x in data["static_mesh_resources"] if x["id"] in selected_resources]
    for item in data["mesh_instances"]:
        item.update(transform=instance_transforms[item["id"]], skin=None)
    selected_blocks = {s["target_id"] for s in submissions if s["target_kind"] == "bsp_block"}
    data["bsp_blocks"] = [x for x in data["bsp_blocks"] if x["id"] in selected_blocks]
    used = {s["material_id"] for block in data["bsp_blocks"] for s in block["geometry"]["sections"]}
    used.update(s["material_id"] for mesh in data["static_mesh_resources"] for s in mesh["geometry"]["sections"])
    for item in data["mesh_instances"]:
        used.update(item["material_ids"])
    data["materials"] = [x for x in data["materials"] if x["id"] in used]
    data["lights"] = []
    snapshot = SceneSnapshot.from_dict(data)
    manifest = {"schema": "t3.scene.scoped-evidence", "version": 1, "frame": frame, "pass": render_pass,
                "stage": stage, "anchor_draw": anchor_draw, "trace_sha256": trace_digest,
                "offline_scene_sha256": scene_digest, "resource_key": report["resource_key"],
                "submitted_sections": len(submissions), "bsp_blocks": len(selected_blocks),
                "limitations": data["source"]["limitations"]}
    if include_asset_candidates:
        manifest["offline_asset_candidates"] = candidate_ids
        manifest["candidate_transform_tolerances"] = {"translation_t3_units": TRANSLATION_TOLERANCE_T3,
                                                       "other_matrix_entries": MATRIX_TOLERANCE}
    return snapshot, manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--scene", required=True, type=Path)
    parser.add_argument("--frame", required=True, type=int)
    parser.add_argument("--pass", dest="render_pass", required=True, type=int)
    parser.add_argument("--stage", required=True, type=int)
    parser.add_argument("--anchor-draw", required=True, type=int)
    parser.add_argument("--include-asset-candidates", action="store_true",
                        help="include scoped actors with matching offline mesh candidates")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if (not bridge.uint(args.frame, 64) or not bridge.uint(args.render_pass) or not bridge.uint(args.stage) or
                not bridge.uint(args.anchor_draw, 64, True)):
            raise ValueError("invalid frame, pass, stage or draw")
        before = args.trace.stat()
        bridge.trace_summary.summarize(args.trace)
        digest = hashlib.sha256()
        with args.trace.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
            finished = os.fstat(stream.fileno())
        trace_digest = digest.hexdigest()
        records = builder.read_frame(args.trace, args.frame, trace_digest)
        after = args.trace.stat()
        identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns)
        if len({identity(s) for s in (before, opened, finished, after)}) != 1:
            raise ValueError("trace changed while validating")
        raw = packet.read_limited(args.scene, packet.MAX_INPUT)
        scene = SceneSnapshot.from_json(raw)
        snapshot, manifest = build(scene, records, args.frame, args.render_pass, args.stage,
                                   args.anchor_draw, trace_digest, hashlib.sha256(raw).hexdigest(),
                                   include_asset_candidates=args.include_asset_candidates)
        body = snapshot.to_json(indent=2) + "\n"
        manifest["snapshot_sha256"] = hashlib.sha256(body.encode()).hexdigest()
        packet.write_pair(args.output, body, manifest)
        print(json.dumps(manifest, indent=2, allow_nan=False))
    except (OSError, ValueError, TypeError, KeyError, IndexError, StopIteration, bridge.trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
