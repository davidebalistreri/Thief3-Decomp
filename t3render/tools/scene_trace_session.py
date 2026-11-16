"""Recheck two SceneIR frames against their finalized scoped capture.

This establishes capture continuity and direct submission membership. It does
not revalidate offline geometry, asset fingerprints or GPU payloads.
"""
import hashlib
import json
import os
from pathlib import Path
import re

import analyze_scene_bridge as bridge
import build_bridge_runtime_snapshot as builder
import compare_bridge_fixture as comparison
import verify_bridge_submission as verifier


def _check_scene(scene, records, render_pass, stage):
    camera = scene.camera
    if scene.source.kind != "runtime" or not scene.source.camera_synchronized or camera is None or not scene.submissions:
        raise ValueError("runtime scene requires a synchronized camera and scoped submissions")
    instances = {item.id: item for item in scene.mesh_instances}
    meshes = {item.id: item for item in scene.static_mesh_resources}
    blocks = {item.id: item for item in scene.bsp_blocks}
    materials = {item.id: item.name for item in scene.materials}
    viewport = camera.viewport
    expected_viewport = {"x": viewport.x, "y": viewport.y, "width": viewport.width, "height": viewport.height,
                         "min_z": viewport.min_depth, "max_z": viewport.max_depth}
    for submission in scene.submissions:
        if (submission.render_pass, submission.stage) != (render_pass, stage):
            raise ValueError("runtime scene has an unexpected submission scope")
        draw = bridge.one(records, "draw", submission.draw_id)
        proof = bridge.submission_for_draw(records, submission.draw_id, render_pass, allow_asset_candidates=True)
        if (draw is None or proof.get("status") != "matched" or proof.get("stage") != stage or
                proof.get("section") != submission.section or draw.get("viewport") != expected_viewport):
            raise ValueError("runtime scene submission does not match the trace")
        observations = [row for row in records if row.get("event") == "scene_bridge_observation" and
                        row.get("ticket") == proof.get("observation_ticket")]
        if len(observations) != 1:
            raise ValueError("runtime scene observation is ambiguous")
        observed = observations[0]
        target = observed["target"]
        outer = [item for item in target["outer"] if item.get("member") is True and item.get("stable") is True
                 and isinstance(item.get("name"), str)]
        if len(outer) != 1 or target.get("outer_truncated"):
            raise ValueError("runtime scene map is not established in this observation")
        name = outer[0]["name"]
        name = name.split(":", 1)[1] if name.lower().startswith("loadgame:") else name
        if name.lower() != scene.source.map.lower():
            raise ValueError("runtime scene map does not match the trace")
        current = observed["current_camera"]
        if (bridge.compare_matrix(current.get("derived_matrix_bits"), list(camera.view)) != "equal_bits" or
                bridge.compare_matrix(current.get("projection_bits"), list(camera.projection)) != "equal_bits"):
            raise ValueError("runtime scene camera does not match the trace")
        if submission.target_kind == "mesh_instance":
            instance = instances[submission.target_id]
            actor = instance.id.rsplit(":", 1)[-1]
            if proof.get("kind") not in ("static_mesh", "static_mesh_candidate") or proof.get("actor") != actor:
                raise ValueError("runtime scene actor does not match the trace")
            if proof["kind"] == "static_mesh":
                entries = [observed["target"]["entry"]]
                resource = meshes[instance.resource_id]
                node = observed["target"]["mesh_node"]
                geometry = resource.geometry
                counts = node["counts"]
                ids = instance.material_ids or tuple(section.material_id for section in geometry.sections)
                sections = [{"name": section.name, "first_index": section.first_index, "index_count": section.index_count,
                             "material": materials.get(material_id) if material_id is not None else None}
                            for section, material_id in zip(geometry.sections, ids)]
                if (resource.name.casefold() != proof["resource_name"].casefold() or
                        counts["vertices"] != len(geometry.positions) or counts["indices"] != len(geometry.indices) or
                        counts["sections"] != len(geometry.sections) or
                        comparison.compare_f32(comparison.vertex_bounds(geometry.positions),
                                               bridge.decode_floats(node["bounds_bits"], 6)) != "match" or
                        comparison.compare_sections(sections, node["sections"]) != "match" or
                        not verifier.section_matches_draw(geometry.sections[submission.section],
                                                          node["sections"][submission.section], draw)):
                    raise ValueError("runtime scene mesh metadata does not match the trace")
            else:
                entries = [entry for entry in observed["entries"] if entry.get("type") == 4 and
                           bridge.fullname(entry.get("owner_name"), entry.get("owner_fname")) == actor]
                geometry = meshes[instance.resource_id].geometry
                section = geometry.sections[submission.section]
                first, count = draw.get("min_vertex_index"), draw.get("num_vertices")
                indices = geometry.indices[section.first_index:section.first_index + section.index_count]
                if (draw.get("call") != "DrawIndexedPrimitive" or draw.get("primitive_type") != 4 or
                        not bridge.uint(first) or not bridge.uint(count, nonzero=True) or
                        first + count > len(geometry.positions) or not indices or
                        draw.get("start_index") != section.first_index or
                        draw.get("primitive_count") != section.index_count // 3 or
                        any(index < first or index >= first + count for index in indices)):
                    raise ValueError("runtime scene candidate geometry does not match the draw arguments")
            if (len(entries) != 1 or
                    bridge.compare_matrix(entries[0].get("object_matrix_bits"), list(instance.transform)) != "equal_bits"):
                raise ValueError("runtime scene transform does not match the trace")
        else:
            if proof.get("kind") != "bsp":
                raise ValueError("runtime scene BSP submission does not match the trace")
            block = blocks[submission.target_id]
            candidates = [item for item in observed["bsp"]["candidates"] if item["source_index"] == proof["runtime_index"]]
            if len(candidates) != 1:
                raise ValueError("runtime scene BSP candidate is ambiguous")
            candidate = candidates[0]
            metadata = dict(candidate, bounds_t3_units=bridge.decode_floats(candidate["bounds_bits"], 6))
            if (any(value != "match" for value in comparison.bsp_metadata_comparisons(block, materials, metadata).values()) or
                    comparison.compare_f32(builder.IDENTITY, bridge.decode_floats(candidate["transform_bits"], 16)) != "match" or
                    not verifier.section_matches_draw(block.geometry.sections[submission.section],
                                                      candidate["sections"][submission.section], draw)):
                raise ValueError("runtime scene BSP metadata does not match the trace")


