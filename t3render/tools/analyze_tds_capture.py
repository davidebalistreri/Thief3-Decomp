#!/usr/bin/env python3
"""Summarize a validated T3Render capture without copying game payloads.

Counts report observed candidates, not visible geometry or a replayable frame.
The source JSONL remains local; the summary contains no buffer or shader bytes.
"""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import sys

import trace_summary
import trace_to_ir


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def obj(value):
    return value if isinstance(value, dict) else {}


def succeeded(value):
    return isinstance(value, int) and not isinstance(value, bool) and (value & 0x80000000) == 0


def named(value, known):
    return value if isinstance(value, str) and value in known else "other"


def uint(value, bits):
    return value if type(value) is int and 0 <= value < (1 << bits) else None


def payload_reason(part):
    status = named(part.get("status", "unknown"), {"captured", "empty", "unknown", "window_unavailable",
        "getter_failed", "unbound", "unknown_range", "overflow", "invalid_stride", "descriptor_failed",
        "out_of_bounds", "unsupported_index_format", "unsupported_topology", "per_draw_limit", "frame_limit"})
    reason = (named(part["shadow_reason"], {"unknown_resource", "unknown_data", "abandoned", "budget_exceeded",
        "too_large", "allocation_failed", "busy", "invalid_range", "unavailable", "other"})
        if "shadow_reason" in part else None)
    cause = named(part["shadow_abandon_cause"], {"repeated_unlock"}) if "shadow_abandon_cause" in part else None
    return ":".join(str(item) for item in (status, reason, cause) if item is not None)


def provenance_group(part):
    if part.get("shadow_reason") != "unknown_resource":
        return None
    value = obj(part.get("shadow_provenance"))
    if not value:
        return None
    lookup = named(value.get("lookup"), {"live_tracked", "live_wrapper_mismatch", "live_reset_invalidated",
        "live_untracked", "native_not_found", "identity_unavailable", "retired_reset_invalidated",
        "retired_marker_untracked", "identity_not_tracked", "retired_tracked"})
    last_reset = named(value.get("last_reset"), {"not_observed", "pending", "succeeded", "failed"})
    known = value.get("history_known") is True
    last_lock = obj(value.get("last_lock")) if known else {}
    hr = last_lock.get("hr")
    hr = hr if type(hr) is int and -(1 << 31) <= hr < (1 << 31) else None
    return (lookup, last_reset, *(uint(value.get(key), 64) for key in
            ("reset_attempts", "reset_successes", "reset_failures")), known,
            *(uint(value.get(key), 64) if known else None for key in
              ("resets_since_creation", "lock_attempts", "successful_locks", "successful_write_unlocks")),
            *(uint(last_lock.get(key), 32) for key in ("offset", "requested_bytes", "flags")), hr)


def image_result(trace, event):
    """Validate the exact PPM generated for this frame; never accept a stale or partial file."""
    result = {k: event[k] for k in ("status", "stage", "hr", "width", "height", "format",
                                    "present_source_rect", "present_dest_rect")}
    if result["status"] != "captured":
        return result
    path = Path(str(trace) + ".frame-" + str(event["frame"]) + ".ppm")
    header = f"P6\n{result['width']} {result['height']}\n255\n".encode("ascii")
    try:
        if path.is_symlink() or path.stat().st_size != len(header) + result["width"] * result["height"] * 3:
            raise ValueError("size or path")
        with path.open("rb") as image:
            if image.read(len(header)) != header:
                raise ValueError("header")
        result["file"] = path.name
        result["sha256"] = digest(path)
    except (OSError, ValueError):
        result["status"] = "file_missing_or_invalid"
    return result


