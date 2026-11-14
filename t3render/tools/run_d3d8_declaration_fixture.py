#!/usr/bin/env python3
"""Compare a synthetic D3D8 declaration fixture under Highball, without TDS."""
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

HEADER = b"P6\n256 256\n255\n"
WORDS = [0x20000000, 0x40020000, 0x40040005, 0xffffffff]
PROGRAM_WORDS = [0xfffe0101, 1, 0xc00f0000, 0x90e40000, 1, 0xd00f0000, 0x90e40005, 0xffff]
EXPECTED_CALLS = ["GetAdapterDisplayMode", "CreateDevice", "SetWorld", "SetView", "SetProjection", "Lighting",
                  "Cull", "Depth", "Texture", "ColorOp", "ColorArg", "Stage1", "PixelShader", "Clear",
                  "BeginScene", "Create1", "Set1", "Get1", "Draw1", "Restore1", "Delete1",
                  "Create2", "Set2", "Get2", "Draw2", "Restore2", "Delete2", "EndScene",
                  "GetBackBuffer", "GetDesc", "LockRect", "UnlockRect", "Present"]


def inspect_log(path):
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    parity = [line for line in lines if line.startswith("parity ")]
    if len(parity) != len(EXPECTED_CALLS):
        raise RunError("missing or duplicate HRESULT results in fixture log")
    for name, line in zip(EXPECTED_CALLS, parity):
        match = re.fullmatch(r"parity (\w+) hr=([0-9a-fA-F]{8})", line)
        if not match or match.group(1) != name or int(match.group(2), 16) & 0x80000000:
            raise RunError(f"fixture HRESULT failure or unexpected order: {line}")
    handles = [line for line in lines if line.startswith("handle ")]
    values = []
    for index, line in enumerate(handles):
        match = re.fullmatch(r"handle (\d+)=([0-9a-fA-F]{8})", line)
        if not match or int(match.group(1)) != index or int(match.group(2), 16) == 0:
            raise RunError(f"invalid fixture shader handle: {line}")
        values.append(int(match.group(2), 16))
    if len(values) != 2 or [line for line in lines if line == "fixture result=PASS"] != ["fixture result=PASS"]:
        raise RunError("fixture did not finish both shader lifetimes")
    return parity, values


def inspect_image(path):
    with path.open("rb") as source:
        image = source.read(len(HEADER) + 256 * 256 * 3 + 1)
    if len(image) != len(HEADER) + 256 * 256 * 3 or not image.startswith(HEADER):
        raise RunError("fixture image is not a complete 256x256 P6 PPM")
    pixels = image[len(HEADER):]
    colors = [pixels[i:i + 3] for i in range(0, len(pixels), 3)]
    if colors[0] != b"\0\0\0" or colors[128 * 256 + 64] != b"\xff\0\0" or \
            colors[128 * 256 + 192] != b"\0\xff\0" or colors.count(b"\xff\0\0") < 1000 or \
            colors.count(b"\0\xff\0") < 1000:
        raise RunError("fixture image is missing the red/green triangles")
    return image


