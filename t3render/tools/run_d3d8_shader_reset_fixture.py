#!/usr/bin/env python3
"""Compare synthetic D3D8 shader behavior across failed and successful Reset."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

from run_synthetic_up_replay import RunError, check_pe32, run_stage, sha256, valid_path, windows_path
from run_d3d8_stateblock_experiment import check_stage_loader
import trace_summary

WORDS = [0x20000000, 0x40020000, 0x40040005, 0xffffffff]
PROGRAM = [0xfffe0101, 1, 0xc00f0000, 0x90e40000, 1, 0xd00f0000, 0xa0e40000, 0xffff]
HR = re.compile(r"0x[0-9a-f]{8}\Z")
FVF = 0x42
SCENARIOS = ("valid-only", "recovery")
PARITY_CALLS = ("GetAdapterDisplayMode", "CreateDevice") + 3 * (
    "SetWorld", "SetView", "SetProjection", "Lighting", "Cull", "Depth", "Texture", "ColorOp", "ColorArg",
    "Stage1", "PixelShader", "ShaderConstant", "Clear", "BeginScene", "Draw", "EndScene", "GetBackBuffer", "GetDesc",
    "LockRect", "UnlockRect", "Present")


def inspect_image(path, original):
    header = b"P6\n256 256\n255\n"
    with Path(path).open("rb") as source:
        image = source.read(len(header) + 256 * 256 * 3 + 1)
    if len(image) != len(header) + 256 * 256 * 3 or not image.startswith(header):
        raise RunError("fixture image is not a complete 256x256 RGB PPM")
    pixels = image[len(header):]
    color = b"\0\xff\0" if original else b"\xff\0\0"
    center = (128 * 256 + 128) * 3
    if (pixels[:3] != b"\0\0\0" or pixels[center:center + 3] != color or
            sum(pixels[i:i + 3] == color for i in range(0, len(pixels), 3)) <= 1000):
        raise RunError("fixture image does not match the selected shader or fallback")
    return image


def original_pixels_preserved(images, phases):
    if len(images) != 3 or len(phases) != 3 or phases[0].get("original") is not True:
        return False
    return all(phase.get("original") is True and image == images[0]
               for image, phase in zip(images, phases))


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result: raise ValueError(f"duplicate member {key}")
        result[key] = value
    return result


def hresult(record):
    value = record.get("hr")
    if not isinstance(value, str) or HR.fullmatch(value) is None: raise RunError("invalid HRESULT record")
    return int(value[2:], 16)


def failed(value):
    return bool(value & 0x80000000)


def inspect_log(path, scenario):
    records, parity, handles, timeline = [], [], [], []
    try:
        for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("reset-json "):
                item = json.loads(line[len("reset-json "):], object_pairs_hook=unique)
                if not isinstance(item, dict): raise RunError("reset record is not an object")
                records.append(item)
                timeline.append(("reset", item.get("event"), item.get("phase")))
            elif line.startswith("parity "):
                match = re.fullmatch(r"parity (\w+) hr=([0-9a-f]{8})", line)
                if not match or failed(int(match.group(2), 16)): raise RunError("fixture parity call failed")
                parity.append(line)
                timeline.append(("parity", match.group(1), None))
            elif line.startswith("shader-handle "):
                match = re.fullmatch(r"shader-handle ([0-9a-f]{8})", line)
                if not match or int(match.group(1), 16) == 0: raise RunError("invalid shader handle")
                handles.append(int(match.group(1), 16))
            elif line == "reset-fixture result=PASS":
                handles.append("complete")
        if (scenario not in SCENARIOS or len(handles) != 2 or handles[1] != "complete" or
                tuple(line.split()[1] for line in parity) != PARITY_CALLS):
            raise RunError("fixture did not complete")
        if scenario == "recovery":
            failed_entries = [i for i, entry in enumerate(timeline) if entry == ("reset", "failed_reset", 1)]
            recovery_entries = [i for i, entry in enumerate(timeline) if entry == ("reset", "recovery_reset", 1)]
            if len(failed_entries) != 1 or len(recovery_entries) != 1 or recovery_entries[0] != failed_entries[0] + 1:
                raise RunError("device operation occurred between failed and recovery Reset")
        position, partial, phases, resets = 0, False, [], []

        def take(name, phase):
            nonlocal position
            if position >= len(records): raise RunError(f"missing {name} record")
            item = records[position]; position += 1
            if item.get("event") != name or type(item.get("phase")) is not int or item["phase"] != phase:
                raise RunError(f"unexpected {name} record")
            hresult(item)
            return item

        def take_words(name, phase, expected):
            nonlocal partial
            query = take(name + "_size", phase)
            if set(query) != {"event", "phase", "hr", "size"}:
                raise RunError(f"invalid {name} size query")
            size = query["size"]
            if failed(hresult(query)):
                if size is not None: raise RunError(f"failed {name} size query returned data")
                partial = True
                return {"query": query, "copy": None}
            if type(size) is not int or not 0 <= size <= 0xffffffff:
                raise RunError(f"invalid {name} query size")
            if not size or size > 4096 or size % 4:
                partial = True
                return {"query": query, "copy": None}
            copied = take(name, phase)
            if set(copied) != {"event", "phase", "hr", "requested_bytes", "returned_bytes", "second_hr",
                               "second_returned_bytes", "verify_hr", "verify_size", "tail_intact", "reads_match", "words"}:
                raise RunError(f"invalid {name} copy record")
            requested, returned, words = copied["requested_bytes"], copied["returned_bytes"], copied["words"]
            if (type(requested) is not int or requested != size or type(returned) is not int or
                    not 0 <= returned <= 0xffffffff or type(copied["tail_intact"]) is not bool):
                raise RunError(f"invalid {name} copy size")
            first_failed = failed(hresult(copied)) or returned != size
            if first_failed or (not copied["tail_intact"] and copied["second_hr"] is None):
                if copied["second_hr"] is not None:
                    raise RunError(f"{name} second copy followed a failed first copy")
                if any(copied[key] is not None for key in ("second_returned_bytes", "verify_hr", "verify_size", "reads_match", "words")):
                    raise RunError(f"incomplete {name} first copy exposed data")
                partial = True
                return {"query": query, "copy": copied}
            second = copied["second_hr"]
            if not isinstance(second, str) or HR.fullmatch(second) is None or type(copied["second_returned_bytes"]) is not int or not 0 <= copied["second_returned_bytes"] <= 0xffffffff:
                raise RunError(f"invalid {name} second copy")
            second_ok = not failed(int(second[2:], 16)) and copied["second_returned_bytes"] == size and copied["tail_intact"]
            if not second_ok:
                if any(copied[key] is not None for key in ("verify_hr", "verify_size", "reads_match", "words")):
                    raise RunError(f"incomplete {name} second copy exposed data")
                partial = True
                return {"query": query, "copy": copied}
            if type(copied["reads_match"]) is not bool:
                raise RunError(f"invalid {name} comparison")
            if not copied["reads_match"]:
                if any(copied[key] is not None for key in ("verify_hr", "verify_size", "words")):
                    raise RunError(f"divergent {name} copies exposed data")
                partial = True
                return {"query": query, "copy": copied}
            verify = copied["verify_hr"]
            if not isinstance(verify, str) or HR.fullmatch(verify) is None:
                raise RunError(f"invalid {name} verification query")
            if failed(int(verify[2:], 16)):
                if copied["verify_size"] is not None or words is not None:
                    raise RunError(f"failed {name} verification exposed data")
                partial = True
            elif type(copied["verify_size"]) is not int or not 0 <= copied["verify_size"] <= 0xffffffff:
                raise RunError(f"invalid {name} verification size")
            elif copied["verify_size"] != size:
                if words is not None: raise RunError(f"changed {name} size exposed data")
                partial = True
            elif (not isinstance(words, list) or len(words) != size // 4 or any(
                    type(w) is not int or not 0 <= w <= 0xffffffff for w in words)):
                raise RunError(f"invalid {name} words")
            elif words != expected: partial = True
            return {"query": query, "copy": copied}

        create = take("create_shader", 0)
        if set(create) != {"event", "phase", "hr"} or failed(hresult(create)): raise RunError("shader creation failed")
        for phase in range(3):
            if phase == 1:
                if scenario == "recovery":
                    bad = take("failed_reset", 1); recovery = take("recovery_reset", 1)
                    if failed(hresult(recovery)): raise RunError("recovery Reset failed")
                    if not failed(hresult(bad)): partial = True
                    resets += [bad, recovery]
                else:
                    first = take("first_reset", 1)
                    if failed(hresult(first)): raise RunError("first Reset failed")
                    resets.append(first)
            if phase == 2:
                good = take("successful_reset", 2)
                if failed(hresult(good)): raise RunError("valid Reset failed")
                resets.append(good)
            before = take("binding_before", phase)
            if set(before) != {"event", "phase", "hr", "kind", "other"}: raise RunError("invalid binding record")
            if failed(hresult(before)):
                if before["kind"] is not None or before["other"] is not None: raise RunError("invalid failed binding")
                partial = True
            elif before["kind"] == "handle":
                if before["other"] is not None: raise RunError("invalid handle binding")
            elif before["kind"] == "other":
                if type(before["other"]) is not int or not 0 <= before["other"] <= 0xffffffff:
                    raise RunError("invalid other binding")
            else: raise RunError("invalid binding kind")
            declaration = take_words("declaration", phase, WORDS)
            function = take_words("function", phase, PROGRAM)
            set_shader = take("set_shader", phase)
            if set(set_shader) != {"event", "phase", "hr"}: raise RunError("invalid SetVertexShader record")
            original = not failed(hresult(set_shader))
            if not original:
                fallback = take("fallback_fvf", phase)
                if set(fallback) != {"event", "phase", "hr"} or failed(hresult(fallback)):
                    raise RunError("fallback FVF failed")
                partial = True
            after = take("binding_after", phase)
            if (set(after) != {"event", "phase", "hr", "original", "matches_selected"} or
                    failed(hresult(after)) or after["original"] is not original or after["matches_selected"] is not True):
                raise RunError("selected binding was not confirmed")
            phases.append({"original": original, "before": before, "declaration": declaration, "function": function})
        unbind, delete = take("unbind_shader", 2), take("delete_shader", 2)
        if failed(hresult(unbind)): raise RunError("shader unbind failed")
        if failed(hresult(delete)): partial = True
        if position != len(records): raise RunError("unexpected extra reset records")
        return {"events": records, "parity": parity, "handle": handles[0], "phases": phases,
                "resets": resets, "partial": partial}
    except (OSError, UnicodeError, ValueError) as exc:
        raise RunError(f"invalid fixture log: {exc}") from exc


def inspect_trace(path, observation, enabled, scenario):
    # Validate the exact private bytes parsed below, even if the input file is
    # concurrently modified. The generic UP packet helper only accepts one frame.
    data = Path(path).read_bytes()
    with tempfile.TemporaryDirectory(prefix="t3-reset-trace-") as directory:
        snapshot = Path(directory) / "trace.jsonl"
        snapshot.write_bytes(data)
        trace_summary.summarize(snapshot)
    records = [json.loads(line) for line in data.decode("utf-8").splitlines()]
    capability = records[0].get("declaration_capture")
    if not isinstance(capability, dict) or capability.get("requested") is not enabled:
        raise RunError("declaration capture option differs")
    frames = [record for record in records if record.get("event") == "frame_begin"]
    ends = [record for record in records if record.get("event") == "frame_end"]
    draws = [record for record in records if record.get("event") == "draw"]
    results = {(record["frame"], record["op_id"]): record for record in records if record.get("event") == "draw_result"}
    resets = [record for record in records if record.get("event") == "reset"]
    reset_frames = [1, 1, 2] if scenario == "recovery" else [1, 2]
    events = ["header", "frame_begin", "clear", "clear_result", "draw", "draw_result", "frame_end",
              "frame_begin"] + ["reset"] * (2 if scenario == "recovery" else 1) + [
              "clear", "clear_result", "draw", "draw_result", "frame_end", "frame_begin", "reset",
              "clear", "clear_result", "draw", "draw_result", "frame_end", "capture_end"]
    if (scenario not in SCENARIOS or len(observation["phases"]) != 3 or len(observation["resets"]) != len(reset_frames) or
            [r.get("event") for r in records] != events or
            [f.get("frame") for f in frames] != [0, 1, 2] or len(ends) != 3 or len(draws) != 3 or
            len(resets) != len(reset_frames)):
        raise RunError("fixture did not capture the three expected frames and resets")
    for record in records:
        if record.get("event") == "clear" and (
                type(record.get("flags")) is not int or record["flags"] != 1 or
                type(record.get("color")) is not int or record["color"] != 0xff000000 or
                type(record.get("rect_count")) is not int or record["rect_count"] != 0):
            raise RunError("fixture clear differs")
        if record.get("event") == "clear_result" and record.get("hr") != 0:
            raise RunError("fixture clear failed")
    for frame, record, probe in zip(reset_frames, resets, observation["resets"]):
        hr = record.get("reset_hr")
        if (record.get("frame") != frame or type(hr) is not int or not -(1 << 31) <= hr < (1 << 32) or
                (hr & 0xffffffff) != hresult(probe)):
            raise RunError("trace Reset HRESULT or frame differs")
    for phase, (draw, end, probe) in enumerate(zip(draws, ends, observation["phases"])):
        result = results.get((draw.get("frame"), draw.get("op_id")), {})
        numeric = {"primitive_type": 4, "primitive_count": 1, "vertex_stride": 16,
                   "vertex_shader": observation["handle"] if probe["original"] else FVF}
        if (draw.get("frame") != phase or draw.get("call") != "DrawPrimitiveUP" or
                any(type(draw.get(key)) is not int or draw[key] != value for key, value in numeric.items()) or
                result.get("hr") != 0 or type(end.get("draws")) is not int or end["draws"] != 1 or end.get("present_hr") != 0 or
                end.get("had_reset") is not (phase != 0) or end.get("concurrent_operations") is not False):
            raise RunError("draw or frame differs from fixture")
        if not enabled:
            if "vertex_declaration" in draw or "shader_stable" in result: raise RunError("opt-out exposed declaration")
            continue
        raw = draw.get("vertex_declaration")
        if not isinstance(raw, dict) or result.get("shader_stable") is not True:
            raise RunError("shader observation is missing or unstable")
        if probe["original"] and type(raw.get("generation")) is not int:
            raise RunError("declaration generation is not an integer")
        if probe["original"] and phase == 0:
            if raw != {"status": "candidate", "generation": 1, "words": WORDS}:
                raise RunError("pre-reset candidate differs")
        elif probe["original"]:
            if raw != {"status": "unavailable", "generation": 1, "words": None,
                       "reason": "reset_attempt", "scan_status": "complete"}:
                raise RunError("post-reset declaration reason differs")
        elif raw != {"status": "unknown", "generation": None, "words": None}:
            raise RunError("fallback FVF unexpectedly matched a declaration")


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
    try: output.mkdir()
    except OSError as exc: raise RunError(f"cannot create output directory: {exc}") from exc
    env = {k: v for k, v in os.environ.items() if not k.startswith("T3RENDER_CAPTURE_")}
    for key in ("WINEDLLPATH", "WINEPATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"): env.pop(key, None)
    env.update({"WINEPREFIX": str(bottle), "WINESERVER": str(server),
                "WINEDLLPATH_PREPEND": f"{bottle}/.highball-dlss/wine:{engine}/renderers/d3dmetal-tsshim/wine:{engine}/renderers/d3dmetal/wine",
                "DYLD_FALLBACK_LIBRARY_PATH": f"{frameworks}:{frameworks}/GStreamer.framework/Versions/1.0/lib",
                "DYLD_FALLBACK_FRAMEWORK_PATH": str(frameworks), "WINEDEBUG": "-all,+loaddll"})
    manifest = {"schema": 1, "test": "synthetic_d3d8_shader_reset", "status": "FAIL",
                "fixture_sha256": sha256(fixture), "proxy_sha256": sha256(proxy), "scenarios": {},
                "conclusions": {"valid-only": "inconclusive", "recovery": "inconclusive",
                                "failed_reset_alone": "not_observed"},
                "limitations": ["Synthetic fixture only; no TDS process or game data.",
                                "No device queries or drawing between failed Reset and successful recovery Reset.",
                                "After recovery, observations cannot isolate effects of the failed Reset alone.",
                                "RGB only; no COM alias coverage, native Windows or gameplay validation."]}
    stage = "setup"
    try:
        partial = False
        for scenario in SCENARIOS:
            scenario_dir = output / scenario; scenario_dir.mkdir()
            observations, images, runs = [], [], {}
            for mode in ("baseline", "proxy_off", "proxy_on"):
                stage = f"{scenario}/{mode}"
                directory = scenario_dir / mode; directory.mkdir()
                executable = directory / fixture.name; shutil.copyfile(fixture, executable)
                if sha256(executable) != manifest["fixture_sha256"]: raise RunError("fixture copy differs")
                dll = directory / "d3d8.dll"
                child = env.copy()
                child["WINEDLLOVERRIDES"] = "d3d8=b;nvapi64,nvngx=n,b;winemenubuilder.exe=d"
                trace = scenario_dir / f"{mode}.jsonl"
                if mode != "baseline":
                    shutil.copyfile(proxy, dll)
                    if sha256(dll) != manifest["proxy_sha256"]: raise RunError("proxy copy differs")
                    child.update({"WINEDLLOVERRIDES": "d3d8=n,b;nvapi64,nvngx=n,b;winemenubuilder.exe=d",
                                  "T3RENDER_CAPTURE_FRAME": "0", "T3RENDER_CAPTURE_FRAMES": "3",
                                  "T3RENDER_CAPTURE_PATH": windows_path(trace)})
                    if mode == "proxy_on": child["T3RENDER_CAPTURE_DECLARATIONS"] = "1"
                log = scenario_dir / f"{mode}.log"
                outputs = [scenario_dir / f"{mode}_{i}.ppm" for i in range(3)]
                args = [wine, executable]
                for phase, path in enumerate(outputs): args += [f"--out{phase}", windows_path(path)]
                args += ["--scenario", scenario]
                run_stage(stage, args, directory, child, log, timeout)
                observed = inspect_log(log, scenario); observations.append(observed)
                check_stage_loader(log, bottle, dll if mode != "baseline" else None)
                mode_images = [inspect_image(path, observed["phases"][i]["original"]) for i, path in enumerate(outputs)]
                images.append(mode_images)
                if mode != "baseline": inspect_trace(trace, observed, mode == "proxy_on", scenario)
                runs[mode] = {"log_sha256": sha256(log), "ppm_sha256": [sha256(p) for p in outputs],
                              "trace_sha256": sha256(trace) if mode != "baseline" else None,
                              "events": observed["events"],
                              "original_pixels_preserved": original_pixels_preserved(mode_images, observed["phases"])}
            stage = f"{scenario}/compare"
            for current in (1, 2):
                if (observations[current]["events"] != observations[0]["events"] or
                        observations[current]["parity"] != observations[0]["parity"] or images[current] != images[0]):
                    raise RunError("baseline and proxy HRESULT/state observations or RGB pixels differ")
            scenario_partial = any(item["partial"] for item in observations) or any(
                not run["original_pixels_preserved"] for run in runs.values())
            partial = partial or scenario_partial
            manifest["scenarios"][scenario] = {"status": "PARTIAL" if scenario_partial else "PASS", "runs": runs}
            if not scenario_partial: manifest["conclusions"][scenario] = "observed_usable_in_fixture"
        manifest["status"] = "PARTIAL" if partial else "PASS"
    except (RunError, OSError, ValueError, trace_summary.TraceError) as exc:
        manifest["failed_stage"] = stage; manifest["error"] = str(exc)
    with (output / "result.json").open("x", encoding="utf-8") as dest:
        json.dump(manifest, dest, indent=2); dest.write("\n")
    if manifest["status"] == "FAIL": raise RunError(f"{stage}: {manifest['error']}; partial output: {output}")
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("engine", "bottle", "fixture", "proxy", "output-dir"): parser.add_argument("--" + option, required=True)
    parser.add_argument("--timeout", type=float, default=45)
    args = parser.parse_args(argv)
    try: result = execute(args.engine, args.bottle, args.fixture, args.proxy, args.output_dir, args.timeout)
    except (RunError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr); return 1
    print(f"{result['status']}: synthetic Reset fixture; results: {args.output_dir}")
    return 0 if result["status"] == "PASS" else 3


if __name__ == "__main__": raise SystemExit(main())
