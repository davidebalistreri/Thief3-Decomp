#!/usr/bin/env python3
"""Conservatively compare one Flesh observation with one diagnostic draw.

The operation interval is a temporal candidate, not proof that the element
submitted the selected draw. No addresses, buffer data or matrices are emitted.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys

import trace_summary
from trace_to_ir import hresult_succeeded


TARGET_ACTOR = "StaticMeshActor__1520"
TARGET_ASSET = "INNbed1A1"
EXPECTED_CALLER = 0x109271AE
EXPECTED_ELEMENT_VTABLE = 0x10E4E8C0


def uint(value, bits=32, nonzero=False):
    return type(value) is int and int(nonzero) <= value < (1 << bits)


def f32_bits(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    try:
        if not math.isfinite(value):
            return None
        encoded = struct.pack("<f", value)
        if not math.isfinite(struct.unpack("<f", encoded)[0]):
            return None
        return struct.unpack("<I", encoded)[0]
    except (OverflowError, struct.error, TypeError):
        return None


def compare_matrix(raw_bits, values):
    if (not isinstance(raw_bits, list) or len(raw_bits) != 16 or
            not all(uint(value) for value in raw_bits) or
            not isinstance(values, list) or len(values) != 16):
        return "unknown"
    # Reject NaNs and infinities on both sides; no malformed state can match.
    if any(not math.isfinite(struct.unpack("<f", struct.pack("<I", value))[0]) for value in raw_bits):
        return "unknown"
    actual = [f32_bits(value) for value in values]
    if any(value is None for value in actual):
        return "unknown"
    return "equal_bits" if raw_bits == actual else "different"


def compare_viewport(raw_bits, viewport):
    if (not isinstance(raw_bits, list) or len(raw_bits) != 6 or
            not all(uint(value) for value in raw_bits) or not isinstance(viewport, dict)):
        return "unknown"
    xywh = [viewport.get(name) for name in ("x", "y", "width", "height")]
    depth = [f32_bits(viewport.get(name)) for name in ("min_z", "max_z")]
    if not all(uint(value) for value in xywh) or any(value is None for value in depth):
        return "unknown"
    if any(not math.isfinite(struct.unpack("<f", struct.pack("<I", value))[0]) for value in raw_bits[4:]):
        return "unknown"
    return "equal_bits" if raw_bits == xywh + depth else "different"


def decode_floats(raw_bits, count):
    if not isinstance(raw_bits, list) or len(raw_bits) != count or not all(uint(v) for v in raw_bits):
        return None
    values = [struct.unpack("<f", struct.pack("<I", value))[0] for value in raw_bits]
    return values if all(math.isfinite(value) for value in values) else None


def decode_rotator(raw_bits):
    if not isinstance(raw_bits, list) or len(raw_bits) != 3 or not all(uint(v) for v in raw_bits):
        return None
    # AActor::Rotation is three signed INTs in Pitch, Yaw, Roll order.
    return [value if value < (1 << 31) else value - (1 << 32) for value in raw_bits]


def fullname(name, fname):
    if not isinstance(name, str) or not name or not uint(fname):
        return None
    number = fname >> 16
    return f"{name}__{number - 1}" if number else name


def owner_link(entry, table_complete):
    if not isinstance(entry, dict) or table_complete is not True:
        return False
    return (entry.get("vtable") == EXPECTED_ELEMENT_VTABLE and entry.get("type") == 4 and
            entry.get("owner_table_member") is True and entry.get("owner_stable") is True and
            uint(entry.get("address"), nonzero=True) and uint(entry.get("owner_candidate"), nonzero=True) and
            entry.get("owner_backlink") == entry.get("address") and
            uint(entry.get("owner_object_index")) and uint(entry.get("owner_class"), nonzero=True) and
            fullname(entry.get("owner_name"), entry.get("owner_fname")) == TARGET_ACTOR)


def synchronized(frame_result, frame_end, draw_result):
    if not isinstance(frame_result, dict) or not isinstance(frame_end, dict) or not isinstance(draw_result, dict):
        return False
    flags_false = ("epoch_unknown", "probe_overlap", "present_overlap", "operation_overlap",
                   "reset_seen", "reset_pending")
    threads = ("previous_present_thread", "present_thread", "draw_thread", "probe_thread")
    thread = frame_result.get("present_thread")
    return (frame_result.get("status") == "synchronized" and
            frame_result.get("probe_reentered", False) is False and
            frame_result.get("previous_present_observed") is True and
            frame_result.get("same_thread") is True and frame_result.get("present_tracked") is True and
            all(frame_result.get(k) is False for k in flags_false) and uint(thread, nonzero=True) and
            all(frame_result.get(k) == thread for k in threads) and
            uint(frame_result.get("completed"), 64, nonzero=True) and
            frame_result.get("started") == frame_result.get("completed") and
            frame_result.get("pending") == 0 and frame_result.get("dropped") == 0 and
            frame_end.get("had_reset") is False and frame_end.get("concurrent_operations") is False and
            hresult_succeeded(frame_end.get("present_hr")) is True and
            hresult_succeeded(draw_result.get("hr")) is True)


def snapshot_window_synchronized(frame_result, frame_end, draw_result):
    """Require an explicitly observed absence of reentry for a new snapshot.

    Historical traces can still establish an observed Present interval, but
    their missing reentry marker cannot establish a complete probe window.
    """
    return (synchronized(frame_result, frame_end, draw_result) and
            frame_result.get("probe_reentered") is False)


def pick_observation(records, draw_id):
    """Last observed probe before a draw; this is not a call-stack proof."""
    last = None
    for item in records:
        if item.get("event") == "scene_bridge_observation":
            last = item
        elif item.get("event") == "draw" and item.get("op_id") == draw_id:
            if last is not None and uint(last.get("next_d3d_operation"), 64, nonzero=True) and \
                    last["next_d3d_operation"] <= draw_id:
                return last
            return None
    return None


def one(records, event, op_id=None):
    values = [item for item in records if item.get("event") == event and
              (op_id is None or item.get("op_id") == op_id)]
    return values[0] if len(values) == 1 else None


def submission_for_draw(records, draw_id, expected_pass):
    """Match a balanced same-thread section scope to a unique semantic entry.

    The caller must first validate the complete trace with trace_summary.
    This never infers ownership from temporal adjacency alone.
    """
    unknown = {"status": "unknown"}
    header = one(records, "header")
    capability = header.get("scene_bridge") if isinstance(header, dict) else None
    if not isinstance(capability, dict) or capability.get("submission_scope") != "section_call_v1" or \
            capability.get("submission_install_status") != "installed":
        return unknown
    draw = one(records, "draw", draw_id)
    outcome = one(records, "draw_result", draw_id)
    frame = one(records, "scene_bridge_frame")
    end = one(records, "frame_end")
    if not snapshot_window_synchronized(frame, end, outcome) or not isinstance(draw, dict):
        return unknown
    if (not uint(frame.get("submission_started"), 64, True) or
            frame.get("submission_started") != frame.get("submission_completed") or
            frame.get("submission_pending") != 0 or frame.get("submission_dropped") != 0):
        return unknown
    ticket = draw.get("submission_ticket")
    if not uint(ticket, 64, True):
        return unknown
    begins = [r for r in records if r.get("event") == "scene_bridge_submission_begin" and r.get("ticket") == ticket]
    ends = [r for r in records if r.get("event") == "scene_bridge_submission_end" and r.get("ticket") == ticket]
    if len(begins) != 1 or len(ends) != 1:
        return unknown
    begin, finish = begins[0], ends[0]
    if (not records.index(begin) < records.index(draw) < records.index(outcome) < records.index(finish) or
            any(item.get("frame") != draw.get("frame") for item in (frame, end, begin, finish, outcome)) or
            begin.get("thread_id") != draw.get("thread_id") or finish.get("thread_id") != draw.get("thread_id") or
            draw.get("thread_id") != frame.get("present_thread") or finish.get("stable") is not True or
            begin.get("pass") != expected_pass or not uint(begin.get("stage")) or not uint(begin.get("section"))):
        return unknown
    matches = []
    for observation in (r for r in records if r.get("event") == "scene_bridge_observation"):
        if (observation.get("frame") != draw.get("frame") or records.index(observation) >= records.index(begin) or
                observation.get("thread_id") != draw.get("thread_id") or observation.get("self") != begin.get("self") or
                observation.get("caller") != EXPECTED_CALLER or observation.get("known_caller") is not True or
                observation.get("pass") != expected_pass or observation.get("pass_stable") is not True):
            continue
        camera = observation.get("current_camera")
        transforms = draw.get("transforms")
        if (not isinstance(camera, dict) or camera.get("stable") is not True or not isinstance(transforms, dict) or
                compare_matrix(camera.get("derived_matrix_bits"), transforms.get("view")) != "equal_bits" or
                compare_matrix(camera.get("projection_bits"), transforms.get("projection")) != "equal_bits"):
            continue
        world = transforms.get("world")
        if not isinstance(world, list) or not world or compare_matrix(begin.get("matrix_bits"), world[0]) != "equal_bits":
            continue
        if begin.get("type") == 4 and begin.get("vtable") == EXPECTED_ELEMENT_VTABLE:
            target = observation.get("target") or {}
            entry = target.get("entry") or {}
            node = target.get("mesh_node") or {}
            asset = target.get("asset") or {}
            owner_class = target.get("owner_class") or {}
            complete = (target.get("object_scan_complete") is True and target.get("list_scan_complete") is True and
                        target.get("object_scan_truncated") is False and target.get("list_scan_truncated") is False and
                        target.get("actors_named_target") == target.get("list_occurrences") == 1)
            if (complete and owner_link(entry, observation.get("object_table_complete")) and
                    owner_class.get("member") is True and owner_class.get("stable") is True and
                    owner_class.get("address") == entry.get("owner_class") and
                    fullname(owner_class.get("name"), owner_class.get("fname")) == "StaticMeshActor" and
                    node.get("stable") is True and trace_summary._bridge_mesh_node(node) and
                    trace_summary._bridge_asset(asset) and asset.get("status") == "candidate" and
                    trace_summary._bridge_asset_audit(asset.get("audit"), asset.get("name")) and
                    node.get("name", "").casefold() == asset.get("name", "").casefold() == TARGET_ASSET.casefold() and
                    begin.get("element") == entry.get("address") and begin.get("owner") == entry.get("owner_candidate") and
                    begin.get("owner_index") == entry.get("owner_object_index") and begin.get("owner_fname") == entry.get("owner_fname") and
                    begin.get("backlink") == entry.get("owner_backlink") and begin.get("mesh_node") == node.get("address") and
                    begin.get("asset_proxy") == asset["audit"].get("proxy") and
                    begin.get("asset_resource") == asset["audit"].get("resource") and
                    isinstance(begin.get("asset_name"), str) and begin["asset_name"].casefold() == asset["name"].casefold() and
                    begin.get("matrix_bits") == entry.get("object_matrix_bits") and
                    isinstance(node.get("sections"), list) and begin["section"] < len(node["sections"])):
                matches.append({"status": "matched", "kind": "static_mesh", "actor": TARGET_ACTOR,
                                "resource_name": asset["name"], "section": begin["section"], "stage": begin["stage"],
                                "observation_ticket": observation["ticket"]})
        elif begin.get("type") == 5 and begin.get("vtable") == 0x10e4a668:
            bsp = observation.get("bsp") or {}
            if bsp.get("source_stable") is not True or not trace_summary._bridge_bsp(bsp, observation.get("entries")):
                continue
            for block in bsp.get("candidates", []):
                if (block.get("stable") is True and block.get("address") == begin.get("element") and
                        block.get("source_index") == begin.get("bsp_index") and bsp.get("source") == begin.get("bsp_source") and
                        bsp.get("blocks") == begin.get("bsp_blocks") and block.get("transform_bits") == begin.get("matrix_bits") and
                        isinstance(block.get("sections"), list) and begin["section"] < len(block["sections"])):
                    matches.append({"status": "matched", "kind": "bsp", "runtime_index": block["source_index"],
                                    "section": begin["section"], "stage": begin["stage"],
                                    "observation_ticket": observation["ticket"]})
    return matches[0] if len(matches) == 1 else unknown


def analyze(records, frame, draw_id, expected_pass):
    """Return only proven comparisons and explicit unknowns for one draw."""
    records = [record for record in records if record.get("frame") == frame or record.get("event") == "header"]
    draw = one(records, "draw", draw_id)
    end = one(records, "frame_end")
    outcome = one(records, "draw_result", draw_id)
    bridge = one(records, "scene_bridge_frame")
    observation = pick_observation(records, draw_id) if draw else None
    result = {
        "frame": frame, "draw": draw_id, "actor": TARGET_ACTOR, "requested_pass": expected_pass,
        "present_interval": "unknown", "complete_observation_window": "unknown",
        "observation_window": "unknown", "static_caller": "unknown",
        "pass": "unknown", "actor_membership_and_backlink": "unknown", "actor_uniqueness": "unknown",
        "actor_transform": {"location_t3_units": None, "rotation_pitch_yaw_roll_units": None,
                            "element_matrix_16": None},
        "runtime_asset": {"status": "unknown", "matches_expected_name": "unknown"},
        "asset_property_audit": "unknown",
        "runtime_mesh_node": {"status": "unknown"},
        "runtime_bsp": {"status": "unknown", "candidates": []},
        "comparisons": {"camera_source_vs_draw_view": "unknown", "camera_derived_vs_draw_view": "unknown",
                        "camera_projection_vs_draw_projection": "unknown", "camera_viewport_vs_draw_viewport": "unknown",
                        "object_matrix_vs_draw_world": "unknown"},
        "actor_submitted_draw": "unknown",
        "limitations": ["The Present interval and temporal order do not identify which object submitted the draw.",
                        "A capture without explicit reentry detection cannot prove a complete observation window.",
                        "The source matrix's meaning is unverified; camera+0xe4 is used as a D3D view in one matched path.",
                        "Repeated reads do not exclude concurrent mutation or address reuse.",
                        "The asset candidate resolves only a local property; inherited defaults are not examined."]}
    if not synchronized(bridge, end, outcome) or not isinstance(draw, dict) or \
            draw.get("thread_id") != bridge.get("draw_thread"):
        return result
    result["present_interval"] = "synchronized"
    result["submission"] = submission_for_draw(records, draw_id, expected_pass)
    if result["submission"].get("kind") == "static_mesh":
        result["actor_submitted_draw"] = "scoped_virtual_call"
    if snapshot_window_synchronized(bridge, end, outcome):
        result["complete_observation_window"] = "synchronized"
    if not observation or observation.get("thread_id") != bridge.get("probe_thread"):
        return result
    result["observation_window"] = "candidate"
    if observation.get("caller") != EXPECTED_CALLER or observation.get("known_caller") is not True:
        return result
    result["static_caller"] = "matches_verified_callsite"
    if not uint(expected_pass) or observation.get("pass") != expected_pass or observation.get("pass_stable") is not True:
        return result
    result["pass"] = "matches"
    bsp = observation.get("bsp")
    if isinstance(bsp, dict) and bsp.get("source_stable") is True and \
            trace_summary._bridge_bsp(bsp, observation.get("entries")):
        candidates = []
        for block in bsp["candidates"]:
            if block["stable"]:
                candidates.append({"list_index": block["list_index"], "source_index": block["source_index"],
                                   "counts": block["counts"], "bounds_t3_units": decode_floats(block["bounds_bits"], 6),
                                   "transform": decode_floats(block["transform_bits"], 16),
                                   "sections": block["sections"]})
        entries = observation["entries"]
        list_complete = (observation.get("entries_truncated") is False and
                         observation.get("entries_attempted") == observation.get("count") == len(entries) and
                         all(uint(item.get("address"), nonzero=True) and uint(item.get("type")) and
                             uint(item.get("vtable"), nonzero=True) for item in entries))
        result["runtime_bsp"] = {"status": "vbsp_array_members", "candidates": candidates,
                                 "candidates_truncated": bsp["candidates_truncated"],
                                 "candidates_complete": (list_complete and not bsp["candidates_truncated"] and
                                                         all(block["stable"] for block in bsp["candidates"]))}
    camera = observation.get("current_camera")
    transforms = draw.get("transforms") if isinstance(draw, dict) else None
    if isinstance(camera, dict) and camera.get("stable") is True and isinstance(transforms, dict):
        comparisons = result["comparisons"]
        comparisons["camera_source_vs_draw_view"] = compare_matrix(camera.get("source_matrix_bits"), transforms.get("view"))
        comparisons["camera_derived_vs_draw_view"] = compare_matrix(camera.get("derived_matrix_bits"), transforms.get("view"))
        comparisons["camera_projection_vs_draw_projection"] = compare_matrix(camera.get("projection_bits"), transforms.get("projection"))
        comparisons["camera_viewport_vs_draw_viewport"] = compare_viewport(camera.get("viewport_bits"), draw.get("viewport"))

    target = observation.get("target")
    if not isinstance(target, dict):
        return result
    complete = (target.get("object_scan_complete") is True and target.get("list_scan_complete") is True and
                target.get("object_scan_truncated") is False and target.get("list_scan_truncated") is False and
                uint(target.get("objects_scanned"), nonzero=True) and uint(observation.get("count")) and
                target.get("list_scanned") == observation.get("count"))
    if not complete or target.get("actors_named_target") != 1 or target.get("list_occurrences") != 1:
        return result
    entry = target.get("entry")
    owner_class = target.get("owner_class")
    if not owner_link(entry, True) or not isinstance(owner_class, dict) or \
            owner_class.get("member") is not True or owner_class.get("stable") is not True or \
            owner_class.get("address") != entry.get("owner_class") or \
            fullname(owner_class.get("name"), owner_class.get("fname")) != "StaticMeshActor" or \
            not uint(entry.get("list_index")) or entry["list_index"] >= observation["count"]:
        return result
    result["actor_uniqueness"] = "one_in_complete_observed_scans"
    result["actor_membership_and_backlink"] = "matched"
    result["actor_transform"] = {
        "location_t3_units": decode_floats(entry.get("owner_location_bits"), 3),
        "rotation_pitch_yaw_roll_units": decode_rotator(entry.get("owner_rotation_bits")),
        "element_matrix_16": decode_floats(entry.get("object_matrix_bits"), 16),
    }
    if isinstance(transforms, dict):
        worlds = transforms.get("world")
        if isinstance(worlds, list) and worlds:
            result["comparisons"]["object_matrix_vs_draw_world"] = compare_matrix(entry.get("object_matrix_bits"), worlds[0])
    asset = target.get("asset")
    asset_checks = ("descriptor_match", "local_property_found", "proxy_live_member", "proxy_vtable_match",
                    "resource_vtable_match", "stable")
    if (isinstance(asset, dict) and asset.get("status") == "candidate" and
            all(asset.get(key) is True for key in asset_checks) and isinstance(asset.get("name"), str) and
            0 < len(asset["name"]) < 128 and all(32 <= ord(c) < 127 for c in asset["name"])):
        result["runtime_asset"] = {"status": "candidate", "name": asset["name"],
                                   "matches_expected_name": asset["name"].casefold() == TARGET_ASSET.casefold()}
        if trace_summary._bridge_asset_audit(asset.get("audit"), asset["name"]):
            result["asset_property_audit"] = "consistent"
    node = target.get("mesh_node")
    if isinstance(node, dict) and node.get("stable") is True and trace_summary._bridge_mesh_node(node):
        node_name = node["name"]
        asset_name = result["runtime_asset"].get("name")
        result["runtime_mesh_node"] = {
            "status": "candidate", "name": node_name, "counts": node["counts"],
            "bounds_t3_units": decode_floats(node["bounds_bits"], 6),
            "sections": node.get("sections"),
            "name_matches_asset": node_name.lower() == asset_name.lower() if asset_name else "unknown",
        }
    return result


def read_selected(path, frame, draw_id):
    # The shared validator rejects malformed, incomplete and inconsistent traces.
    before = path.stat()
    trace_summary.summarize(path)
    records = []
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        for raw in stream:
            digest.update(raw)
            item = json.loads(raw, object_pairs_hook=trace_summary._object, parse_constant=trace_summary._constant)
            if item.get("event") == "header" and item.get("scene_bridge", {}).get("submission_scope"):
                records.append(item)
                continue
            if item.get("frame") != frame:
                continue
            event = item.get("event")
            if event == "draw" and item.get("op_id") == draw_id:
                # Never retain or forward geometry payloads or resource identities.
                records.append({key: item.get(key) for key in ("event", "frame", "op_id", "thread_id", "transforms", "viewport", "submission_ticket")})
            elif event in ("scene_bridge_observation", "scene_bridge_frame", "frame_end",
                           "scene_bridge_submission_begin", "scene_bridge_submission_end") or \
                    event == "draw_result" and item.get("op_id") == draw_id:
                records.append(item)
        finished = os.fstat(stream.fileno())
    after = path.stat()
    fingerprint = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns)
    if len({fingerprint(s) for s in (before, opened, finished, after)}) != 1:
        raise ValueError("trace changed while reading")
    return records, digest.hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--frame", required=True, type=int)
    parser.add_argument("--draw", required=True, type=int, help="draw operation ID")
    parser.add_argument("--pass", dest="expected_pass", required=True, type=int)
    args = parser.parse_args(argv)
    if not uint(args.frame, 64) or not uint(args.draw, 64, nonzero=True) or not uint(args.expected_pass):
        parser.error("frame, draw and pass must be unsigned integers; draw must be nonzero")
    try:
        records, digest = read_selected(args.trace, args.frame, args.draw)
        if one(records, "frame_end") is None or one(records, "draw", args.draw) is None:
            raise ValueError("requested frame or draw is absent or ambiguous")
        result = analyze(records, args.frame, args.draw, args.expected_pass)
        result["trace_sha256"] = digest
        print(json.dumps(result, indent=2, allow_nan=False))
    except (OSError, ValueError, TypeError, json.JSONDecodeError, trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
