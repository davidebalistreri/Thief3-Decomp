#!/usr/bin/env python3
"""Validate and summarize a T3Render diagnostic JSONL trace.

This format records diagnostics, not a replayable capture. Unknown events are
counted but deliberately not interpreted.
"""

import argparse
from collections import Counter
import json
import math
from pathlib import Path
import sys


class TraceError(ValueError):
    """The input is not a complete, well-formed diagnostic trace."""


def _is_int(value):
    # bool is a subclass of int in Python but is not a JSON integer.
    return isinstance(value, int) and not isinstance(value, bool)


def _uint(value, bits=32):
    return _is_int(value) and 0 <= value < (1 << bits)


def _nullable_uint(value):
    return value is None or _uint(value)


def _bridge_bits(value, count):
    return value is None or (isinstance(value, list) and len(value) == count and all(_uint(v) for v in value))


def _bridge_camera(value):
    return (isinstance(value, dict) and _nullable_uint(value.get("address")) and
            type(value.get("stable")) is bool and _bridge_bits(value.get("source_matrix_bits"), 16) and
            _bridge_bits(value.get("projection_bits"), 16) and _bridge_bits(value.get("derived_matrix_bits"), 16) and
            _bridge_bits(value.get("combined_matrix_bits"), 16) and
            _bridge_bits(value.get("viewport_bits"), 6))


def _bridge_entry(value):
    if not isinstance(value, dict):
        return False
    if not _uint(value.get("list_index")) or any(not _nullable_uint(value.get(k)) for k in ("address", "vtable", "type", "flags", "owner_candidate",
                                                      "owner_object_index", "owner_fname", "owner_class", "owner_backlink")):
        return False
    name = value.get("owner_name")
    return (type(value.get("owner_table_member")) is bool and type(value.get("owner_stable")) is bool and
            (name is None or isinstance(name, str) and len(name) < 64 and all(32 <= ord(c) < 127 for c in name)) and
            _bridge_bits(value.get("owner_location_bits"), 3) and _bridge_bits(value.get("owner_rotation_bits"), 3) and
            _bridge_bits(value.get("object_matrix_bits"), 16))


def _bridge_named(value):
    if not isinstance(value, dict) or any(not _nullable_uint(value.get(k)) for k in ("address", "index", "fname", "outer")):
        return False
    name = value.get("name")
    return (type(value.get("member")) is bool and type(value.get("stable")) is bool and
            (name is None or isinstance(name, str) and len(name) < 64 and all(32 <= ord(c) < 127 for c in name)))


def _bridge_target(value):
    if not isinstance(value, dict):
        return False
    flags = ("object_scan_complete", "list_scan_complete", "object_scan_truncated", "list_scan_truncated", "outer_truncated")
    counts = ("objects_scanned", "actors_named_target", "list_scanned", "list_occurrences", "outer_count")
    entries = value.get("outer")
    return (all(type(value.get(k)) is bool for k in flags) and all(_uint(value.get(k)) for k in counts) and
            isinstance(entries, list) and len(entries) == value["outer_count"] and len(entries) <= 4 and
            all(_bridge_named(e) for e in entries) and _bridge_named(value.get("owner_class")) and
            (value.get("entry") is None or _bridge_entry(value["entry"])) and _bridge_asset(value.get("asset")))


def _bridge_asset(value):
    statuses = {"unknown", "unreadable", "descriptor_unsupported", "local_property_missing", "property_malformed",
                "object_not_unique", "proxy_mismatch", "resource_mismatch", "candidate"}
    flags = ("descriptor_match", "local_property_found", "proxy_live_member", "proxy_vtable_match",
             "resource_vtable_match", "stable")
    if not isinstance(value, dict) or value.get("status") not in statuses or any(type(value.get(k)) is not bool for k in flags):
        return False
    name = value.get("name")
    if name is not None and (not isinstance(name, str) or len(name) >= 128 or not all(32 <= ord(c) < 127 for c in name)):
        return False
    return value["status"] != "candidate" or (all(value[k] for k in flags) and bool(name))


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON member {key!r}")
        result[key] = value
    return result


def _constant(value):
    raise ValueError(f"invalid JSON constant {value}")