def inspect_trace(path, handles, enabled, program_capture=False, mode="program", experimental_combined=False):
    trace_summary.summarize(path)
    records = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    expected_events = ["header", "frame_begin", "clear", "clear_result", "draw", "draw_result",
                       "draw", "draw_result", "frame_end", "capture_end"]
    if [item.get("event") for item in records] != expected_events:
        raise RunError("unexpected capture event order")
    header, _, _, clear_result, *tail = records
    draw1, result1, draw2, result2, end, _ = tail
    capability = header.get("declaration_capture")
    if not isinstance(capability, dict) or capability.get("requested") is not enabled:
        raise RunError("declaration opt-in header does not match the run")
    program_capability = header.get("vertex_program_capture")
    if program_capture:
        if program_capability != {"requested": True, "words_per_observation": 1024, "words_per_frame": 262144}:
            raise RunError("vertex-program opt-in header does not match the run")
    elif program_capability is not None:
        raise RunError("unexpected vertex-program capability")
    if (clear_result.get("hr") != 0 or end.get("present_hr") != 0 or end.get("draws") != 2 or
            end.get("had_reset") is not False or end.get("concurrent_operations") is not False):
        raise RunError("capture did not complete a clean two-draw frame")
    for generation, (draw, result, handle) in enumerate(zip((draw1, draw2), (result1, result2), handles), 1):
        fields = {"primitive_type": 4, "primitive_count": 1, "vertex_stride": 16, "vertex_shader": handle}
        if (draw.get("call") != "DrawPrimitiveUP" or any(type(draw.get(k)) is not int or draw[k] != v
                                                           for k, v in fields.items()) or result.get("hr") != 0 or
                result.get("op_id") != draw.get("op_id")):
            raise RunError("draw/result/handle mismatch")
        observable = not experimental_combined or generation == 1
        if enabled and observable:
            declaration = draw.get("vertex_declaration")
            if (result.get("shader_stable") is not True or not isinstance(declaration, dict) or
                    type(declaration.get("generation")) is not int or declaration != {
                        "status": "candidate", "generation": generation, "words": WORDS}):
                raise RunError("declaration observation was not paired and stable")
        elif enabled and experimental_combined:
            declaration = draw.get("vertex_declaration")
            if result.get("shader_stable") is not False or not isinstance(declaration, dict) or \
                    declaration.get("status") != "unsafe" or declaration.get("words") is not None:
                raise RunError("combined facade must fail closed after the first shader deletion")
        elif "vertex_declaration" in draw or (not experimental_combined and "shader_stable" in result):
            raise RunError("opt-out unexpectedly produced declaration observations")
        elif experimental_combined and result.get("shader_stable") is not (generation == 1):
            raise RunError("combined facade shader window did not fail closed after deletion")
        version = "vs_1_0" if mode == "program-10" else "vs_1_1"
        words = ([0xfffe0100] + PROGRAM_WORDS[1:]) if mode == "program-10" else PROGRAM_WORDS
        expected_program = {"status": "candidate", "version": version, "generation": generation, "words": words}
        if enabled and observable and program_capture and mode in ("program", "program-10"):
            if draw.get("vertex_program") != expected_program:
                raise RunError("vertex program did not match the fixture input")
        elif "vertex_program" in draw:
            raise RunError("unexpected vertex-program observation")