def analyze(path):
    framing = trace_summary.summarize(path)
    # The converter rejects every payload in a frame if its aggregate budget
    # is invalid. Compute those budgets before scoring any draw.
    budgets, begins, header = {}, {}, {}
    with Path(path).open(encoding="utf-8") as source:
        for line in source:
            event = json.loads(line)
            if event["event"] == "header":
                header = event
            elif event["event"] == "frame_begin":
                begins[event["frame"]] = event
                budgets[event["frame"]] = {"buffer": 0, "program": 0, "constants": 0, "history": 0}
            elif event["event"] == "draw":
                totals = budgets[event["frame"]]
                values = {"buffer": trace_to_ir.buffered_frame_bytes(event) if "buffer_data" in event else 0,
                          "program": trace_to_ir.vertex_program_frame_words(event, header),
                          "constants": trace_to_ir.vertex_constant_frame_words(event, header),
                          "history": trace_to_ir.pre_reset_frame_words(event, header)}
                for key, value in values.items():
                    totals[key] = totals[key] + value if totals[key] is not None and value is not None else None
    frames = []
    current = None
    pending = {}
    with Path(path).open(encoding="utf-8") as source:
        for line in source:
            event = json.loads(line)
            kind = event["event"]
            if kind == "header":
                header = event
            elif kind == "frame_begin":
                current = {"frame": event["frame"], "draws": 0, "indexed_draws": 0,
                           "stable_indexed_draws": 0, "indexed_with_both_buffers": 0,
                           "color_indexed_with_both_buffers": 0, "indexed_with_all_components": 0,
                           "color_indexed_with_all_components": 0, "post_repeat_vertex_payloads": 0,
                           "post_repeat_index_payloads": 0, "declaration_candidates": 0,
                           "program_candidates": 0, "constant_samples": 0,
                           "pre_reset_declaration_hypotheses": 0, "pre_reset_program_hypotheses": 0,
                           "declarations_after_reset_attempt": 0,
                           "reset_event_recorded": False,
                           "vertex_payloads": Counter(), "index_payloads": Counter(),
                           "buffer_window_failures": Counter(), "lock_failures": Counter(),
                           "_unknown_provenance": {}}
                if obj(header.get("frame_images")).get("requested") is True:
                    current["frame_image"] = {"status": "not_recorded"}
                if obj(header.get("scene_bridge")).get("requested") is True:
                    current["scene_bridge"] = {"status": "not_recorded", "observations": 0,
                                               "reported_entries": 0, "truncated_observations": 0}
                pending = {}
            elif kind == "draw" and current is not None:
                current["draws"] += 1
                states = obj(event.get("render_states"))
                color = states.get("168")
                color_writes = isinstance(color, int) and not isinstance(color, bool) and bool(color & 7)
                buffered = obj(event.get("buffer_data"))
                vertex, index = obj(buffered.get("vertex")), obj(buffered.get("index"))
                indexed = event.get("call") == "DrawIndexedPrimitive"
                decl = obj(event.get("vertex_declaration"))
                if decl.get("status") == "unavailable" and decl.get("reason") == "reset_attempt":
                    current["declarations_after_reset_attempt"] += 1
                if indexed:
                    current["indexed_draws"] += 1
                    current["vertex_payloads"][payload_reason(vertex)] += 1
                    current["index_payloads"][payload_reason(index)] += 1
                    for resource, part in (("vertex", vertex), ("index", index)):
                        group = provenance_group(part)
                        if group is not None:
                            item = current["_unknown_provenance"].setdefault((resource, *group), [0, set()])
                            item[0] += 1
                            pointer = part.get("resource")
                            if isinstance(pointer, str) and len(pointer) <= 32:
                                item[1].add(pointer)
                    failure = obj(buffered.get("window_failure"))
                    if failure:
                        # Deliberately exclude resource addresses and raw memory.
                        status = named(failure.get("status"), {"device_unsafe", "global_unsafe", "manager_unavailable",
                                                              "device_unknown", "window_unavailable"})
                        cause = named(failure.get("cause"), {"internal", "explicit", "state_block", "process_vertices",
                                                           "external_private_data", "unlock_mismatch", "lock_registry", "unknown"})
                        line = trace_to_ir.uint32(failure.get("source_line"))
                        label = f"{status}:{cause}:{line if line is not None else 'unknown'}"
                        current["buffer_window_failures"][label] += 1
                        lock = obj(failure.get("lock"))
                        if lock:
                            fields = (named(lock.get("resource"), {"vertex", "index"}),
                                named(lock.get("registry_result"), {"unknown_resource", "stale_generation", "invalid_range",
                                      "invalid_lock", "overlapping_locks", "unavailable", "other"}),
                                lock.get("recovery_attempted") if type(lock.get("recovery_attempted")) is bool else None,
                                uint(lock.get("offset"), 32), uint(lock.get("requested_bytes"), 32),
                                uint(lock.get("normalized_bytes"), 64), uint(lock.get("buffer_size"), 64),
                                uint(lock.get("flags"), 32))
                            current["lock_failures"][fields] += 1
                pending[event.get("op_id")] = {"indexed": indexed, "color": color_writes, "raw": event}
            elif kind == "draw_result" and current is not None:
                draw = pending.pop(event.get("op_id"), {})
                ok = succeeded(event.get("hr"))
                shader = ok and event.get("shader_stable") is True
                buffer = ok and event.get("buffer_stable") is True
                raw = draw.get("raw", {})
                totals = budgets[current["frame"]]
                def within(name, capability, field):
                    limit = trace_to_ir.uint32(obj(header.get(capability)).get(field))
                    return totals[name] is not None and limit is not None and totals[name] <= limit
                declaration = trace_to_ir.observed_declaration(raw, event, header)
                program = trace_to_ir.observed_vertex_program(raw, event, header,
                    within("program", "vertex_program_capture", "words_per_frame"))
                constants = trace_to_ir.observed_vertex_constants(raw, event, header, begins[current["frame"]],
                    within("constants", "vertex_constant_capture", "words_per_frame"))
                history_budget = within("history", "declaration_history", "words_per_frame")
                history = trace_to_ir.pre_reset_declaration(raw, event, header, history_budget)
                prior_program = trace_to_ir.pre_reset_vertex_program(raw, event, header,
                    within("program", "vertex_program_capture", "words_per_frame"), history_budget)
                has_decl = declaration.get("status") == "candidate"
                has_program = obj(program).get("status") == "candidate"
                has_constants = obj(constants).get("status") == "sampled"
                current["declaration_candidates"] += bool(has_decl)
                current["program_candidates"] += bool(has_program)
                current["constant_samples"] += bool(has_constants)
                current["pre_reset_declaration_hypotheses"] += history is not None
                current["pre_reset_program_hypotheses"] += prior_program is not None
                if draw.get("indexed"):
                    observed = trace_to_ir.buffered_data(raw, event, header, True,
                        within("buffer", "buffer_capture", "frame_limit_bytes"))
                    vertex, index = obj(observed.get("vertex")), obj(observed.get("index"))
                    current["stable_indexed_draws"] += buffer
                    both = buffer and vertex.get("status") == "captured" and index.get("status") == "captured"
                    full = both and shader and has_decl and has_program and has_constants
                    current["indexed_with_both_buffers"] += bool(both)
                    current["color_indexed_with_both_buffers"] += bool(both and draw.get("color"))
                    current["indexed_with_all_components"] += bool(full)
                    current["color_indexed_with_all_components"] += bool(full and draw.get("color"))
                    current["post_repeat_vertex_payloads"] += bool(vertex.get("status") == "captured" and vertex.get("shadow_observation") == "post_repeated_unlock_write")
                    current["post_repeat_index_payloads"] += bool(index.get("status") == "captured" and index.get("shadow_observation") == "post_repeated_unlock_write")
            elif kind == "reset" and current is not None:
                current["reset_event_recorded"] = True
            elif kind == "scene_bridge_observation" and current is not None and "scene_bridge" in current:
                bridge = current["scene_bridge"]
                bridge["observations"] += 1
                entries = event.get("entries")
                bridge["reported_entries"] += len(entries) if isinstance(entries, list) else 0
                bridge["truncated_observations"] += event.get("entries_truncated") is True
            elif kind == "scene_bridge_frame" and current is not None and "scene_bridge" in current:
                bridge = current["scene_bridge"]
                bridge["status"] = named(event.get("status"), {"synchronized", "unknown", "no_observation", "not_installed"})
                for field in ("started", "completed", "pending", "dropped"):
                    bridge[field] = uint(event.get(field), 64)
                for field in ("same_thread", "present_tracked", "epoch_unknown", "probe_overlap", "present_overlap",
                              "operation_overlap"):
                    bridge[field] = event.get(field) if type(event.get(field)) is bool else None
            elif kind == "frame_image" and current is not None:
                current["frame_image"] = image_result(path, event)
            elif kind == "frame_end" and current is not None:
                current["present_hr"] = event.get("present_hr")
                current["reset_during_frame"] = event.get("had_reset") if type(event.get("had_reset")) is bool else None
                current["concurrent_operations"] = event.get("concurrent_operations")
                current["eligible_for_candidate_selection"] = bool(
                    succeeded(event.get("present_hr")) and event.get("had_reset") is False and
                    event.get("concurrent_operations") is False and not current["reset_event_recorded"] and
                    type(event.get("draws")) is int and event["draws"] == current["draws"])
                for name in ("vertex_payloads", "index_payloads", "buffer_window_failures"):
                    current[name] = dict(sorted(current[name].items()))
                names = ("resource", "registry_result", "recovery_attempted", "offset", "requested_bytes",
                         "normalized_bytes", "buffer_size", "flags")
                current["lock_failures"] = [{**dict(zip(names, fields)), "draw_observations": count}
                    for fields, count in sorted(current["lock_failures"].items(), key=lambda item: repr(item[0]))]
                names = ("resource", "lookup", "last_reset", "reset_attempts", "reset_successes", "reset_failures",
                         "history_known", "resets_since_creation", "lock_attempts", "successful_locks",
                         "successful_write_unlocks", "last_lock_offset", "last_lock_requested_bytes", "last_lock_flags", "last_lock_hr")
                current["unknown_resource_provenance"] = [
                    {**dict(zip(names, fields)), "draw_observations": observations, "distinct_resource_pointers": len(pointers)}
                    for fields, (observations, pointers) in sorted(current.pop("_unknown_provenance").items(), key=lambda item: repr(item[0]))]
                frames.append(current)
                current = None
    facade = obj(header.get("experimental_facade"))
    eligible = [frame for frame in frames if frame["eligible_for_candidate_selection"] and
                frame["color_indexed_with_both_buffers"] > 0]
    best = max(eligible, key=lambda f: (f["color_indexed_with_all_components"],
                                        f["color_indexed_with_both_buffers"])) if eligible else None
    bridge_header = obj(header.get("scene_bridge"))
    bridge_requested = bridge_header.get("requested") is True
    bridge_statuses = {"installed", "disabled", "invalid_arguments", "already_attempted", "unsupported_processor",
                       "executable_identity_unavailable", "executable_mismatch", "image_mismatch", "prologue_mismatch",
                       "module_pin_failed", "hook_engine_failed", "hook_create_failed", "hook_enable_failed",
                       "hook_disable_failed", "not_reported"}
    return {"schema": 1, "status": "PARTIAL", "trace_sha256": digest(path),
            "frames_requested": header.get("requested_frames"), "frames_captured": framing["frame_count"],
            "scene_bridge": {"requested": bridge_requested,
                             "install_status": named(bridge_header.get("install_status"), bridge_statuses)
                             if bridge_requested else None},
            "declaration_history_requested": obj(header.get("declaration_history")).get("requested") is True,
            "best_frame_by_observed_candidates": best["frame"] if best else None,
            "experimental_facade": {k: facade.get(k) if type(facade.get(k)) is bool else None for k in
                                    ("enabled", "identity_verified", "concurrency_verified",
                                     "repeated_unlock_resource_discard", "repeated_unlock_post_write_recovery")},
            "frames": frames,
            "limitations": ["The bedroom and camera are not identified by the capture; user confirmation is required.",
                            "Buffer and shader candidates are structurally checked, but do not establish GPU inputs or visible draws.",
                            "Payload-reason and reset-attempt counts are reported trace statuses, not validated GPU state.",
                            "Buffer provenance counts only intercepted operations; pointers are grouped within each frame and are not proof of distinct COM identities.",
                            "Texture contents and pixel shaders are not captured.",
                            "An optional image is a pre-Present backbuffer; presentation transforms and concurrent Present calls are not validated.",
                            "Scene bridge synchronization describes the Present interval and thread, not proof of camera, pass, actor or mesh identity.",
                            "No full TDS-to-DX12 replay or pixel parity is established."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    parser.add_argument("-o", "--output", help="new summary JSON file; default: standard output")
    args = parser.parse_args(argv)
    try:
        result = analyze(args.trace)
        text = json.dumps(result, indent=2) + "\n"
        if args.output:
            with Path(args.output).open("x", encoding="utf-8") as output:
                output.write(text)
        else:
            print(text, end="")
    except (OSError, ValueError, trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
