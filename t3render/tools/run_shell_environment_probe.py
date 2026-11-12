#!/usr/bin/env python3
"""Probe ShellExecuteExA environment inheritance on Highball; never runs TDS."""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import sys

from run_synthetic_up_replay import RunError, check_pe32, run_stage, sha256, valid_path, windows_path


def expected_environment(output):
    return {"T3RENDER_SHELL_MARKER": "t3render-shell-inherit-v1",
            "T3RENDER_CAPTURE_PATH": windows_path(output / "unused-frame.jsonl"),
            "T3RENDER_CAPTURE_KEY": "F11", "T3RENDER_CAPTURE_BUFFERS": "1",
            "T3RENDER_CAPTURE_FRAME": "<absent>"}


def validate_environment(path, expected):
    with Path(path).open("rb") as source:
        data = source.read(16385)
    if len(data) > 16384:
        raise RunError("environment snapshot exceeds size limit")
    try:
        lines = data.decode("ascii").splitlines()
    except UnicodeError as exc:
        raise RunError("environment snapshot is not ASCII") from exc
    want = [f"{key}={value}" for key, value in expected.items()]
    if lines != want:
        raise RunError(f"environment snapshot mismatch: {path}")


def execute(engine, bottle, fixture, output, timeout):
    engine, bottle, fixture, output = map(valid_path, (engine, bottle, fixture, output))
    # The fixture deliberately uses ShellExecuteExA, so reject paths which do
    # not round-trip through the runtime's ANSI command line or need escaping.
    if any(not str(path).isascii() or '"' in str(path) for path in (fixture, output)):
        raise RunError("fixture and output paths must be ASCII and contain no double quotes")
    wine, server, frameworks = engine / "engine/bin/wine", engine / "engine/bin/wineserver", engine / "frameworks"
    if not all(p.is_file() and os.access(p, os.X_OK) for p in (wine, server)) or not frameworks.is_dir():
        raise RunError("invalid Highball engine")
    z = bottle / "dosdevices/z:"
    if not bottle.is_dir() or not z.is_symlink() or os.readlink(z) != "/":
        raise RunError("bottle must map Z: to /")
    check_pe32(fixture, False)
    if not math.isfinite(timeout) or timeout <= 0:
        raise RunError("timeout must be positive and finite")
    for protected in (engine, bottle):
        if output.resolve() == protected.resolve() or protected.resolve() in output.resolve().parents:
            raise RunError("output must be outside engine and bottle")
    try:
        output.mkdir()
    except OSError as exc:
        raise RunError(f"cannot create new output directory: {exc}") from exc
    result = {"schema": 1, "test": "synthetic_shellexecute_environment", "status": "FAIL",
              "fixture_sha256": sha256(fixture), "snapshots": {},
              "limitations": ["Two ShellExecuteExA calls between synthetic x86 programs on this runtime only.",
                              "Steam, Ion Launcher, TDS, and a level transition are not tested.",
                              "The fixture never loads the capture proxy; no game frame is captured."]}
    stage = "setup"
    try:
        executable = output / fixture.name
        shutil.copyfile(fixture, executable)
        expected = expected_environment(output)
        env = {k: v for k, v in os.environ.items() if not k.startswith("T3RENDER_CAPTURE_")}
        for key in ("WINEDLLPATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"):
            env.pop(key, None)
        env.update({"WINEPREFIX": str(bottle), "WINESERVER": str(server),
                    "WINEDLLPATH_PREPEND": f"{bottle}/.highball-dlss/wine:{engine}/renderers/d3dmetal-tsshim/wine:{engine}/renderers/d3dmetal/wine",
                    "DYLD_FALLBACK_LIBRARY_PATH": f"{frameworks}:{frameworks}/GStreamer.framework/Versions/1.0/lib",
                    "DYLD_FALLBACK_FRAMEWORK_PATH": str(frameworks), "WINEDEBUG": "-all",
                    "WINEDLLOVERRIDES": "nvapi64,nvngx=n,b;winemenubuilder.exe=d"})
        env.update({key: value for key, value in expected.items() if value != "<absent>"})
        stage = "shell_chain"
        run_stage(stage, [wine, executable, "parent", windows_path(output)], output, env, output / "shell.log", timeout)
        for phase in ("parent", "launcher", "game"):
            stage = phase
            snapshot = output / (phase + ".env")
            validate_environment(snapshot, expected)
            result["snapshots"][phase] = {"path": snapshot.name, "sha256": sha256(snapshot)}
        stage = "side_effects"
        if (output / "unused-frame.jsonl").exists():
            raise RunError("unexpected capture file: fixture must not load the proxy")
        result["status"] = "PASS"
    except (RunError, OSError, ValueError) as exc:
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
    parser.add_argument("--output-dir", required=True, help="new absolute output directory")
    parser.add_argument("--timeout", type=float, default=45)
    args = parser.parse_args(argv)
    try:
        execute(args.engine, args.bottle, args.fixture, args.output_dir, args.timeout)
    except RunError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"PASS: three matching environment snapshots; results: {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
