#!/usr/bin/env python3
"""Compare the synthetic D3D8 UP fixture, proxy capture and DX12 replay.

All paths must be absolute. Output is created in a new directory and partial
artifacts are retained on failure. This does not launch TDS or validate a game
frame, and does not alter Highball's saved configuration.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys


CONVERTER = Path(__file__).resolve().with_name("trace_to_up_packet.py")
PPM_HEADER = b"P6\n256 256\n255\n"
PPM_SIZE = len(PPM_HEADER) + 256 * 256 * 3
LIMITATIONS = [
    "Synthetic single DrawPrimitiveUP triangle, 256x256, uniform opaque color.",
    "This is not a general T3 frame replay, gameplay parity, a PBR test, or a native Windows test.",
    "Unobserved D3D8 operations and other shader, geometry, texture and lighting cases are not validated.",
    "Parity is specific to the selected engine, bottle, build and runtime environment.",
]


class RunError(Exception):
    pass


def valid_path(value):
    path = Path(value)
    if not path.is_absolute() or any(c in value for c in (":", "\\", "\n", "\r")):
        raise RunError(f"path must be absolute and contain no colon, backslash or newline: {value}")
    return path


def check_pe32(path, dll):
    """Reject non-x86, wrong-kind, truncated or non-PE32 input binaries."""
    try:
        with path.open("rb") as source:
            dos = source.read(64)
            if len(dos) != 64 or dos[:2] != b"MZ":
                raise ValueError("missing DOS header")
            offset = struct.unpack_from("<I", dos, 0x3c)[0]
            if offset < 64 or offset > 16 * 1024 * 1024:
                raise ValueError("invalid PE offset")
            source.seek(offset)
            header = source.read(26)
            if len(header) != 26 or header[:4] != b"PE\0\0":
                raise ValueError("missing PE header")
            machine, _, _, _, _, opt_size, flags = struct.unpack_from("<HHIIIHH", header, 4)
            magic = struct.unpack_from("<H", header, 24)[0]
            if machine != 0x14c or not flags & 0x0002 or bool(flags & 0x2000) != dll:
                raise ValueError("wrong machine or binary kind")
            if opt_size < 96 or magic != 0x10b:
                raise ValueError("not PE32")
            source.seek(offset + 24 + opt_size - 1)
            if not source.read(1):
                raise ValueError("truncated optional header")
    except (OSError, ValueError, struct.error) as exc:
        kind = "DLL" if dll else "executable"
        raise RunError(f"{path} must be a valid x86 PE32 {kind}: {exc}") from exc


def windows_path(path):
    return "Z:" + str(path).replace("/", "\\")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_stage(name, args, cwd, env, log_path, timeout):
    with log_path.open("x", encoding="utf-8") as log:
        log.write(json.dumps({"stage": name, "command": [str(arg) for arg in args],
                              "cwd": str(cwd)}, ensure_ascii=False) + "\n")
        log.flush()
        try:
            process = subprocess.Popen([str(arg) for arg in args], cwd=cwd, env=env,
                                       stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                status = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired as exc:
                # Only terminate the new process group, never the bottle's existing wineserver.
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
                log.write(f"\ntimeout_seconds={timeout}\n")
                raise RunError(f"{name} timed out after {timeout} seconds; see {log_path}") from exc
            log.write(f"\nexit_status={status}\n")
        except OSError as exc:
            raise RunError(f"{name} could not start: {exc}; see {log_path}") from exc
    if status != 0:
        raise RunError(f"{name} exited with status {status}; see {log_path}")


def image_data(path):
    try:
        with path.open("rb") as image:
            data = image.read(PPM_SIZE + 1)
    except OSError as exc:
        raise RunError(f"cannot read image {path}: {exc}") from exc
    if len(data) != PPM_SIZE or not data.startswith(PPM_HEADER):
        raise RunError(f"{path} is not the expected complete 256x256 P6 PPM")
    return data


def execute(engine, bottle, build, output, timeout, camera=False):
    engine, bottle, build, output = map(valid_path, (engine, bottle, build, output))
    wine = engine / "engine/bin/wine"
    wineserver = engine / "engine/bin/wineserver"
    if not all(path.is_file() and os.access(path, os.X_OK) for path in (wine, wineserver)) or not (engine / "frameworks").is_dir():
        raise RunError(f"invalid Highball engine: {engine}")
    z_drive = bottle / "dosdevices/z:"
    if not bottle.is_dir() or not z_drive.is_symlink() or os.readlink(z_drive) != "/":
        raise RunError(f"bottle must map Z: to /: {bottle}")
    fixture = build / "d3d8_up_fixture/t3render_d3d8_up_fixture.exe"
    proxy = build / "d3d8.dll"
    replay = build / "t3render_d3d12_replay_up.exe"
    for path, dll in ((fixture, False), (proxy, True), (replay, False)):
        check_pe32(path, dll)
    if not math.isfinite(timeout) or timeout <= 0:
        raise RunError("timeout must be a positive finite number of seconds")
    try:
        resolved_output = output.resolve()
        for protected in (engine, bottle, build):
            resolved_protected = protected.resolve()
            if resolved_output == resolved_protected or resolved_protected in resolved_output.parents:
                raise RunError(f"output directory must be outside engine, bottle and build: {output}")
    except OSError as exc:
        raise RunError(f"cannot resolve output location: {exc}") from exc
    try:
        output.mkdir()
    except OSError as exc:
        raise RunError(f"cannot create new output directory {output}: {exc}") from exc

    mode_limit = ("Camera mode uses a single XYZ triangle and captured world, view and projection matrices."
                  if camera else "Screen-space mode is restricted to z=0 and rhw=1.")
    result = {"schema": 1, "test": "synthetic_d3d8_up_dx12_replay", "status": "FAIL",
              "fixture_mode": "camera" if camera else "screen_space",
              "engine": str(engine), "bottle": str(bottle), "build_dir": str(build),
              "limitations": [mode_limit, *LIMITATIONS], "stages": [], "sha256": {}}
    stage = "setup"
    try:
        baseline = output / "baseline"
        captured = output / "captured"
        baseline.mkdir()
        captured.mkdir()
        baseline_exe = baseline / fixture.name
        captured_exe = captured / fixture.name
        shutil.copyfile(fixture, baseline_exe)
        shutil.copyfile(fixture, captured_exe)
        shutil.copyfile(proxy, captured / "d3d8.dll")
        for name, path in (("fixture_exe", baseline_exe), ("proxy_dll", captured / "d3d8.dll"), ("replay_exe", replay)):
            result["sha256"][name] = sha256(path)
        if result["sha256"]["fixture_exe"] != sha256(captured_exe):
            raise RunError("baseline and captured fixture copies differ")

        env = {k: v for k, v in os.environ.items() if not k.startswith("T3RENDER_CAPTURE_")}
        for key in ("WINEDLLPATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"):
            env.pop(key, None)
        frameworks = engine / "frameworks"
        env.update({
            "WINEPREFIX": str(bottle), "WINESERVER": str(wineserver),
            "WINEDLLPATH_PREPEND": f"{bottle}/.highball-dlss/wine:{engine}/renderers/d3dmetal-tsshim/wine:{engine}/renderers/d3dmetal/wine",
            "DYLD_FALLBACK_LIBRARY_PATH": f"{frameworks}:{frameworks}/GStreamer.framework/Versions/1.0/lib",
            "DYLD_FALLBACK_FRAMEWORK_PATH": str(frameworks), "WINEDEBUG": "-all,+loaddll",
            "WINEDLLOVERRIDES": "d3d8=b;nvapi64,nvngx=n,b;winemenubuilder.exe=d",
        })
        baseline_ppm, captured_ppm, replay_ppm = (output / name for name in ("baseline.ppm", "captured.ppm", "replay.ppm"))
        trace, packet = output / "capture.jsonl", output / "packet.txt"
        fixture_args = ["--camera"] if camera else []
        stage = "baseline"
        run_stage(stage, [wine, baseline_exe, *fixture_args, "--output", windows_path(baseline_ppm)], baseline, env,
                  output / "baseline.log", timeout)
        result["stages"].append(stage)
        capture_env = env.copy()
        capture_env.update({"WINEDLLOVERRIDES": "d3d8=n,b;nvapi64,nvngx=n,b;winemenubuilder.exe=d",
                            "T3RENDER_CAPTURE_FRAME": "0", "T3RENDER_CAPTURE_PATH": windows_path(trace)})
        stage = "capture"
        run_stage(stage, [wine, captured_exe, *fixture_args, "--output", windows_path(captured_ppm)], captured,
                  capture_env, output / "capture.log", timeout)
        result["stages"].append(stage)
        stage = "convert"
        run_stage(stage, [sys.executable, "-B", CONVERTER, trace, "--synthetic-fixture", "-o", packet],
                  output, env, output / "convert.log", timeout)
        with packet.open("rb") as source:
            header = source.readline(32).rstrip(b"\r\n")
        if header != (b"T3R_XYZ_V1" if camera else b"T3R_UP_V1"):
            raise RunError("converted packet does not match the selected fixture mode")
        result["stages"].append(stage)
        stage = "replay"
        run_stage(stage, [wine, replay, windows_path(packet), "--output", windows_path(replay_ppm)],
                  output, env, output / "replay.log", timeout)
        result["stages"].append(stage)
        stage = "compare"
        images = [image_data(path) for path in (baseline_ppm, captured_ppm, replay_ppm)]
        for name, path in (("baseline_ppm", baseline_ppm), ("captured_ppm", captured_ppm),
                           ("replay_ppm", replay_ppm), ("trace", trace), ("packet", packet)):
            result["sha256"][name] = sha256(path)
        if not images[0] == images[1] == images[2]:
            raise RunError("baseline, proxy capture and replay images differ byte for byte")
        result["stages"].append(stage)
        result["status"] = "PASS"
    except (RunError, OSError) as exc:
        result["failed_stage"] = stage
        result["error"] = str(exc)
    try:
        with (output / "result.json").open("x", encoding="utf-8") as dest:
            json.dump(result, dest, indent=2, ensure_ascii=False)
            dest.write("\n")
    except OSError as exc:
        raise RunError(f"cannot write result manifest in {output}: {exc}") from exc
    if result["status"] != "PASS":
        raise RunError(f"{result['failed_stage']}: {result['error']}; partial output: {output}")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, help="absolute Highball engine root")
    parser.add_argument("--bottle", required=True, help="absolute Wine bottle root")
    parser.add_argument("--build-dir", required=True, help="absolute x86 CMake build directory")
    parser.add_argument("--output-dir", required=True, help="new absolute output directory")
    parser.add_argument("--timeout", type=float, default=120, help="per-stage timeout in seconds (default: 120)")
    parser.add_argument("--camera", action="store_true", help="replay the synthetic XYZ camera fixture")
    args = parser.parse_args(argv)
    try:
        result = execute(args.engine, args.bottle, args.build_dir, args.output_dir, args.timeout, args.camera)
    except RunError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"PASS: identical PPM SHA-256 {result['sha256']['baseline_ppm']}; results: {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
