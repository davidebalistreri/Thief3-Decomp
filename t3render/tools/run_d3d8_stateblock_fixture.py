#!/usr/bin/env python3
"""Compare isolated WRITEONLY buffer/state-block behavior with and without the proxy.

The real proxy still invalidates buffer observations on every state-block call.
This fixture never launches TDS and does not demonstrate safe COM alias coverage.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import sys

from run_synthetic_up_replay import RunError, check_pe32, run_stage, sha256, valid_path, windows_path
import trace_summary
import trace_to_up_packet as trace


HEADER = b"P6\n256 256\n255\n"
PPM_SIZE = len(HEADER) + 256 * 256 * 3
MARKERS = ["state before_apply_all=PASS", "state apply_all_restores_a=PASS",
           "state delete_preserves_bound_a=PASS", "state recording_preserves_bound_a=PASS",
           "state recorded_restores_b=PASS", "state capture_restores_a=PASS",
           "state stateblock_retains_released_buffers=PASS",
           "state stateblock_retains_released_buffers=PASS", "state result=PASS"]


def inspect_log(path):
    lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    if [line for line in lines if line.startswith("state ")] != MARKERS:
        raise RunError("fixture state-block result markers are missing, altered or out of order")


def inspect_image(path):
    with Path(path).open("rb") as source:
        data = source.read(PPM_SIZE + 1)
    if len(data) != PPM_SIZE or not data.startswith(HEADER):
        raise RunError("fixture image is not a complete 256x256 P6 PPM")
    pixels = data[len(HEADER):]
    center = (128 * 256 + 128) * 3
    if (pixels[:3] != b"\x00\x00\x00" or pixels[center:center + 3] != b"\xff\x00\x00" or
            sum(pixels[i:i+3] == b"\xff\x00\x00" for i in range(0, len(pixels), 3)) <= 1000):
        raise RunError("fixture image is missing the red triangle or black background")
    return data


def inspect_buffer_range(component, value):
    # Both state-block fixtures draw the first three 20-byte vertices and
    # three 16-bit indices from stream zero, even for oversized resources.
    expected = {
        "vertex": {"stream": 0, "first_vertex": 0, "vertex_count": 3, "stride": 20, "byte_offset": 0},
        "index": {"index_count": 3, "index_format": 101, "byte_offset": 0},
    }.get(component)
    if (expected is None or not isinstance(value, dict) or
            any(trace.integer(value.get(key)) != number for key, number in expected.items())):
        raise RunError(f"unexpected {component} buffer range")


def inspect_trace(path):
    try:
        records = trace.validated_records(path)
        if [x.get("event") for x in records] != ["header", "frame_begin", "clear", "clear_result", "draw",
                                                   "draw_result", "frame_end", "capture_end"]:
            raise RunError("fixture requires a single ordered clear and indexed draw")
        header, begin, clear, clear_result, draw, result, end, _ = records
        integer = trace.integer
        capability = header.get("buffer_capture")
        if (not isinstance(capability, dict) or capability.get("requested") is not True or
                integer(begin.get("frame")) != 0 or integer(clear.get("flags")) != 1 or
                integer(clear.get("color")) != 0xff000000 or
                not trace.hresult_succeeded(clear_result.get("hr")) or
                not trace.hresult_succeeded(result.get("hr")) or
                not trace.hresult_succeeded(end.get("present_hr")) or
                end.get("draws") != 1 or end.get("had_reset") is not False or
                end.get("concurrent_operations") is not False):
            raise RunError("fixture frame was unsuccessful or buffer opt-in was not recorded")
        expected = {"primitive_type": 4, "primitive_count": 1, "min_vertex_index": 0,
                    "num_vertices": 3, "start_index": 0, "vertex_shader": 0x44}
        if draw.get("call") != "DrawIndexedPrimitive" or any(integer(draw.get(k)) != v for k, v in expected.items()):
            raise RunError("fixture indexed draw does not match")
        streams = draw.get("streams")
        if not isinstance(streams, list): raise RunError("fixture stream data missing")
        zero = [s for s in streams if isinstance(s, dict) and integer(s.get("stream")) == 0]
        if len(zero) != 1: raise RunError("fixture stream zero not unique")
        stream, index = zero[0], draw.get("indices")
        for binding, size in ((stream, 60), (index, 6)):
            if not isinstance(binding, dict) or not trace.hresult_succeeded(binding.get("hr")):
                raise RunError("fixture buffer getter failed")
            desc = binding.get("description")
            if (not isinstance(desc, dict) or integer(desc.get("usage")) != 8 or integer(desc.get("pool")) != 1 or
                    integer(desc.get("size")) != size or not isinstance(binding.get("buffer"), str) or
                    re.fullmatch(r"0x[0-9a-fA-F]+", binding["buffer"]) is None or
                    int(binding["buffer"], 16) == 0):
                raise RunError("fixture requires MANAGED WRITEONLY buffers with expected sizes")
        if (integer(stream.get("stride")) != 20 or integer(index.get("base_vertex_index")) != 0 or
                integer(stream["description"].get("fvf")) != 0x44 or
                integer(index["description"].get("format")) != 101):
            raise RunError("fixture buffer binding differs")
        data = draw.get("buffer_data")
        if not isinstance(data, dict) or data.get("source") != "application_lock_shadow" or result.get("buffer_stable") is not False:
            raise RunError("proxy did not conservatively mark its state-block window unavailable")
        for component, binding, size in (("vertex", stream, 60), ("index", index, 6)):
            value = data.get(component)
            inspect_buffer_range(component, value)
            if (not isinstance(value, dict) or value.get("status") != "window_unavailable" or
                    value.get("data") is not None or value.get("encoding") != "hex" or
                    integer(value.get("byte_count")) != size or
                    value.get("resource") != binding.get("buffer")):
                raise RunError(f"proxy did not conservatively mark the {component} bytes unavailable")
    except (OSError, UnicodeError, ValueError, trace_summary.TraceError) as exc:
        raise RunError(f"invalid state-block fixture trace: {exc}") from exc


def execute(engine, bottle, fixture, proxy, output, timeout):
    engine, bottle, fixture, proxy, output = map(valid_path, (engine, bottle, fixture, proxy, output))
    wine, server, frameworks = engine / "engine/bin/wine", engine / "engine/bin/wineserver", engine / "frameworks"
    if not all(p.is_file() and os.access(p, os.X_OK) for p in (wine, server)) or not frameworks.is_dir():
        raise RunError("invalid Highball engine")
    z = bottle / "dosdevices/z:"
    if not bottle.is_dir() or not z.is_symlink() or os.readlink(z) != "/":
        raise RunError("bottle must map Z: to /")
    check_pe32(fixture, False); check_pe32(proxy, True)
    if not math.isfinite(timeout) or timeout <= 0: raise RunError("timeout must be positive and finite")
    for protected in (engine, bottle, fixture.parent, proxy.parent):
        if output.resolve() == protected.resolve() or protected.resolve() in output.resolve().parents:
            raise RunError("output must be outside engine, bottle and binary directories")
    try: output.mkdir()
    except OSError as exc: raise RunError(f"cannot create output directory: {exc}") from exc
    env = {k: v for k, v in os.environ.items() if not k.startswith("T3RENDER_CAPTURE_")}
    for key in ("WINEDLLPATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"): env.pop(key, None)
    env.update({"WINEPREFIX": str(bottle), "WINESERVER": str(server),
                "WINEDLLPATH_PREPEND": f"{bottle}/.highball-dlss/wine:{engine}/renderers/d3dmetal-tsshim/wine:{engine}/renderers/d3dmetal/wine",
                "DYLD_FALLBACK_LIBRARY_PATH": f"{frameworks}:{frameworks}/GStreamer.framework/Versions/1.0/lib",
                "DYLD_FALLBACK_FRAMEWORK_PATH": str(frameworks), "WINEDEBUG": "-all"})
    result = {"schema": 1, "test": "synthetic_d3d8_stateblock_writeonly_buffers", "status": "FAIL",
              "fixture_sha256": sha256(fixture), "proxy_sha256": sha256(proxy), "runs": {},
              "limitations": ["Synthetic D3D8 state-block fixture; no TDS process or game data.",
                              "Buffer observations are intentionally unavailable after state-block calls.",
                              "Resource/device alias closure, concurrent calls, reset and Windows are not tested.",
                              "Image comparison covers RGB, not alpha."]}
    stage = "setup"
    try:
        images = []
        for stage in ("baseline", "proxy"):
            directory = output / stage
            directory.mkdir()
            exe = directory / fixture.name
            shutil.copyfile(fixture, exe)
            stage_env = env.copy()
            stage_env["WINEDLLOVERRIDES"] = "d3d8=b;nvapi64,nvngx=n,b;winemenubuilder.exe=d"
            if stage == "proxy":
                shutil.copyfile(proxy, directory / "d3d8.dll")
                stage_env.update({"WINEDLLOVERRIDES": "d3d8=n,b;nvapi64,nvngx=n,b;winemenubuilder.exe=d",
                                  "T3RENDER_CAPTURE_FRAME": "0", "T3RENDER_CAPTURE_BUFFERS": "1",
                                  "T3RENDER_CAPTURE_PATH": windows_path(output / "capture.jsonl")})
            log, ppm = output / (stage + ".log"), output / (stage + ".ppm")
            run_stage(stage, [wine, exe, "--output", windows_path(ppm)], directory, stage_env, log, timeout)
            inspect_log(log)
            images.append(inspect_image(ppm))
            if stage == "proxy": inspect_trace(output / "capture.jsonl")
            result["runs"][stage] = {"log_sha256": sha256(log), "ppm_sha256": sha256(ppm)}
        stage = "compare"
        if images[0] != images[1]: raise RunError("baseline and proxy RGB PPM bytes differ")
        result["trace_sha256"] = sha256(output / "capture.jsonl")
        result["status"] = "PASS"
    except (RunError, OSError, ValueError) as exc:
        result["failed_stage"] = stage
        result["error"] = str(exc)
    try:
        with (output / "result.json").open("x", encoding="utf-8") as destination:
            json.dump(result, destination, indent=2); destination.write("\n")
    except OSError as exc: raise RunError(f"cannot write result manifest: {exc}") from exc
    if result["status"] != "PASS": raise RunError(f"{stage}: {result['error']}; partial output: {output}")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("engine", "bottle", "fixture", "proxy", "output-dir"): parser.add_argument("--" + option, required=True)
    parser.add_argument("--timeout", type=float, default=45)
    args = parser.parse_args(argv)
    try: result = execute(args.engine, args.bottle, args.fixture, args.proxy, args.output_dir, args.timeout)
    except RunError as exc:
        print(f"error: {exc}", file=sys.stderr); return 1
    print(f"PASS: identical RGB PPM SHA-256 {result['runs']['baseline']['ppm_sha256']}; results: {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
