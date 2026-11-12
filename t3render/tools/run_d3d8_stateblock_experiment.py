#!/usr/bin/env python3
"""Compare state-block cases using a separately built, unsafe test-only proxy.

Never use this DLL in TDS. The experiment does not close COM aliases or prove
the mutation-window policy safe. It runs synthetic processes only.
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
from run_winepath_proxy_probe import check_loader, loaded_d3d8
from run_d3d8_stateblock_fixture import inspect_buffer_range, inspect_image
import trace_summary
import trace_to_up_packet as trace


CASES = ("control", "create", "record", "apply", "capture", "delete", "lifetime", "reacquire")


def check_stage_loader(log, bottle, expected_dll=None):
    if expected_dll is None:
        return check_loader(log)
    native = [name for name, kind in loaded_d3d8(log) if kind == "native"]
    if len(native) != 1:
        raise RunError("loader did not report exactly one native D3D8 proxy")
    # Wine may print any DOS drive mapping to the same Unix path, e.g. both
    # Z:\\Volumes\\Personal and D:\\ can refer to the workspace. Resolve the
    # logged drive through this bottle and require the *same file*, not just a
    # matching basename or hash. Do not accept relative or device paths.
    match = re.fullmatch(r"([A-Za-z]):\\([^/<>:\"|?*\x00-\x1f]+)", native[0])
    if not match:
        raise RunError("loader reported an invalid native D3D8 path")
    parts = match.group(2).split("\\")
    drive = Path(bottle) / "dosdevices" / (match.group(1).lower() + ":")
    if (any(part in ("", ".", "..") for part in parts) or not drive.is_symlink()):
        raise RunError("loader reported an invalid native D3D8 drive mapping")
    try:
        same_file = os.path.samefile(drive.joinpath(*parts), expected_dll)
    except OSError:
        same_file = False
    if not same_file:
        raise RunError("native D3D8 proxy is not the requested file")
    return check_loader(log, expected_native=native[0])


def inspect_log(path, case):
    lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    if case == "lifetime":
        markers = ["state case_lifetime=PASS", "state result=PASS"]
    elif case == "reacquire":
        markers = ["state stateblock_retains_released_buffers=PASS", "state case_reacquire=PASS", "state result=PASS"]
    else:
        markers = [f"state {case}=PASS", "state result=PASS"]
    if [line for line in lines if line.startswith("state ")] != markers:
        raise RunError(f"state-block markers differ for {case}")


def inspect_trace(path, case, experiment):
    try:
        records = trace.validated_records(path)
        if [r.get("event") for r in records] != ["header", "frame_begin", "clear", "clear_result", "draw",
                                                   "draw_result", "frame_end", "capture_end"]:
            raise RunError("expected one clear and one indexed draw")
        header, begin, clear, cleared, draw, result, end, _ = records
        cap = header.get("buffer_capture")
        number = trace.integer
        if (not isinstance(cap, dict) or cap.get("requested") is not True or
                number(begin.get("frame")) != 0 or number(clear.get("flags")) != 1 or
                number(clear.get("color")) != 0xff000000 or not trace.hresult_succeeded(cleared.get("hr")) or
                not trace.hresult_succeeded(result.get("hr")) or not trace.hresult_succeeded(end.get("present_hr")) or
                end.get("draws") != 1 or end.get("had_reset") is not False or
                end.get("concurrent_operations") is not False):
            raise RunError("missing opt-in or unsuccessful fixture frame")
        expected = {"primitive_type": 4, "primitive_count": 1, "min_vertex_index": 0,
                    "num_vertices": 3, "start_index": 0, "vertex_shader": 0x44}
        if draw.get("call") != "DrawIndexedPrimitive" or any(number(draw.get(k)) != v for k, v in expected.items()):
            raise RunError("unexpected draw parameters")
        streams = draw.get("streams")
        if not isinstance(streams, list): raise RunError("missing streams")
        stream0 = [v for v in streams if isinstance(v, dict) and number(v.get("stream")) == 0]
        if len(stream0) != 1: raise RunError("stream zero is missing or ambiguous")
        stream, index = stream0[0], draw.get("indices")
        retained = case in ("lifetime", "reacquire")
        for binding, size in ((stream, 80 if retained else 60), (index, 8 if retained else 6)):
            if not isinstance(binding, dict) or not trace.hresult_succeeded(binding.get("hr")):
                raise RunError("buffer getter failed")
            desc = binding.get("description")
            if (not isinstance(desc, dict) or number(desc.get("usage")) != 8 or number(desc.get("pool")) != 1 or
                    number(desc.get("size")) != size or not isinstance(binding.get("buffer"), str) or
                    re.fullmatch(r"0x[0-9a-fA-F]+", binding["buffer"]) is None or
                    int(binding["buffer"], 16) == 0):
                raise RunError("expected MANAGED WRITEONLY buffers with known sizes")
        if (number(stream.get("stride")) != 20 or number(index.get("base_vertex_index")) != 0 or
                number(stream["description"].get("fvf")) != 0x44 or number(index["description"].get("format")) != 101):
            raise RunError("buffer binding or format differs")
        stable = case == "control" or (experiment and case != "reacquire")
        status = ("captured" if stable and case != "lifetime" else "unknown" if stable else "window_unavailable")
        if result.get("buffer_stable") is not stable:
            raise RunError(f"unexpected stable window for {case}: expected {stable}")
        data = draw.get("buffer_data")
        if not isinstance(data, dict) or data.get("source") != "application_lock_shadow":
            raise RunError("missing buffer-shadow data")
        vertices = b"".join(struct.pack("<4fI", *v) for v in (
            (25.6, 217.6, 0, 1, 0xffff0000), (128, 25.6, 0, 1, 0xffff0000),
            (230.4, 217.6, 0, 1, 0xffff0000)))
        expected_bytes = {"vertex": vertices.hex(), "index": struct.pack("<3H", 0, 1, 2).hex()}
        for name, binding, size in (("vertex", stream, 60), ("index", index, 6)):
            part = data.get(name)
            inspect_buffer_range(name, part)
            if (not isinstance(part, dict) or part.get("status") != status or part.get("encoding") != "hex" or
                    number(part.get("byte_count")) != size or part.get("resource") != binding.get("buffer") or
                    part.get("data") != (expected_bytes[name] if status == "captured" else None)):
                raise RunError(f"unexpected {name} observation for {case}: expected {status}")
        return {"buffer_stable": stable, "vertex_status": status, "index_status": status}
    except (OSError, UnicodeError, ValueError, trace_summary.TraceError) as exc:
        raise RunError(f"invalid state-block experiment trace: {exc}") from exc


def execute(engine, bottle, fixture, production, experiment, output, timeout):
    engine, bottle, fixture, production, experiment, output = map(valid_path,
        (engine, bottle, fixture, production, experiment, output))
    if any(not str(p).isascii() or '"' in str(p) for p in (fixture, production, experiment, output)):
        raise RunError("fixture, DLL and output paths must be ASCII and contain no double quotes")
    wine, server, frameworks = engine / "engine/bin/wine", engine / "engine/bin/wineserver", engine / "frameworks"
    if not all(p.is_file() and os.access(p, os.X_OK) for p in (wine, server)) or not frameworks.is_dir():
        raise RunError("invalid Highball engine")
    z = bottle / "dosdevices/z:"
    if not bottle.is_dir() or not z.is_symlink() or os.readlink(z) != "/": raise RunError("bottle must map Z: to /")
    check_pe32(fixture, False); check_pe32(production, True); check_pe32(experiment, True)
    if not math.isfinite(timeout) or timeout <= 0: raise RunError("timeout must be positive and finite")
    for protected in (engine, bottle, fixture.parent, production.parent, experiment.parent):
        if output.resolve() == protected.resolve() or protected.resolve() in output.resolve().parents:
            raise RunError("output must be outside engine, bottle and binary directories")
    fixture_sha, production_sha, experiment_sha = map(sha256, (fixture, production, experiment))
    if production_sha == experiment_sha: raise RunError("test-only and production proxy DLLs must differ")
    try: output.mkdir()
    except OSError as exc: raise RunError(f"cannot create output directory: {exc}") from exc
    env = {k: v for k, v in os.environ.items() if not k.startswith("T3RENDER_CAPTURE_")}
    for key in ("WINEDLLPATH", "WINEPATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"): env.pop(key, None)
    env.update({"WINEPREFIX": str(bottle), "WINESERVER": str(server),
                "WINEDLLPATH_PREPEND": f"{bottle}/.highball-dlss/wine:{engine}/renderers/d3dmetal-tsshim/wine:{engine}/renderers/d3dmetal/wine",
                "DYLD_FALLBACK_LIBRARY_PATH": f"{frameworks}:{frameworks}/GStreamer.framework/Versions/1.0/lib",
                "DYLD_FALLBACK_FRAMEWORK_PATH": str(frameworks), "WINEDEBUG": "-all,+loaddll"})
    result = {"schema": 1, "test": "synthetic_stateblock_mutation_window_experiment", "status": "FAIL",
              "fixture_sha256": fixture_sha, "production_proxy_sha256": production_sha,
              "test_only_proxy_sha256": experiment_sha, "cases": {},
              "limitations": ["Test-only DLL; never deploy it to TDS.",
                              "No game data or wrapped buffer passed through native device aliases.",
                              "State-block mutation windows do not establish general COM or concurrent lock safety.",
                              "RGB PPM equality does not establish alpha or general renderer parity."]}
    stage = "setup"
    try:
        for case in CASES:
            images = []
            case_dir = output / case; case_dir.mkdir()
            case_result = {}
            for mode in ("baseline", "production", "experiment"):
                stage = f"{case}/{mode}"
                directory = case_dir / mode; directory.mkdir()
                exe = directory / fixture.name; shutil.copyfile(fixture, exe)
                if sha256(exe) != fixture_sha: raise RunError("fixture copy differs")
                e = env.copy()
                e["WINEDLLOVERRIDES"] = "d3d8=b;nvapi64,nvngx=n,b;winemenubuilder.exe=d"
                dll = directory / "d3d8.dll"
                if mode != "baseline":
                    source = production if mode == "production" else experiment
                    shutil.copyfile(source, dll)
                    if sha256(dll) != (production_sha if mode == "production" else experiment_sha):
                        raise RunError("proxy copy differs")
                    e.update({"WINEDLLOVERRIDES": "d3d8=n,b;nvapi64,nvngx=n,b;winemenubuilder.exe=d",
                              "T3RENDER_CAPTURE_FRAME": "0", "T3RENDER_CAPTURE_BUFFERS": "1",
                              "T3RENDER_CAPTURE_PATH": windows_path(directory / "capture.jsonl")})
                log, ppm = directory / "run.log", directory / "image.ppm"
                run_stage(stage, [wine, exe, "--case", case, "--output", windows_path(ppm)], directory, e, log, timeout)
                inspect_log(log, case)
                check_stage_loader(log, bottle, dll if mode != "baseline" else None)
                images.append(inspect_image(ppm))
                observation = None
                if mode != "baseline": observation = inspect_trace(directory / "capture.jsonl", case, mode == "experiment")
                case_result[mode] = {"ppm_sha256": sha256(ppm), "log_sha256": sha256(log), "shadow": observation}
            if not all(image == images[0] for image in images[1:]): raise RunError(f"{case}: RGB PPMs differ")
            result["cases"][case] = case_result
        result["status"] = "PASS"
    except (RunError, OSError, ValueError) as exc:
        result["failed_stage"] = stage; result["error"] = str(exc)
    try:
        with (output / "result.json").open("x", encoding="utf-8") as destination:
            json.dump(result, destination, indent=2); destination.write("\n")
    except OSError as exc: raise RunError(f"cannot write result manifest: {exc}") from exc
    if result["status"] != "PASS": raise RunError(f"{stage}: {result['error']}; partial output: {output}")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("engine", "bottle", "fixture", "production", "experiment", "output-dir"):
        parser.add_argument("--" + option, required=True)
    parser.add_argument("--fixture-only", action="store_true", required=True,
                        help="attest the experimental DLL will be used only in this isolated fixture")
    parser.add_argument("--timeout", type=float, default=45)
    args = parser.parse_args(argv)
    try: execute(args.engine, args.bottle, args.fixture, args.production, args.experiment, args.output_dir, args.timeout)
    except RunError as exc:
        print(f"error: {exc}", file=sys.stderr); return 1
    print(f"PASS: {len(CASES)} isolated state-block cases; results: {args.output_dir}")
    return 0


if __name__ == "__main__": raise SystemExit(main())
