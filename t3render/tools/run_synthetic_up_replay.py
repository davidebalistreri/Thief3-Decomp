#!/usr/bin/env python3
"""Compare synthetic D3D8 fixture runs; optionally replay UP draws on DX12.

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
import re
import shutil
import signal
import struct
import subprocess
import sys

import trace_summary
import trace_to_up_packet


CONVERTER = Path(__file__).resolve().with_name("trace_to_up_packet.py")
BUFFER_CONVERTER = Path(__file__).resolve().with_name("buffer_snapshot_to_up_packet.py")
PPM_HEADER = b"P6\n256 256\n255\n"
PPM_SIZE = len(PPM_HEADER) + 256 * 256 * 3
READBACK_MARKER = b"buffer_readback vertex_offset=20 vertex_bytes=80 index_offset=4 index_bytes=6 result=PASS"
LIMITATIONS = [
    "Synthetic single triangle, 256x256, uniform opaque color.",
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


def validate_buffer_readback_log(path):
    with path.open("rb") as log:
        markers = [line.rstrip(b"\r\n") for line in log if line.startswith(b"buffer_readback ")]
    if markers != [READBACK_MARKER]:
        raise RunError(f"missing or invalid buffer readback PASS for expected ranges; see {path}")


def validate_buffered_trace(path):
    """Validate only the observed binding and draw metadata, not resource bytes."""
    try:
        records = trace_to_up_packet.validated_records(path)
        expected = ["header", "frame_begin", "clear", "clear_result", "draw", "draw_result", "frame_end", "capture_end"]
        if [item.get("event") for item in records] != expected:
            raise RunError("buffered fixture requires one complete ordered clear and draw")
        header, begin, clear, clear_result, draw, draw_result, end, _ = records
        integer = trace_to_up_packet.integer
        succeeded = trace_to_up_packet.hresult_succeeded
        if integer(header.get("operation_results")) != 1 or integer(begin.get("frame")) != 0:
            raise RunError("buffered fixture requires operation results in frame 0")
        if (integer(clear.get("rect_count")) != 0 or clear.get("rects_status") != "none" or
                clear.get("rects") != [] or integer(clear.get("flags")) != 1 or
                integer(clear.get("color")) != 0xff000000):
            raise RunError("buffered fixture clear differs from the expected target clear")
        if not all(succeeded(item.get("hr")) for item in (clear_result, draw_result)) or not succeeded(end.get("present_hr")):
            raise RunError("buffered fixture clear, draw or Present failed")
        if integer(end.get("draws")) != 1 or end.get("concurrent_operations") is not False or end.get("had_reset") is not False:
            raise RunError("buffered fixture requires one draw without concurrency or reset")
        expected_draw = {"primitive_type": 4, "min_vertex_index": 1, "num_vertices": 3,
                         "start_index": 2, "primitive_count": 1, "vertex_shader": 0x44, "pixel_shader": 0}
        if draw.get("call") != "DrawIndexedPrimitive" or any(integer(draw.get(k)) != v for k, v in expected_draw.items()):
            raise RunError("buffered fixture indexed draw parameters differ")
        streams = draw.get("streams")
        if not isinstance(streams, list):
            raise RunError("buffered fixture vertex streams are missing")
        stream_zero = [item for item in streams if isinstance(item, dict) and integer(item.get("stream")) == 0]
        if len(stream_zero) != 1:
            raise RunError("buffered fixture requires exactly one stream 0")
        stream = stream_zero[0]
        index = draw.get("indices")

        def bound(item):
            if not isinstance(item, dict) or not succeeded(item.get("hr")):
                return False
            buffer_id = item.get("buffer")
            if not isinstance(buffer_id, str) or not re.fullmatch(r"0x[0-9a-fA-F]+", buffer_id):
                return False
            try:
                return int(buffer_id[2:], 16) > 0
            except ValueError:
                return False

        vertex_desc = stream.get("description")
        if (not bound(stream) or integer(stream.get("stride")) != 20 or not isinstance(vertex_desc, dict) or
                integer(vertex_desc.get("size")) != 100 or integer(vertex_desc.get("fvf")) != 0x44):
            raise RunError("buffered fixture stream 0 binding, stride or vertex description differs")
        index_desc = index.get("description") if isinstance(index, dict) else None
        if (not bound(index) or integer(index.get("base_vertex_index")) != 1 or not isinstance(index_desc, dict) or
                integer(index_desc.get("format")) != 101 or integer(index_desc.get("size")) != 10):
            raise RunError("buffered fixture index binding, base or description differs")
        return {"call": "DrawIndexedPrimitive", **expected_draw, "stream": 0, "stride": 20,
                "vertex_buffer_size": 100, "index_format": 101, "index_buffer_size": 10,
                "base_vertex_index": 1}
    except (trace_summary.TraceError, OSError, UnicodeError, ValueError) as exc:
        raise RunError(f"invalid buffered fixture trace: {exc}") from exc


def execute(engine, bottle, build, output, timeout, camera=False, buffered=False,
            verify_buffer_readback=False, replay_buffer_snapshot=False):
    if camera and buffered:
        raise RunError("camera and buffered are exclusive")
    if verify_buffer_readback and not buffered:
        raise RunError("buffer readback verification requires buffered mode")
    if replay_buffer_snapshot and not (buffered and verify_buffer_readback):
        raise RunError("buffer snapshot replay requires buffered mode and buffer readback verification")
    needs_replay = not buffered or replay_buffer_snapshot
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
    binaries = [(fixture, False), (proxy, True)]
    if needs_replay:
        binaries.append((replay, False))
    for path, dll in binaries:
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

    if replay_buffer_snapshot:
        mode_limit = ("Only the fixture-owned READONLY snapshot is replayed; the proxy trace does not contain "
                      "buffer bytes. Indexed vertices are converted to one unindexed synthetic triangle.")
    elif buffered:
        mode_limit = "Buffered capture records draw and binding metadata only; buffer bytes and DX12 replay are not available."
    elif camera:
        mode_limit = "Camera mode uses one DrawPrimitiveUP XYZ triangle and captured world, view and projection matrices."
    else:
        mode_limit = "Screen-space DrawPrimitiveUP mode is restricted to z=0 and rhw=1."
    test_name = ("synthetic_d3d8_fixture_buffer_snapshot_dx12_replay" if replay_buffer_snapshot else
                 "synthetic_d3d8_indexed_buffer_capture" if buffered else "synthetic_d3d8_up_dx12_replay")
    result = {"schema": 1, "test": test_name,
              "status": "FAIL", "dx12_replay": "not_run" if buffered else "performed_on_success",
              "fixture_mode": "buffered" if buffered else ("camera" if camera else "screen_space"),
              "engine": str(engine), "bottle": str(bottle), "build_dir": str(build),
              "limitations": [mode_limit, *LIMITATIONS], "stages": [], "sha256": {}}
    if replay_buffer_snapshot:
        result["buffer_snapshot"] = {"producer": "fixture_readback_not_proxy_trace", "conversion": "not_verified"}
    if verify_buffer_readback:
        result["buffer_readback"] = {"requested": True, "baseline": "not_verified", "capture": "not_verified",
                                     "vertex_offset": 20, "vertex_bytes": 80, "index_offset": 4, "index_bytes": 6}
        result["limitations"].append(
            "The fixture compares readback ranges against its known inputs; buffer contents are not serialized in the trace. "
            "Driver read locks may stall or fail for other resources and runtimes.")
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
        used_binaries = [("fixture_exe", baseline_exe), ("proxy_dll", captured / "d3d8.dll")]
        if needs_replay:
            used_binaries.append(("replay_exe", replay))
        for name, path in used_binaries:
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
        buffer_snapshot = output / "buffer-snapshot.txt"
        fixture_args = ["--buffered"] if buffered else (["--camera"] if camera else [])
        if verify_buffer_readback:
            fixture_args.append("--verify-buffer-readback")
        stage = "baseline"
        run_stage(stage, [wine, baseline_exe, *fixture_args, "--output", windows_path(baseline_ppm)], baseline, env,
                  output / "baseline.log", timeout)
        if verify_buffer_readback:
            validate_buffer_readback_log(output / "baseline.log")
            result["buffer_readback"]["baseline"] = "PASS"
        result["stages"].append(stage)
        capture_env = env.copy()
        capture_env.update({"WINEDLLOVERRIDES": "d3d8=n,b;nvapi64,nvngx=n,b;winemenubuilder.exe=d",
                            "T3RENDER_CAPTURE_FRAME": "0", "T3RENDER_CAPTURE_PATH": windows_path(trace)})
        stage = "capture"
        capture_args = fixture_args + (["--buffer-snapshot", windows_path(buffer_snapshot)] if replay_buffer_snapshot else [])
        run_stage(stage, [wine, captured_exe, *capture_args, "--output", windows_path(captured_ppm)], captured,
                  capture_env, output / "capture.log", timeout)
        if verify_buffer_readback:
            validate_buffer_readback_log(output / "capture.log")
            result["buffer_readback"]["capture"] = "PASS"
        result["stages"].append(stage)
        if buffered:
            stage = "trace_validation"
            result["buffered_draw"] = validate_buffered_trace(trace)
            result["stages"].append(stage)
        if needs_replay:
            stage = "convert"
            conversion_source = buffer_snapshot if replay_buffer_snapshot else trace
            converter = BUFFER_CONVERTER if replay_buffer_snapshot else CONVERTER
            run_stage(stage, [sys.executable, "-B", converter, conversion_source, "--synthetic-fixture", "-o", packet],
                      output, env, output / "convert.log", timeout)
            with packet.open("rb") as source:
                header = source.readline(32).rstrip(b"\r\n")
            if header != (b"T3R_XYZ_V1" if camera else b"T3R_UP_V1"):
                raise RunError("converted packet does not match the selected fixture mode")
            if replay_buffer_snapshot:
                result["buffer_snapshot"]["conversion"] = "PASS"
            result["stages"].append(stage)
            stage = "replay"
            if replay_buffer_snapshot:
                result["dx12_replay"] = "attempted"
            run_stage(stage, [wine, replay, windows_path(packet), "--output", windows_path(replay_ppm)],
                      output, env, output / "replay.log", timeout)
            if replay_buffer_snapshot:
                result["dx12_replay"] = "completed"
            result["stages"].append(stage)
        stage = "compare"
        image_paths = [baseline_ppm, captured_ppm] + ([replay_ppm] if needs_replay else [])
        images = [image_data(path) for path in image_paths]
        artifacts = [("baseline_ppm", baseline_ppm), ("captured_ppm", captured_ppm), ("trace", trace)]
        if needs_replay:
            artifacts.extend([("replay_ppm", replay_ppm), ("packet", packet)])
        if replay_buffer_snapshot:
            artifacts.append(("buffer_snapshot", buffer_snapshot))
        for name, path in artifacts:
            result["sha256"][name] = sha256(path)
        if not all(image == images[0] for image in images[1:]):
            raise RunError("fixture images differ byte for byte")
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
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--camera", action="store_true", help="replay the synthetic XYZ camera fixture")
    modes.add_argument("--buffered", action="store_true", help="compare indexed D3D8 draws; replay only with --replay-buffer-snapshot")
    parser.add_argument("--verify-buffer-readback", action="store_true",
                        help="verify fixture readback in both runs (requires --buffered)")
    parser.add_argument("--replay-buffer-snapshot", action="store_true",
                        help="replay the fixture-owned indexed buffer snapshot on DX12 (requires --buffered --verify-buffer-readback)")
    args = parser.parse_args(argv)
    if args.verify_buffer_readback and not args.buffered:
        parser.error("--verify-buffer-readback requires --buffered")
    if args.replay_buffer_snapshot and not (args.buffered and args.verify_buffer_readback):
        parser.error("--replay-buffer-snapshot requires --buffered and --verify-buffer-readback")
    try:
        result = execute(args.engine, args.bottle, args.build_dir, args.output_dir, args.timeout,
                         args.camera, args.buffered, args.verify_buffer_readback, args.replay_buffer_snapshot)
    except RunError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    suffix = "; DX12 replay not run" if args.buffered and not args.replay_buffer_snapshot else ""
    print(f"PASS: identical PPM SHA-256 {result['sha256']['baseline_ppm']}{suffix}; results: {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