def validate(path, previous, current, render_pass, stage):
    """Require adjacent synchronized Present windows on one device and trace.

    Returns only semantic provenance; process addresses are never returned.
    The trace must be finalized and immutable while read.
    """
    path = Path(path)
    old, new = previous.source.frame_id, current.source.frame_id
    if type(old) is not int or type(new) is not int or new != old + 1:
        raise ValueError("runtime history requires consecutive frames")
    expected = previous.source.fingerprints.get("trace")
    if not expected or current.source.fingerprints.get("trace") != expected:
        raise ValueError("runtime history requires one trace fingerprint")

    before = path.stat()
    summary = bridge.trace_summary.summarize(path)
    digest = hashlib.sha256()
    selected = {old: [], new: []}
    header = None
    reset = False
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        for line in stream:
            digest.update(line)
            row = json.loads(line, object_pairs_hook=bridge.trace_summary._object,
                             parse_constant=bridge.trace_summary._constant)
            if row.get("event") == "header":
                header = row
            if row.get("event") == "reset":
                reset = True
            if row.get("frame") in selected:
                selected[row["frame"]].append(row)
        finished = os.fstat(stream.fileno())
    after = path.stat()
    identity = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns)
    if len({identity(value) for value in (before, opened, finished, after)}) != 1 or digest.hexdigest() != expected:
        raise ValueError("trace changed or does not match runtime fingerprint")
    if header is None or header.get("requested_frames") not in (2, 3) or summary["frame_count"] != header["requested_frames"]:
        raise ValueError("runtime history requires one finalized multi-frame capture")
    if reset:
        raise ValueError("runtime history cannot cross a capture containing Reset")
    if [row["frame"] for row in summary["frames"]].count(old) != 1 or [row["frame"] for row in summary["frames"]].count(new) != 1:
        raise ValueError("runtime history frames are absent or ambiguous in the trace")
    frames = []
    for frame_id, scene in ((old, previous), (new, current)):
        records = [header] + selected[frame_id]
        begin = bridge.one(records, "frame_begin")
        frame = bridge.one(records, "scene_bridge_frame")
        end = bridge.one(records, "frame_end")
        device = begin.get("device") if isinstance(begin, dict) else None
        if (not isinstance(device, str) or not re.fullmatch(r"0x[0-9a-fA-F]{1,8}", device) or
                int(device, 16) == 0 or not isinstance(frame, dict) or not isinstance(end, dict) or
                frame.get("status") != "synchronized" or end.get("had_reset") is not False):
            raise ValueError("runtime frame lacks synchronized device evidence")
        _check_scene(scene, records, render_pass, stage)
        frames.append((int(device, 16), frame["present_thread"], frame["previous_present_thread"]))
    if frames[0][0] != frames[1][0] or frames[0][1] != frames[1][2]:
        raise ValueError("runtime history device or Present thread changed")
    return {"trace_sha256": expected, "previous_frame": old, "current_frame": new}
