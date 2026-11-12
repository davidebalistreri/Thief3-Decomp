#!/usr/bin/env python3
"""Run isolated D3D8 alias/buffer fixtures; never launch TDS.

Use only an idle Wine/Highball bottle. The proxy's VB/IB observer is explicitly
fixture-only. Unknown device aliases are reported and are never given wrappers.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import sys

from run_synthetic_up_replay import RunError, check_pe32, run_stage, sha256, valid_path, windows_path
from run_d3d8_stateblock_experiment import check_stage_loader
from run_d3d8_stateblock_fixture import inspect_buffer_range, inspect_image
import trace_summary
import trace_to_up_packet as trace


CASES = (
    "device_qi", "vb_device", "ib_device", "vb_unknown", "ib_unknown",
    "texture_device", "texture_resource", "texture_base", "texture_bound",
    "surface_device", "surface_container_device", "surface_container_unknown", "surface_container_texture",
    "backbuffer_device", "backbuffer_container_device", "cube_device", "cube_face_device",
    "volume_texture_device", "volume_device", "swap_surface_device",
)
CALLS = ("set_stream", "set_indices", "get_stream", "get_indices", "process_input", "process_target",
         "process_fvf", "process_stream", "process_vertices", "restore_fvf", "restore_stream", "clear_alias", "alias_draw")
HR = re.compile(r"0x[0-9a-f]{8}\Z")


def _object(pairs):
    out = {}
    for key, value in pairs:
        if key in out:
            raise ValueError(f"duplicate member {key}")
        out[key] = value
    return out


def _hr(record, field="hr"):
    value = record.get(field)
    if not isinstance(value, str) or HR.fullmatch(value) is None:
        raise RunError(f"invalid {field}")
    return int(value[2:], 16)


def inspect_log(path):
    try:
        records = []
        for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("alias-json "):
                record = json.loads(line[len("alias-json "):], object_pairs_hook=_object)
                if not isinstance(record, dict): raise RunError("fixture record must be an object")
                records.append(record)
        if len(records) < 2 or records[0].get("event") != "acquire" or records[-1].get("event") != "end":
            raise RunError("fixture records are incomplete or out of order")
        acquisition, end = records[0], records[-1]
        if (set(acquisition) != {"event", "stage", "hr", "nonnull"} or
                not isinstance(acquisition["stage"], str) or not acquisition["stage"] or
                type(acquisition["nonnull"]) is not bool or set(end) != {"event", "status"}):
            raise RunError("invalid acquisition or end record")
        hr = _hr(acquisition)
        status = end["status"]
        if status == "acquisition_failed":
            if len(records) != 2 or not hr & 0x80000000 or acquisition["nonnull"]:
                raise RunError("failed path has inconsistent acquisition")
            return {"status": status, "acquisition": acquisition, "alias": None, "calls": []}
        if status not in ("exercised", "unsafe_alias") or hr & 0x80000000 or not acquisition["nonnull"]:
            raise RunError("invalid successful alias outcome")
        alias = records[1]
        if set(alias) != {"event", "same_interface", "same_iunknown", "same_vtable", "vtable_unchanged", "roundtrip_hr", "roundtrip_same"} or alias.get("event") != "alias":
            raise RunError("missing alias identity")
        for key in ("same_interface", "same_iunknown", "same_vtable", "vtable_unchanged", "roundtrip_same"):
            if type(alias.get(key)) is not bool: raise RunError("invalid alias identity")
        safe = (all(alias[key] for key in ("same_interface", "same_iunknown", "same_vtable", "vtable_unchanged", "roundtrip_same")) and
                not _hr(alias, "roundtrip_hr") & 0x80000000)
        if status == "unsafe_alias":
            if len(records) != 3 or safe: raise RunError("unsafe alias was exercised or marked safe")
            return {"status": status, "acquisition": acquisition, "alias": alias, "calls": []}
        if not safe: raise RunError("buffer wrappers were sent to an unverified alias")
        body = records[2:-1]
        identity = [item for item in body if item.get("event") == "binding_identity"]
        calls = [item for item in body if item.get("event") == "call"]
        if (len(body) != len(CALLS) + 1 or len(identity) != 1 or
                set(identity[0]) != {"event", "vertex_same", "index_same"} or
                identity[0].get("vertex_same") is not True or identity[0].get("index_same") is not True or
                body.index(identity[0]) != 4 or tuple(item.get("name") for item in calls) != CALLS):
            raise RunError("incomplete or reordered alias calls")
        for item in calls:
            if set(item) != {"event", "name", "hr"}: raise RunError("invalid call record")
            if _hr(item) & 0x80000000 and item["name"] != "process_vertices":
                raise RunError(f"{item['name']} failed")
        return {"status": status, "acquisition": acquisition, "alias": alias, "calls": calls}
    except (OSError, UnicodeError, ValueError) as exc:
        raise RunError(f"invalid fixture log: {exc}") from exc


def inspect_trace(path, exercised):
    try:
        records = trace.validated_records(path)
        header = records[0]
        capability = header.get("buffer_capture")
        if not isinstance(capability, dict): raise RunError("buffer capability missing")
        component_limit = trace.integer(capability.get("component_limit_bytes"))
        frame_limit = trace.integer(capability.get("frame_limit_bytes"))
        if (capability.get("requested") is not True or capability.get("encoding") != "hex" or
                component_limit is None or not 0 < component_limit <= 256 * 1024 or
                frame_limit is None or not component_limit <= frame_limit <= 8 * 1024 * 1024):
            raise RunError("invalid buffer observation capability")
        begins = [item for item in records if item.get("event") == "frame_begin"]
        ends = [item for item in records if item.get("event") == "frame_end"]
        draws = [item for item in records if item.get("event") == "draw"]
        captured_bytes = 0
        for candidate in draws:
            data = candidate.get("buffer_data")
            if not isinstance(data, dict): continue
            for name in ("vertex", "index"):
                part = data.get(name)
                if not isinstance(part, dict) or part.get("status") != "captured": continue
                size, encoded = trace.integer(part.get("byte_count")), part.get("data")
                if (size is None or size > component_limit or part.get("encoding") != "hex" or
                        not isinstance(encoded, str) or len(encoded) != size * 2 or
                        re.fullmatch(r"[0-9a-fA-F]*", encoded) is None):
                    raise RunError("invalid captured component or component budget")
                captured_bytes += size
        if captured_bytes > frame_limit: raise RunError("capture exceeds frame budget")
        results = {item.get("op_id"): item for item in records if item.get("event") == "draw_result"}
        expected_events = ["header", "frame_begin", "clear", "clear_result", "draw", "draw_result"]
        if exercised: expected_events += ["clear", "clear_result", "draw", "draw_result"]
        expected_events += ["frame_end", "capture_end"]
        if [item.get("event") for item in records] != expected_events:
            raise RunError("fixture clear and draw order differs")
        if len(begins) != 1 or len(ends) != 1 or len(draws) != (2 if exercised else 1):
            raise RunError("unexpected frame or draw count")
        for clear in (item for item in records if item.get("event") == "clear"):
            if (trace.integer(clear.get("flags")) != 1 or trace.integer(clear.get("color")) != 0xff000000 or
                    trace.integer(clear.get("rect_count")) != 0):
                raise RunError("fixture clear differs")
        if any(not trace.hresult_succeeded(item.get("hr")) for item in records if item.get("event") == "clear_result"):
            raise RunError("fixture clear failed")
        end = ends[0]
        if (trace.integer(end.get("frame")) != 0 or trace.integer(end.get("draws")) != len(draws) or
                end.get("had_reset") is not False or end.get("concurrent_operations") is not False or
                not trace.hresult_succeeded(end.get("present_hr"))):
            raise RunError("incomplete or unstable frame")
        draw = draws[0]
        expected_draw = {"primitive_type": 4, "primitive_count": 1, "min_vertex_index": 0,
                         "num_vertices": 3, "start_index": 0, "vertex_shader": 0x44}
        if draw.get("call") != "DrawIndexedPrimitive" or any(trace.integer(draw.get(k)) != v for k, v in expected_draw.items()):
            raise RunError("unexpected control draw")
        result = results.get(draw.get("op_id"), {})
        if result.get("buffer_stable") is not True or not trace.hresult_succeeded(result.get("hr")):
            raise RunError("control draw did not observe a stable buffer window")
        streams = draw.get("streams")
        zero = [s for s in streams if isinstance(s, dict) and trace.integer(s.get("stream")) == 0] if isinstance(streams, list) else []
        if len(zero) != 1 or not isinstance(draw.get("indices"), dict): raise RunError("control bindings missing")
        vertex_binding, index_binding = zero[0], draw["indices"]
        vertex_desc, index_desc = vertex_binding.get("description"), index_binding.get("description")
        if (not trace.hresult_succeeded(vertex_binding.get("hr")) or not trace.hresult_succeeded(index_binding.get("hr")) or
                trace.integer(vertex_binding.get("stride")) != 20 or trace.integer(index_binding.get("base_vertex_index")) != 0 or
                not isinstance(vertex_desc, dict) or not isinstance(index_desc, dict) or
                any(trace.integer(vertex_desc.get(k)) != v for k, v in {"size": 60, "usage": 8, "pool": 1, "fvf": 0x44}.items()) or
                any(trace.integer(index_desc.get(k)) != v for k, v in {"size": 6, "usage": 8, "pool": 1, "format": 101}.items())):
            raise RunError("control binding values differ")
        expected = {
            "vertex": b"".join(struct.pack("<4fI", *v) for v in (
                (25.6, 217.6, 0, 1, 0xffff0000), (128, 25.6, 0, 1, 0xffff0000),
                (230.4, 217.6, 0, 1, 0xffff0000))).hex(),
            "index": struct.pack("<3H", 0, 1, 2).hex(),
        }
        data = draw.get("buffer_data")
        if not isinstance(data, dict) or data.get("source") != "application_lock_shadow": raise RunError("no buffer shadow")
        for name, binding, size in (("vertex", vertex_binding, 60), ("index", index_binding, 6)):
            part = data.get(name)
            inspect_buffer_range(name, part)
            if (part.get("status") != "captured" or part.get("encoding") != "hex" or
                    trace.integer(part.get("byte_count")) != size or part.get("data") != expected[name] or
                    not isinstance(binding.get("buffer"), str) or re.fullmatch(r"0x[0-9a-fA-F]{1,8}", binding["buffer"]) is None or
                    int(binding["buffer"], 16) == 0 or part.get("resource") != binding["buffer"]):
                raise RunError(f"control {name} bytes not captured")
        if exercised:
            last = draws[1]
            later = results.get(draws[1].get("op_id"), {})
            later_streams = last.get("streams")
            later_zero = [s for s in later_streams if isinstance(s, dict) and trace.integer(s.get("stream")) == 0] if isinstance(later_streams, list) else []
            later_index = last.get("indices")
            if (last.get("call") != "DrawIndexedPrimitive" or any(trace.integer(last.get(k)) != v for k, v in expected_draw.items()) or
                    not trace.hresult_succeeded(later.get("hr")) or len(later_zero) != 1 or
                    not isinstance(later_index, dict) or later_zero[0].get("buffer") != vertex_binding.get("buffer") or
                    later_index.get("buffer") != index_binding.get("buffer") or
                    not trace.hresult_succeeded(later_zero[0].get("hr")) or not trace.hresult_succeeded(later_index.get("hr")) or
                    trace.integer(later_zero[0].get("stride")) != 20 or trace.integer(later_index.get("base_vertex_index")) != 0):
                raise RunError("alias draw failed or missing")
            if type(later.get("buffer_stable")) is not bool:
                raise RunError("alias buffer stability missing")
            if later["buffer_stable"]:
                later_data = last.get("buffer_data")
                if not isinstance(later_data, dict) or later_data.get("source") != "application_lock_shadow":
                    raise RunError("stable alias draw has no buffer shadow")
                for name, binding, size in (("vertex", later_zero[0], 60), ("index", later_index, 6)):
                    part = later_data.get(name)
                    inspect_buffer_range(name, part)
                    if (part.get("status") != "captured" or part.get("encoding") != "hex" or
                            trace.integer(part.get("byte_count")) != size or part.get("data") != expected[name] or
                            part.get("resource") != binding.get("buffer")):
                        raise RunError(f"stable alias {name} bytes differ")
        return {"draws": len(draws), "control_vertex": "captured", "control_index": "captured",
                "alias_buffer_stable": results.get(draws[1].get("op_id"), {}).get("buffer_stable") if exercised else None}
    except (OSError, UnicodeError, ValueError, trace_summary.TraceError) as exc:
        raise RunError(f"invalid proxy trace: {exc}") from exc


def execute(engine, bottle, fixture, proxy, output, timeout):
    engine, bottle, fixture, proxy, output = map(valid_path, (engine, bottle, fixture, proxy, output))
    if any(not str(p).isascii() or '"' in str(p) for p in (fixture, proxy, output)):
        raise RunError("fixture, proxy and output paths must be ASCII without double quotes")
    wine, server, frameworks = engine / "engine/bin/wine", engine / "engine/bin/wineserver", engine / "frameworks"
    if not all(p.is_file() and os.access(p, os.X_OK) for p in (wine, server)) or not frameworks.is_dir():
        raise RunError("invalid Highball engine")
    z = bottle / "dosdevices/z:"
    if not bottle.is_dir() or not z.is_symlink() or os.readlink(z) != "/": raise RunError("bottle must map Z: to /")
    check_pe32(fixture, False); check_pe32(proxy, True)
    if not math.isfinite(timeout) or timeout <= 0: raise RunError("timeout must be positive and finite")
    for protected in (engine, bottle, fixture.parent, proxy.parent):
        if output.resolve() == protected.resolve() or protected.resolve() in output.resolve().parents:
            raise RunError("output must be outside engine, bottle and binary directories")
    fixture_hash, proxy_hash = sha256(fixture), sha256(proxy)
    try: output.mkdir()
    except OSError as exc: raise RunError(f"cannot create output directory: {exc}") from exc
    env = {k: v for k, v in os.environ.items() if not k.startswith("T3RENDER_CAPTURE_")}
    for key in ("WINEDLLPATH", "WINEPATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"): env.pop(key, None)
    env.update({"WINEPREFIX": str(bottle), "WINESERVER": str(server),
                "WINEDLLPATH_PREPEND": f"{bottle}/.highball-dlss/wine:{engine}/renderers/d3dmetal-tsshim/wine:{engine}/renderers/d3dmetal/wine",
                "DYLD_FALLBACK_LIBRARY_PATH": f"{frameworks}:{frameworks}/GStreamer.framework/Versions/1.0/lib",
                "DYLD_FALLBACK_FRAMEWORK_PATH": str(frameworks), "WINEDEBUG": "-all,+loaddll"})
    result = {"schema": 1, "test": "synthetic_d3d8_buffer_alias", "status": "FAIL",
              "fixture_sha256": fixture_hash, "proxy_sha256": proxy_hash, "cases": {},
              "limitations": ["Synthetic fixture only; no TDS process or game data.",
                              "Different device interface pointers are never given wrapped buffers.",
                              "Equal pointers in this runtime do not prove complete COM alias coverage.",
                              "ProcessVertices success and all optional paths must be checked individually.",
                              "Only RGB image parity is checked; native Windows and gameplay are untested."]}
    stage = "setup"
    try:
        partial = False
        # Wine finds native d3d8.dll beside the executable. Reuse one isolated
        # application directory per mode instead of copying binaries per case.
        applications = {}
        for mode in ("baseline", "proxy"):
            application = output / ("app_" + mode); application.mkdir()
            executable = application / fixture.name; shutil.copyfile(fixture, executable)
            if sha256(executable) != fixture_hash: raise RunError("fixture copy differs")
            dll = application / "d3d8.dll"
            if mode == "proxy":
                shutil.copyfile(proxy, dll)
                if sha256(dll) != proxy_hash: raise RunError("proxy copy differs")
            applications[mode] = (application, executable, dll)
        for case in CASES:
            entries = {}; images = []
            case_dir = output / case; case_dir.mkdir()
            for mode in ("baseline", "proxy"):
                stage = f"{case}/{mode}"
                directory = case_dir / mode; directory.mkdir()
                application, executable, dll = applications[mode]
                child = env.copy()
                child["WINEDLLOVERRIDES"] = "d3d8=b;nvapi64,nvngx=n,b;winemenubuilder.exe=d"
                if mode == "proxy":
                    child.update({"WINEDLLOVERRIDES": "d3d8=n,b;nvapi64,nvngx=n,b;winemenubuilder.exe=d",
                                  "T3RENDER_CAPTURE_FRAME": "0", "T3RENDER_CAPTURE_BUFFERS": "1",
                                  "T3RENDER_CAPTURE_PATH": windows_path(directory / "capture.jsonl")})
                log, ppm = directory / "run.log", directory / "image.ppm"
                run_stage(stage, [wine, executable, "--case", case, "--output", windows_path(ppm)], application, child, log, timeout)
                observed = inspect_log(log)
                check_stage_loader(log, bottle, dll if mode == "proxy" else None)
                images.append(inspect_image(ppm))
                shadow = inspect_trace(directory / "capture.jsonl", observed["status"] == "exercised") if mode == "proxy" else None
                entries[mode] = {"observation": observed, "log_sha256": sha256(log), "ppm_sha256": sha256(ppm), "shadow": shadow}
            if entries["baseline"]["observation"] != entries["proxy"]["observation"] or images[0] != images[1]:
                raise RunError(f"{case}: baseline/proxy observations or RGB pixels differ")
            observation = entries["proxy"]["observation"]
            if (observation["status"] != "exercised" or
                    entries["proxy"]["shadow"]["alias_buffer_stable"] is not True or any(
                        item["name"] == "process_vertices" and _hr(item) & 0x80000000 for item in observation["calls"])):
                partial = True
            result["cases"][case] = entries
        result["status"] = "PARTIAL" if partial else "PASS"
    except (RunError, OSError, ValueError) as exc:
        result["failed_stage"] = stage; result["error"] = str(exc)
    try:
        with (output / "result.json").open("x", encoding="utf-8") as destination:
            json.dump(result, destination, indent=2); destination.write("\n")
    except OSError as exc: raise RunError(f"cannot write result manifest: {exc}") from exc
    if result["status"] == "FAIL": raise RunError(f"{stage}: {result['error']}; partial output: {output}")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("engine", "bottle", "fixture", "proxy", "output-dir"):
        parser.add_argument("--" + option, required=True)
    parser.add_argument("--fixture-only", action="store_true", required=True,
                        help="attest the buffer observer is used only in this synthetic fixture")
    parser.add_argument("--timeout", type=float, default=45)
    args = parser.parse_args(argv)
    try: result = execute(args.engine, args.bottle, args.fixture, args.proxy, args.output_dir, args.timeout)
    except RunError as exc:
        print(f"error: {exc}", file=sys.stderr); return 1
    print(f"{result['status']}: {len(result['cases'])} isolated cases; results: {args.output_dir}")
    return 0 if result["status"] == "PASS" else 3


if __name__ == "__main__": raise SystemExit(main())