def summarize(path):
    """Read a trace incrementally; raise TraceError on invalid input."""
    events = Counter()
    calls = Counter()
    frames = []
    current = None
    header_seen = False
    capture_ended = False
    operation_results = False
    frame_images = False
    bridge_capability = None
    bridge_tickets = set()
    clear_rect_limit = None
    requested_frames = None
    lines = 0

    try:
        with Path(path).open(encoding="utf-8") as stream:
            for lines, raw in enumerate(stream, 1):
                try:
                    item = json.loads(raw, object_pairs_hook=_object, parse_constant=_constant)
                except ValueError as exc:
                    raise TraceError(f"line {lines}: invalid JSON: {exc}") from exc
                if not isinstance(item, dict):
                    raise TraceError(f"line {lines}: expected a JSON object")
                event = item.get("event")
                if not isinstance(event, str) or not event:
                    raise TraceError(f"line {lines}: event must be a non-empty string")
                if capture_ended:
                    raise TraceError(f"line {lines}: event after capture_end")
                if current is not None and current["bridge_finished"] and event != "frame_end":
                    raise TraceError(f"line {lines}: event after scene bridge frame result")

                if not header_seen:
                    if event != "header":
                        raise TraceError(f"line {lines}: first event must be header")
                    if item.get("format") != "t3render-d3d8-diagnostic":
                        raise TraceError(f"line {lines}: unsupported trace format")
                    if not _is_int(item.get("version")) or item["version"] != 1:
                        raise TraceError(f"line {lines}: unsupported trace version")
                    if "requested_frames" in item:
                        requested_frames = item["requested_frames"]
                        if not _is_int(requested_frames) or not 1 <= requested_frames <= 3:
                            raise TraceError(f"line {lines}: requested_frames must be an integer from 1 through 3")
                    if "operation_results" in item:
                        if not _is_int(item["operation_results"]) or item["operation_results"] != 1:
                            raise TraceError(f"line {lines}: unsupported operation result capability")
                        clear_rect_limit = item.get("clear_rect_limit")
                        if not _is_int(clear_rect_limit) or clear_rect_limit <= 0:
                            raise TraceError(f"line {lines}: clear_rect_limit must be a positive integer")
                        operation_results = True
                    if "frame_images" in item:
                        if item["frame_images"] != {"requested": True, "timing": "before_present"}:
                            raise TraceError(f"line {lines}: invalid frame image capability")
                        frame_images = True
                    if "scene_bridge" in item:
                        bridge_capability = item["scene_bridge"]
                        statuses = {"installed", "disabled", "invalid_arguments", "already_attempted",
                                    "unsupported_processor", "executable_identity_unavailable", "executable_mismatch",
                                    "image_mismatch", "prologue_mismatch", "module_pin_failed", "hook_engine_failed",
                                    "hook_create_failed", "hook_enable_failed", "hook_disable_failed", "not_reported"}
                        if (not isinstance(bridge_capability, dict) or bridge_capability.get("requested") is not True or
                            bridge_capability.get("install_status") not in statuses or
                            bridge_capability.get("synchronization") != "present_interval_and_thread_only" or
                            not _is_int(bridge_capability.get("probe_limit")) or
                            not 0 < bridge_capability["probe_limit"] <= 128 or
                            not _is_int(bridge_capability.get("entries_per_probe")) or
                            not 0 < bridge_capability["entries_per_probe"] <= 128 or
                            not _is_int(bridge_capability.get("target_object_limit")) or
                            not 0 < bridge_capability["target_object_limit"] <= 65536 or
                            not _is_int(bridge_capability.get("target_list_limit")) or
                            not 0 < bridge_capability["target_list_limit"] <= 8192 or
                            bridge_capability.get("reentry_detection") not in (None, "saturating_counter_v1")):
                            raise TraceError(f"line {lines}: invalid scene bridge capability")
                    header_seen = True
                elif event == "header":
                    raise TraceError(f"line {lines}: duplicate header")
                elif event == "capture_end":
                    if current is not None:
                        raise TraceError(f"line {lines}: capture_end inside a frame")
                    frame_count = item.get("frames")
                    if not _is_int(frame_count):
                        raise TraceError(f"line {lines}: capture_end frames must be an integer")
                    if frame_count != len(frames):
                        raise TraceError(
                            f"line {lines}: capture_end frames {frame_count} does not match {len(frames)} complete frames"
                        )
                    if requested_frames is not None and frame_count != requested_frames:
                        raise TraceError(f"line {lines}: capture_end frames does not match requested_frames")
                    capture_ended = True
                elif event == "frame_begin":
                    frame = item.get("frame")
                    if not _is_int(frame):
                        raise TraceError(f"line {lines}: frame must be an integer")
                    if current is not None:
                        raise TraceError(f"line {lines}: frame_begin before previous frame_end")
                    if requested_frames is not None:
                        if frame < 0 or frame >= (1 << 64) - 1:
                            raise TraceError(f"line {lines}: frame outside requested window range")
                        if frames and frame != frames[-1]["frame"] + 1:
                            raise TraceError(f"line {lines}: requested capture frames are not consecutive")
                    current = {"frame": frame, "draws": 0, "calls": Counter(), "pending": {},
                               "last_op": 0, "overlap": False, "image_seen": False,
                               "bridge_finished": False, "bridge_frame": None, "bridge_count": 0,
                               "bridge_threads": set(), "bridge_draw_threads": set(), "bridge_order_valid": True}
                elif event == "scene_bridge_observation":
                    if (bridge_capability is None or bridge_capability["install_status"] != "installed" or
                            current is None or item.get("frame") != current["frame"] or current["bridge_finished"]):
                        raise TraceError(f"line {lines}: scene observation outside an installed capture frame")
                    ticket, thread, next_op = item.get("ticket"), item.get("thread_id"), item.get("next_d3d_operation")
                    if (not _uint(ticket, 64) or not ticket or ticket in bridge_tickets or not _uint(thread) or not thread or
                            not _uint(next_op, 64) or not next_op or not _uint(item.get("self")) or
                            not _uint(item.get("caller")) or type(item.get("known_caller")) is not bool or
                            type(item.get("pass_stable")) is not bool or
                            any(not _nullable_uint(item.get(k)) for k in ("pass", "count", "array")) or
                            any(not _bridge_camera(item.get(k)) for k in ("configured_camera", "current_camera", "culling_camera"))):
                        raise TraceError(f"line {lines}: invalid scene observation metadata")
                    entries = item.get("entries")
                    if (type(item.get("object_table_complete")) is not bool or type(item.get("entries_truncated")) is not bool or
                            not _uint(item.get("entries_attempted")) or not isinstance(entries, list) or
                            len(entries) != item["entries_attempted"] or len(entries) > bridge_capability["entries_per_probe"] or
                            any(not _bridge_entry(e) for e in entries) or not _bridge_target(item.get("target"))):
                        raise TraceError(f"line {lines}: invalid bounded scene entries")
                    target = item["target"]
                    if (target["objects_scanned"] > bridge_capability["target_object_limit"] or
                            target["list_scanned"] > bridge_capability["target_list_limit"] or
                            target["actors_named_target"] > target["objects_scanned"] or
                            target["list_occurrences"] > target["list_scanned"] or
                            (target["object_scan_complete"] and target["object_scan_truncated"]) or
                            (target["list_scan_complete"] and target["list_scan_truncated"])):
                        raise TraceError(f"line {lines}: invalid target scan counts")
                    bridge_tickets.add(ticket)
                    current["bridge_count"] += 1
                    current["bridge_threads"].add(thread)
                    if current["image_seen"] or current["pending"] or next_op <= current["last_op"]:
                        current["bridge_order_valid"] = False
                    if current["bridge_count"] > bridge_capability["probe_limit"]:
                        raise TraceError(f"line {lines}: scene observation limit exceeded")
                elif event == "scene_bridge_frame":
                    if (bridge_capability is None or current is None or item.get("frame") != current["frame"] or
                            current["bridge_finished"]):
                        raise TraceError(f"line {lines}: invalid scene bridge frame result")
                    statuses = {"synchronized", "unknown", "no_observation", "not_installed"}
                    flags = ("previous_present_observed", "same_thread", "present_tracked", "epoch_unknown",
                             "probe_overlap", "present_overlap", "operation_overlap", "reset_seen", "reset_pending")
                    threads = ("previous_present_thread", "present_thread", "draw_thread", "probe_thread")
                    counts = ("started", "completed", "pending", "dropped")
                    reentry_capable = bridge_capability.get("reentry_detection") == "saturating_counter_v1"
                    if ((reentry_capable and type(item.get("probe_reentered")) is not bool) or
                            (not reentry_capable and "probe_reentered" in item) or
                            (item.get("probe_reentered") is True and item.get("probe_overlap") is not True)):
                        raise TraceError(f"line {lines}: invalid scene probe reentry metadata")
                    if (item.get("status") not in statuses or any(type(item.get(k)) is not bool for k in flags) or
                            any(not _uint(item.get(k)) for k in threads) or any(not _uint(item.get(k), 64) for k in counts) or
                            item["started"] > bridge_capability["probe_limit"] or
                            item["completed"] != current["bridge_count"] or
                            item["started"] < item["completed"] + item["pending"]):
                        raise TraceError(f"line {lines}: invalid scene bridge frame metadata")
                    if item["status"] == "not_installed" and bridge_capability["install_status"] == "installed":
                        raise TraceError(f"line {lines}: inconsistent scene bridge install status")
                    if item["status"] == "no_observation" and (item["started"] or item["dropped"]):
                        raise TraceError(f"line {lines}: inconsistent empty scene observation")
                    if item["status"] == "synchronized":
                        thread = item["present_thread"]
                        if (bridge_capability["install_status"] != "installed" or not item["previous_present_observed"] or
                                not item["same_thread"] or not item["present_tracked"] or item["epoch_unknown"] or
                                item["probe_overlap"] or item["present_overlap"] or item["operation_overlap"] or
                                item["reset_seen"] or item["reset_pending"] or
                                not item["completed"] or item["started"] != item["completed"] or item["pending"] or
                                item["dropped"] or not thread or any(item[k] != thread for k in threads) or
                                current["bridge_threads"] != {thread} or current["bridge_draw_threads"] != {thread} or
                                not current["bridge_order_valid"] or
                                current["pending"] or current["overlap"] or not current["draws"]):
                            raise TraceError(f"line {lines}: unsupported synchronized scene claim")
                    current["bridge_finished"] = True
                    current["bridge_frame"] = item
                elif event == "frame_image":
                    if not frame_images or current is None or item.get("frame") != current["frame"] or current["image_seen"]:
                        raise TraceError(f"line {lines}: image must appear once in its matching frame")
                    if current["pending"]:
                        raise TraceError(f"line {lines}: image overlaps an operation")
                    stages = {"complete", "not_started", "pending_operations", "get_backbuffer", "get_description",
                              "unsupported_format_or_size", "create_image_surface", "copy_backbuffer", "lock_image",
                              "unlock_image", "invalid_pitch", "create_file", "write_file", "exception"}
                    if item.get("status") not in ("captured", "unavailable") or item.get("stage") not in stages:
                        raise TraceError(f"line {lines}: invalid image result")
                    if not _is_int(item.get("hr")) or any(not _is_int(item.get(k)) or item[k] < 0
                        for k in ("width", "height", "format")) or any(type(item.get(k)) is not bool
                        for k in ("present_source_rect", "present_dest_rect")):
                        raise TraceError(f"line {lines}: invalid image metadata")
                    if item["status"] == "captured" and (item["stage"] != "complete" or item["hr"] != 0 or
                        not 0 < item["width"] <= 8192 or not 0 < item["height"] <= 8192 or
                        item["width"] * item["height"] > 16777216 or item["format"] not in (21, 22)):
                        raise TraceError(f"line {lines}: invalid captured image metadata")
                    current["image_seen"] = True
                elif event == "reset" and current is not None and current["image_seen"]:
                    raise TraceError(f"line {lines}: Reset observed after the frame image")
                elif event in ("draw", "draw_result", "clear", "clear_result", "frame_end"):
                    if current is None:
                        raise TraceError(f"line {lines}: {event} outside a frame")
                    frame = item.get("frame")
                    if not _is_int(frame):
                        raise TraceError(f"line {lines}: frame must be an integer")
                    if frame != current["frame"]:
                        raise TraceError(
                            f"line {lines}: frame {frame} does not match active frame {current['frame']}"
                        )
                    if event in ("draw", "clear"):
                        if current["image_seen"]:
                            raise TraceError(f"line {lines}: operation observed after the frame image")
                        has_buffer_data = event == "draw" and "buffer_data" in item
                        has_declaration = event == "draw" and "vertex_declaration" in item
                        if has_buffer_data and not isinstance(item["buffer_data"], dict):
                            raise TraceError(f"line {lines}: buffer_data must be an object")
                        if has_buffer_data and not operation_results:
                            raise TraceError(f"line {lines}: buffer_data requires operation result capability")
                        if has_declaration and not isinstance(item["vertex_declaration"], dict):
                            raise TraceError(f"line {lines}: vertex_declaration must be an object")
                        if has_declaration and not operation_results:
                            raise TraceError(f"line {lines}: vertex_declaration requires operation result capability")
                        if event == "clear":
                            if not operation_results:
                                raise TraceError(f"line {lines}: clear requires operation result capability")
                            count = item.get("rect_count")
                            status = item.get("rects_status")
                            rects = item.get("rects")
                            if not _is_int(count) or not 0 <= count <= 0xffffffff:
                                raise TraceError(f"line {lines}: invalid clear rect_count")
                            if status == "none":
                                if count != 0 or rects != []:
                                    raise TraceError(f"line {lines}: invalid empty clear rectangles")
                            elif status == "captured":
                                if not 0 < count <= clear_rect_limit or not isinstance(rects, list) or len(rects) != count:
                                    raise TraceError(f"line {lines}: invalid captured clear rectangles")
                                for rect in rects:
                                    if not isinstance(rect, dict) or set(rect) != {"x1", "y1", "x2", "y2"} or any(
                                        not _is_int(value) or not -(1 << 31) <= value < (1 << 31)
                                        for value in rect.values()
                                    ):
                                        raise TraceError(f"line {lines}: invalid clear rectangle")
                            elif status in ("inconsistent", "null_pointer", "count_limit", "unreadable", "address_overflow"):
                                if rects is not None:
                                    raise TraceError(f"line {lines}: unknown clear rectangles must be null")
                                if ((status == "inconsistent" and count != 0) or
                                    (status == "null_pointer" and count == 0) or
                                    (status == "count_limit" and count <= clear_rect_limit) or
                                    (status in ("unreadable", "address_overflow") and not 0 < count <= clear_rect_limit)):
                                    raise TraceError(f"line {lines}: clear rectangle status does not match count")
                            else:
                                raise TraceError(f"line {lines}: invalid clear rectangle status")
                            for field in ("flags", "color", "depth_bits", "stencil"):
                                value = item.get(field)
                                if not _is_int(value) or not 0 <= value <= 0xffffffff:
                                    raise TraceError(f"line {lines}: invalid clear {field}")
                            depth = item.get("depth")
                            if depth is not None and (isinstance(depth, bool) or not isinstance(depth, (int, float)) or
                                                      (isinstance(depth, float) and not math.isfinite(depth))):
                                raise TraceError(f"line {lines}: invalid clear depth")
                        if operation_results or event == "clear" or "op_id" in item:
                            op_id = item.get("op_id")
                            if not _is_int(op_id) or not current["last_op"] < op_id < (1 << 64) - 1:
                                raise TraceError(f"line {lines}: operation id must increase within the frame")
                            if current["pending"]:
                                current["overlap"] = True
                            current["last_op"] = op_id
                            current["pending"][op_id] = (event, has_buffer_data)
                    if event == "draw":
                        call = item.get("call")
                        if not isinstance(call, str) or not call:
                            raise TraceError(f"line {lines}: draw call must be a non-empty string")
                        current["draws"] += 1
                        if bridge_capability is not None:
                            thread = item.get("thread_id")
                            if not _uint(thread):
                                raise TraceError(f"line {lines}: scene capture draw requires a thread ID")
                            current["bridge_draw_threads"].add(thread)
                        current["calls"][call] += 1
                        calls[call] += 1
                    elif event in ("draw_result", "clear_result"):
                        op_id = item.get("op_id")
                        pending = current["pending"].get(op_id) if _is_int(op_id) else None
                        if pending is None or pending[0] != event.removesuffix("_result"):
                            raise TraceError(f"line {lines}: result has no matching operation")
                        if not _is_int(item.get("hr")) or not -(1 << 31) <= item["hr"] < (1 << 32):
                            raise TraceError(f"line {lines}: operation hr must be an integer")
                        if "shader_stable" in item and (event != "draw_result" or not isinstance(item["shader_stable"], bool)):
                            raise TraceError(f"line {lines}: shader_stable requires a boolean draw result")
                        if pending[1]:
                            if not isinstance(item.get("buffer_stable"), bool):
                                raise TraceError(f"line {lines}: buffered draw result requires boolean buffer_stable")
                        elif "buffer_stable" in item:
                            raise TraceError(f"line {lines}: buffer_stable has no matching buffer_data")
                        del current["pending"][op_id]
                    elif event == "frame_end":
                        hr = item.get("present_hr")
                        if not _is_int(hr):
                            raise TraceError(f"line {lines}: present_hr must be an integer")
                        if current["pending"]:
                            raise TraceError(f"line {lines}: frame ends with pending operations")
                        if operation_results and not isinstance(item.get("concurrent_operations"), bool):
                            raise TraceError(f"line {lines}: concurrent_operations must be a boolean")
                        if current["overlap"] and item.get("concurrent_operations") is not True:
                            raise TraceError(f"line {lines}: overlapping operations were not flagged")
                        if bridge_capability is not None and not current["bridge_finished"]:
                            raise TraceError(f"line {lines}: scene bridge frame result is missing")
                        bridge_frame = current["bridge_frame"]
                        if bridge_frame and bridge_frame["status"] == "synchronized" and (
                                not -(1 << 31) <= hr < (1 << 32) or (hr & 0x80000000) != 0 or
                                item.get("had_reset") is not False or item.get("concurrent_operations") is not False):
                            raise TraceError(f"line {lines}: frame outcome invalidates synchronized scene claim")
                        if "concurrent_operations" in item:
                            current["concurrent_operations"] = item["concurrent_operations"]
                        current["present_hr"] = hr
                        current["calls"] = dict(sorted(current["calls"].items()))
                        del current["pending"]
                        del current["last_op"]
                        del current["overlap"]
                        del current["image_seen"]
                        del current["bridge_finished"]
                        del current["bridge_frame"]
                        del current["bridge_count"]
                        del current["bridge_threads"]
                        del current["bridge_draw_threads"]
                        del current["bridge_order_valid"]
                        frames.append(current)
                        current = None
                events[event] += 1
    except (OSError, UnicodeError) as exc:
        raise TraceError(f"cannot read trace: {exc}") from exc

    if not header_seen:
        raise TraceError("empty trace: missing header")
    if current is not None:
        raise TraceError(f"unexpected EOF: frame {current['frame']} has no frame_end")
    if not capture_ended:
        raise TraceError("unexpected EOF: missing capture_end")
    if not frames:
        raise TraceError("trace has no complete frames")
    if not calls and not events["clear"]:
        raise TraceError("trace has no draw events or clear events")

    return {
        "format": "t3render-d3d8-diagnostic",
        "version": 1,
        "lines": lines,
        "frame_count": len(frames),
        "draw_count": sum(calls.values()),
        "calls": dict(sorted(calls.items())),
        "events": dict(sorted(events.items())),
        "frames": frames,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", help="diagnostic JSONL file")
    parser.add_argument("--json", action="store_true", help="write machine-readable summary")
    args = parser.parse_args(argv)
    try:
        summary = summarize(args.trace)
    except TraceError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        print(f"Valid diagnostic trace: {summary['frame_count']} frames, {summary['draw_count']} draws")
        for call, count in summary["calls"].items():
            print(f"  {call}: {count}")
        for frame in summary["frames"]:
            detail = ", ".join(f"{name}={count}" for name, count in frame["calls"].items()) or "no draws"
            print(f"  frame {frame['frame']}: {frame['draws']} draws ({detail}), present_hr={frame['present_hr']}")
        print("Diagnostic data only; replay and visual parity are not verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
