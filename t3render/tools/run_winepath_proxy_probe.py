#!/usr/bin/env python3
"""Test isolated proxy loading via WINEPATH, without copying a DLL into a game.

The executable, DLL and output are synthetic. The loader log must identify the
exact native DLL in a separate directory, and a complete UP trace must validate.
This does not test TDS, Steam, level changes or buffer-wrapper safety.
"""
import argparse
import json
import math
import ntpath
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

from run_synthetic_up_replay import RunError, check_pe32, image_data, run_stage, sha256, valid_path, windows_path, PPM_HEADER
import trace_summary
import trace_to_up_packet


LOAD_LINE = re.compile(r'\btrace:loaddll:\S+\s+Loaded\s+L"([^"\r\n]+)"\s+at\s+(?:0x)?[0-9a-f]+:\s*(native|builtin)\b', re.I)


def loaded_d3d8(path):
    """Extract Wine +loaddll records, not arbitrary fixture output."""
    records = []
    with Path(path).open(encoding="utf-8", errors="replace") as log:
        for line in log:
            match = LOAD_LINE.search(line)
            if not match:
                continue
            # Wine debugstr_w doubles path backslashes in quoted L strings.
            name = match.group(1).replace("\\\\", "\\")
            if ntpath.basename(name).casefold() == "d3d8.dll":
                records.append((name, match.group(2).casefold()))
    return records


def check_loader(path, *, expected_native=None):
    records = loaded_d3d8(path)
    native = [name for name, kind in records if kind == "native"]
    builtin = [name for name, kind in records if kind == "builtin"]
    if expected_native is None:
        if native or not builtin:
            raise RunError("baseline did not load the builtin D3D8 alone")
    elif len(native) != 1 or ntpath.normcase(native[0]) != ntpath.normcase(expected_native):
        raise RunError("loader did not report exactly the requested proxy as native D3D8")
    return [{"path": name, "kind": kind} for name, kind in records]


def check_fixture_image(data):
    offset = len(PPM_HEADER) + (128 * 256 + 128) * 3
    if data[offset:offset + 3] != b"\xff\x00\x00":
        raise RunError("fixture triangle does not cover the expected red center")


def execute(engine, bottle, fixture, proxy, output, timeout):
    engine, bottle, fixture, proxy, output = map(valid_path, (engine, bottle, fixture, proxy, output))
    if any(not str(path).isascii() or '"' in str(path) for path in (fixture, proxy, output)):
        raise RunError("fixture, proxy and output paths must be ASCII and contain no double quotes")
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
        fixture_hash, proxy_hash = sha256(fixture), sha256(proxy)
    except OSError as exc:
        raise RunError(f"cannot hash input binaries: {exc}") from exc
    try:
        output.mkdir()
    except OSError as exc:
        raise RunError(f"cannot create new output directory: {exc}") from exc
    result = {"schema": 1, "test": "synthetic_d3d8_winepath_proxy", "status": "FAIL",
              "fixture_sha256": fixture_hash, "proxy_sha256": proxy_hash, "runs": {},
              "limitations": ["Isolated synthetic UP fixture; TDS and Steam are not launched.",
                              "The buffer observer is disabled; wrapper/alias safety is not tested.",
                              "This tests only this Wine environment and loader path, not real TDS launch inheritance.",
                              "PPM comparison covers RGB only and does not establish general D3D8 parity."]}
    stage = "setup"
    try:
        env = {k: v for k, v in os.environ.items() if not k.startswith("T3RENDER_CAPTURE_")}
        for key in ("WINEDLLPATH", "WINEPATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"):
            env.pop(key, None)
        env.update({"WINEPREFIX": str(bottle), "WINESERVER": str(server),
                    "WINEDLLPATH_PREPEND": f"{bottle}/.highball-dlss/wine:{engine}/renderers/d3dmetal-tsshim/wine:{engine}/renderers/d3dmetal/wine",
                    "DYLD_FALLBACK_LIBRARY_PATH": f"{frameworks}:{frameworks}/GStreamer.framework/Versions/1.0/lib",
                    "DYLD_FALLBACK_FRAMEWORK_PATH": str(frameworks), "WINEDEBUG": "-all,+loaddll",
                    "WINEDLLOVERRIDES": "d3d8=b;nvapi64,nvngx=n,b;winemenubuilder.exe=d"})
        # Only our newly created scratch directory is removed. Diagnostics and
        # the trace remain in the new output directory even on a failed run.
        with tempfile.TemporaryDirectory(prefix="t3render-winepath-", dir=output) as tmp:
            scratch = Path(tmp)
            app, dll_dir = scratch / "app", scratch / "proxy"
            app.mkdir(); dll_dir.mkdir()
            executable, dll = app / fixture.name, dll_dir / "d3d8.dll"
            shutil.copyfile(fixture, executable)
            shutil.copyfile(proxy, dll)
            if sha256(executable) != result["fixture_sha256"] or sha256(dll) != result["proxy_sha256"]:
                raise RunError("temporary binary copies differ from inputs")
            if (app / "d3d8.dll").exists():
                raise RunError("fixture directory unexpectedly contains d3d8.dll")
            images = []
            for stage in ("baseline", "capture"):
                child_env = env.copy()
                if stage == "capture":
                    child_env.update({"WINEPATH": windows_path(dll_dir),
                                      "WINEDLLOVERRIDES": "d3d8=n,b;nvapi64,nvngx=n,b;winemenubuilder.exe=d",
                                      "T3RENDER_CAPTURE_FRAME": "0",
                                      "T3RENDER_CAPTURE_PATH": windows_path(output / "capture.jsonl")})
                log, ppm = output / (stage + ".log"), output / (stage + ".ppm")
                run_stage(stage, [wine, executable, "--output", windows_path(ppm)], app, child_env, log, timeout)
                loaded = check_loader(log, expected_native=windows_path(dll) if stage == "capture" else None)
                image = image_data(ppm)
                check_fixture_image(image)
                images.append(image)
                result["runs"][stage] = {"log": log.name, "log_sha256": sha256(log),
                                         "ppm": ppm.name, "ppm_sha256": sha256(ppm), "d3d8_loads": loaded}
            stage = "trace"
            trace = output / "capture.jsonl"
            packet = trace_to_up_packet.convert(trace, synthetic_fixture=True)
            if not packet.startswith("T3R_UP_V1\n"):
                raise RunError("capture is not the expected screen-space UP fixture")
            result["trace_sha256"] = sha256(trace)
            stage = "compare"
            if images[0] != images[1]:
                raise RunError("baseline and capture RGB images differ")
            result["status"] = "PASS"
    except (RunError, OSError, ValueError, UnicodeError, trace_summary.TraceError) as exc:
        result["failed_stage"] = stage
        result["error"] = str(exc)
    try:
        with (output / "result.json").open("x", encoding="utf-8") as destination:
            json.dump(result, destination, indent=2)
            destination.write("\n")
    except OSError as exc:
        raise RunError(f"cannot write result manifest: {exc}") from exc
    if result["status"] != "PASS":
        raise RunError(f"{stage}: {result['error']}; partial output: {output}")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--bottle", required=True)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--proxy", required=True)
    parser.add_argument("--output-dir", required=True, help="new absolute output directory")
    parser.add_argument("--timeout", type=float, default=45)
    args = parser.parse_args(argv)
    try:
        result = execute(args.engine, args.bottle, args.fixture, args.proxy, args.output_dir, args.timeout)
    except RunError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"PASS: native proxy loaded from WINEPATH; RGB SHA-256 {result['runs']['baseline']['ppm_sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