def execute(engine, bottle, fixture, proxy, output, timeout, mode, program_capture=False, experimental_combined=False):
    engine, bottle, fixture, proxy, output = map(valid_path, (engine, bottle, fixture, proxy, output))
    wine, server, frameworks = engine / "engine/bin/wine", engine / "engine/bin/wineserver", engine / "frameworks"
    if not all(p.is_file() and os.access(p, os.X_OK) for p in (wine, server)) or not frameworks.is_dir():
        raise RunError("invalid Highball engine")
    z = bottle / "dosdevices/z:"
    if not bottle.is_dir() or not z.is_symlink() or os.readlink(z) != "/":
        raise RunError("bottle must map Z: to /")
    check_pe32(fixture, False)
    check_pe32(proxy, True)
    if not math.isfinite(timeout) or timeout <= 0:
        raise RunError("timeout must be positive and finite")
    for protected in (engine, bottle, fixture.parent, proxy.parent):
        if output.resolve() == protected.resolve() or protected.resolve() in output.resolve().parents:
            raise RunError("output must be outside engine, bottle and binary directories")
    try:
        output.mkdir()
    except OSError as exc:
        raise RunError(f"cannot create output directory: {exc}") from exc
    env = {k: v for k, v in os.environ.items() if not k.startswith(("T3RENDER_CAPTURE_", "T3RENDER_EXPERIMENTAL_"))}
    for key in ("WINEDLLPATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"):
        env.pop(key, None)
    env.update({"WINEPREFIX": str(bottle), "WINESERVER": str(server),
                "WINEDLLPATH_PREPEND": f"{bottle}/.highball-dlss/wine:{engine}/renderers/d3dmetal-tsshim/wine:{engine}/renderers/d3dmetal/wine",
                "DYLD_FALLBACK_LIBRARY_PATH": f"{frameworks}:{frameworks}/GStreamer.framework/Versions/1.0/lib",
                "DYLD_FALLBACK_FRAMEWORK_PATH": str(frameworks), "WINEDEBUG": "-all,+loaddll"})
    manifest = {"schema": 1, "test": "synthetic_d3d8_vertex_declaration", "mode": mode, "status": "FAIL",
                "vertex_program_capture": program_capture, "experimental_combined": experimental_combined,
                "fixture_sha256": sha256(fixture), "proxy_sha256": sha256(proxy), "runs": {},
                "limitations": ["Synthetic x86 D3D8 fixture; no TDS process, assets or game frame.",
                                "Two UP draws, one declaration layout, no buffer wrappers or state blocks.",
                                "RGB only; no alpha, native Windows, COM alias or concurrent mutation validation.",
                                "A stable observation is a candidate, not proof of GPU input or replayability."]}
    stage = "setup"
    try:
        images, parity = [], []
        for stage in ("baseline", "proxy_off", "proxy_on"):
            directory = output / stage
            directory.mkdir()
            exe = directory / fixture.name
            shutil.copyfile(fixture, exe)
            stage_env = env.copy()
            stage_env["WINEDLLOVERRIDES"] = "d3d8=b;nvapi64,nvngx=n,b;winemenubuilder.exe=d"
            trace = output / f"{stage}.jsonl"
            if stage != "baseline":
                shutil.copyfile(proxy, directory / "d3d8.dll")
                stage_env.update({"WINEDLLOVERRIDES": "d3d8=n,b;nvapi64,nvngx=n,b;winemenubuilder.exe=d",
                                  "T3RENDER_CAPTURE_FRAME": "0", "T3RENDER_CAPTURE_PATH": windows_path(trace)})
                if experimental_combined:
                    stage_env["T3RENDER_EXPERIMENTAL_COMBINED"] = "1"
                if stage == "proxy_on":
                    stage_env["T3RENDER_CAPTURE_DECLARATIONS"] = "1"
                    if program_capture:
                        stage_env["T3RENDER_CAPTURE_VERTEX_PROGRAMS"] = "1"
            log, ppm = output / f"{stage}.log", output / f"{stage}.ppm"
            run_stage(stage, [wine, exe, "--output", windows_path(ppm), "--mode", mode], directory, stage_env, log, timeout)
            current_parity, handles = inspect_log(log)
            images.append(inspect_image(ppm))
            parity.append(current_parity)
            if stage != "baseline":
                inspect_trace(trace, handles, stage == "proxy_on", program_capture and stage == "proxy_on", mode,
                              experimental_combined)
            manifest["runs"][stage] = {"log_sha256": sha256(log), "ppm_sha256": sha256(ppm),
                                       "trace_sha256": sha256(trace) if stage != "baseline" else None}
        stage = "compare"
        if images[0] != images[1] or images[0] != images[2]:
            raise RunError("RGB PPM bytes differ")
        if parity[0] != parity[1] or parity[0] != parity[2]:
            raise RunError("D3D8 HRESULT observations differ")
        manifest["status"] = "PASS"
    except (RunError, OSError, ValueError, trace_summary.TraceError) as exc:
        manifest["failed_stage"] = stage
        manifest["error"] = str(exc)
    with (output / "result.json").open("x", encoding="utf-8") as dest:
        json.dump(manifest, dest, indent=2)
        dest.write("\n")
    if manifest["status"] != "PASS":
        raise RunError(f"{stage}: {manifest['error']}; partial output: {output}")
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("engine", "bottle", "fixture", "proxy", "output-dir"):
        parser.add_argument("--" + option, required=True)
    parser.add_argument("--mode", choices=("program", "program-10", "declaration-only"), default="program")
    parser.add_argument("--vertex-programs", action="store_true", help="observe the fixture's VS 1.x program")
    parser.add_argument("--experimental-combined", action="store_true", help="activate the supplied combined facade proxy")
    parser.add_argument("--timeout", type=float, default=45)
    args = parser.parse_args(argv)
    try:
        result = execute(args.engine, args.bottle, args.fixture, args.proxy, args.output_dir, args.timeout, args.mode,
                         args.vertex_programs, args.experimental_combined)
    except (RunError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"PASS: {args.mode}, identical HRESULTs and RGB PPM {result['runs']['baseline']['ppm_sha256']}; results: {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
