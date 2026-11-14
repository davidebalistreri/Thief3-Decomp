#!/usr/bin/env python3
"""Run one timed, three-frame diagnostic capture in a Highball virtual desktop.

This is an experimental observation of one proxy configuration. It cannot
verify the map or camera, compare vanilla pixels, or replay the room. The
optional termination affects only a single matching TDS process, never Steam.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

import analyze_tds_capture
from run_synthetic_up_replay import RunError, check_pe32
from run_winepath_proxy_probe import loaded_d3d8


class BatchError(RuntimeError):
    pass


def digest(path):
    return analyze_tds_capture.digest(path)


def game_pids():
    try:
        listing = subprocess.check_output(["ps", "-A", "-o", "pid=,comm="], text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise BatchError(f"cannot verify TDS processes: {exc}") from exc
    result = []
    for line in listing.splitlines():
        fields = line.strip().split(None, 1)
        if len(fields) == 2 and fields[1].casefold().endswith("\\system\\t3main.exe"):
            result.append(int(fields[0]))
    return result


def windows_path(path):
    return "Z:" + str(path).replace("/", "\\")


def install_proxy(source, destination, expected_hash=None):
    """Never overwrite a DLL or symlink, even when it has the same hash."""
    raw = source.read_bytes()
    expected = hashlib.sha256(raw).hexdigest()
    if expected_hash is not None and expected != expected_hash:
        raise BatchError("proxy changed before installation; no file was changed")
    if os.path.lexists(destination):
        raise BatchError(f"destination already exists: {destination}")
    created = None
    try:
        with destination.open("xb") as output:
            stat = os.fstat(output.fileno())
            created = (stat.st_dev, stat.st_ino)
            output.write(raw)
    except FileExistsError as exc:
        raise BatchError(f"destination appeared during installation: {destination}") from exc
    except BaseException:
        # Remove only the file we exclusively created; do not touch a
        # replacement installed by someone else during a failed write.
        try:
            stat = destination.lstat()
            if created == (stat.st_dev, stat.st_ino):
                destination.unlink()
        except OSError:
            pass
        raise
    return expected, created


def remove_proxy(destination, expected, processes, identity=None):
    """Only remove our exact binary when no TDS process is running."""
    if processes:
        raise BatchError("TDS is still running; the temporary proxy remains installed")
    if destination.is_symlink() or not destination.is_file():
        raise BatchError("proxy is missing, changed or a symlink; it was not removed")
    original = destination.lstat()
    if identity is not None and identity != (original.st_dev, original.st_ino):
        raise BatchError("proxy file identity changed; it was not removed")
    if digest(destination) != expected:
        raise BatchError("proxy is missing, changed or a symlink; it was not removed")
    current = destination.lstat()
    if (original.st_dev, original.st_ino) != (current.st_dev, current.st_ino):
        raise BatchError("proxy changed during verification; it was not removed")
    destination.unlink()


def complete_trace(path, frames):
    if not path.is_file():
        return False
    with path.open("rb") as source:
        source.seek(0, os.SEEK_END)
        source.seek(max(0, source.tell() - 256))
        tail = source.read().splitlines()
    if not tail:
        return False
    try:
        return json.loads(tail[-1]) == {"event": "capture_end", "frames": frames}
    except (ValueError, UnicodeError):
        return False


def validate_paths(engine, bottle, system, proxy, output):
    paths = [Path(p).expanduser() for p in (engine, bottle, system, proxy, output)]
    if not all(p.is_absolute() for p in paths):
        raise BatchError("all paths must be absolute")
    if any(any(c in str(p) for c in (":", "\\", "\n", "\r")) for p in paths):
        raise BatchError("paths must not contain a colon, backslash or newline")
    if os.path.lexists(paths[-1]):
        raise BatchError("output must be a new path with an existing parent")
    engine, bottle, system, proxy, output = (p.resolve() for p in paths)
    wine, server, frameworks = engine / "engine/bin/wine", engine / "engine/bin/wineserver", engine / "frameworks"
    if not all(p.is_file() and os.access(p, os.X_OK) for p in (wine, server)) or not frameworks.is_dir():
        raise BatchError("invalid Highball engine")
    zdrive = bottle / "dosdevices/z:"
    if not zdrive.is_symlink() or os.readlink(zdrive) != "/":
        raise BatchError("bottle must map Z: to /")
    drive = (bottle / "drive_c").resolve()
    if drive not in system.parents or not (system / "T3Main.exe").is_file():
        raise BatchError("game System directory must be inside the bottle and contain T3Main.exe")
    if any(output == p or p in output.parents for p in (engine, bottle, system, proxy.parent)):
        raise BatchError("output must be outside the engine, bottle and proxy directory")
    if not output.parent.is_dir() or os.path.lexists(output):
        raise BatchError("output must be a new path with an existing parent")
    try:
        check_pe32(proxy, True)
    except RunError as exc:
        raise BatchError(str(exc)) from exc
    return engine, bottle, system, proxy, output


def env_for(engine, bottle, output, frames):
    capture = {"T3RENDER_CAPTURE_PATH": windows_path(output / "bedroom.jsonl"),
               "T3RENDER_CAPTURE_TRIGGER_PATH": windows_path(output / "capture.ready"),
               "T3RENDER_CAPTURE_FRAMES": str(frames), "T3RENDER_CAPTURE_BUFFERS": "1",
               "T3RENDER_CAPTURE_SCREENSHOTS": "1",
               "T3RENDER_CAPTURE_DECLARATIONS": "1", "T3RENDER_CAPTURE_DECLARATION_HISTORY": "1",
               "T3RENDER_CAPTURE_VERTEX_PROGRAMS": "1", "T3RENDER_CAPTURE_VERTEX_CONSTANTS": "1",
               "T3RENDER_EXPERIMENTAL_COMBINED": "1", "T3RENDER_EXPERIMENTAL_REPEATED_UNLOCK": "1",
               "T3RENDER_EXPERIMENTAL_REPEAT_RECOVERY": "1"}
    env = {k: v for k, v in os.environ.items() if not k.startswith("T3RENDER_") and
           k not in ("WINEDLLPATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH")}
    frameworks = engine / "frameworks"
    env.update(capture)
    env.update({"WINEPREFIX": str(bottle), "WINESERVER": str(engine / "engine/bin/wineserver"),
                "WINEDLLPATH_PREPEND": f"{bottle}/.highball-dlss/wine:{engine}/renderers/d3dmetal-tsshim/wine:{engine}/renderers/d3dmetal/wine",
                "DYLD_FALLBACK_LIBRARY_PATH": f"{frameworks}:{frameworks}/GStreamer.framework/Versions/1.0/lib",
                "DYLD_FALLBACK_FRAMEWORK_PATH": str(frameworks), "WINEDEBUG": "+loaddll",
                "WINEDLLOVERRIDES": "d3d8=n,b;nvapi64,nvngx=n,b;winemenubuilder.exe=d"})
    return env, capture


def wait_for_one_game(deadline):
    while time.monotonic() < deadline:
        pids = game_pids()
        if len(pids) > 1:
            raise BatchError("multiple TDS processes detected")
        if pids:
            return
        time.sleep(0.5)
    raise BatchError("TDS did not start before the timeout")


def process_start(pid):
    try:
        return subprocess.check_output(["ps", "-p", str(pid), "-o", "lstart="], text=True).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise BatchError(f"cannot verify TDS process identity: {exc}") from exc


def terminate_game(deadline, expected_pid, expected_start):
    pids = game_pids()
    if pids and (pids != [expected_pid] or not expected_start or process_start(expected_pid) != expected_start):
        raise BatchError("TDS process changed after the trigger; none was terminated")
    if pids:
        os.kill(pids[0], signal.SIGTERM)
    while time.monotonic() < deadline:
        if not game_pids():
            return
        time.sleep(0.2)
    raise BatchError("TDS did not exit; the temporary proxy remains installed")


def loader_verified(log, windows_dll):
    records = loaded_d3d8(log)
    native = windows_dll.casefold()
    return any(name.casefold() == native and kind == "native" for name, kind in records) and any(
        kind == "builtin" and name.casefold().endswith("\\system32\\d3d8.dll") for name, kind in records)


def execute(engine, bottle, system, proxy, output, delay, timeout, frames, terminate,
            preflight=False, fixture=None, replay=None):
    engine, bottle, system, proxy, output = validate_paths(engine, bottle, system, proxy, output)
    if not math.isfinite(delay) or not math.isfinite(timeout) or delay < 0 or timeout <= delay or frames not in (1, 2, 3):
        raise BatchError("require nonnegative delay, a longer finite timeout and 1–3 frames")
    if (fixture is None) != (replay is None):
        raise BatchError("the synthetic fixture and DX12 replay must be supplied together")
    if fixture is not None:
        fixture, replay = Path(fixture), Path(replay)
        if not fixture.is_absolute() or not replay.is_absolute():
            raise BatchError("fixture and replay paths must be absolute")
        fixture, replay = fixture.resolve(), replay.resolve()
        try:
            check_pe32(fixture, False)
            check_pe32(replay, False)
        except RunError as exc:
            raise BatchError(str(exc)) from exc
    if game_pids():
        raise BatchError("TDS is already running; no proxy was installed")
    if os.path.lexists(system / "d3d8.dll"):
        raise BatchError("game System already contains d3d8.dll; no file was changed")
    output.mkdir()
    target = system / "d3d8.dll"
    env, capture = env_for(engine, bottle, output, frames)
    executable = "C:\\" + str((system / "T3Main.exe").relative_to((bottle / "drive_c").resolve())).replace("/", "\\")
    deployment = {"schema": 1, "proxy": str(target), "sha256": digest(proxy),
                  "capture_config": capture, "proxy_installed": False, "temporary_proxy_removed": None}
    (output / "deployment.json").write_text(json.dumps(deployment, indent=2) + "\n", encoding="utf-8")
    installed = False
    launcher = None
    game_seen = False
    status = "FAILED"
    reason = ""
    host_preflight = "NOT_REQUESTED"
    highball_preflight = "NOT_REQUESTED"
    try:
        root = Path(__file__).resolve().parents[2]
        if preflight:
            print("Esecuzione della suite T3Render prima dell'avvio del gioco...", flush=True)
            host_preflight = "FAILED"
            with (output / "preflight-tests.log").open("xb") as log:
                result = subprocess.run([sys.executable, "-B", "-m", "unittest", "discover", "-s",
                                         str(root / "t3render/tests")], cwd=root, stdout=log,
                                        stderr=subprocess.STDOUT, timeout=300, check=False)
            if result.returncode != 0:
                raise BatchError("host preflight failed; TDS was not launched")
            host_preflight = "PASS"
        if fixture is not None:
            print("Verifica sintetica Highball/DX12 prima dell'avvio del gioco...", flush=True)
            highball_preflight = "FAILED"
            with (output / "preflight-highball.log").open("xb") as log:
                result = subprocess.run([sys.executable, "-B", str(root / "t3render/tools/run_d3d8_device_facade.py"),
                    "--engine", str(engine), "--bottle", str(bottle), "--fixture", str(fixture),
                    "--output-dir", str(output / "synthetic-fixture"), "--resource-facade",
                    "--dx12-replay", str(replay), "--frame-image"], cwd=root, stdout=log, stderr=subprocess.STDOUT,
                    timeout=180, check=False)
            if result.returncode != 0:
                raise BatchError("synthetic Highball preflight failed; TDS was not launched")
            highball_preflight = "PASS"
        if game_pids():
            raise BatchError("TDS started during preflight; no proxy was installed")
        expected, identity = install_proxy(proxy, target, deployment["sha256"])
        installed = True
        deployment["proxy_installed"] = True
        deployment["file_identity"] = {"device": identity[0], "inode": identity[1]}
        (output / "deployment.json").write_text(json.dumps(deployment, indent=2) + "\n", encoding="utf-8")
        if expected != deployment["sha256"]:
            raise BatchError("proxy changed while it was installed")
        with (output / "wine.log").open("xb") as log:
            launcher = subprocess.Popen([str(engine / "engine/bin/wine"), r"C:\windows\explorer.exe",
                                         "/desktop=T3RenderTDS,1280x800", executable], cwd=system, env=env,
                                        stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + timeout
        wait_for_one_game(deadline)
        game_seen = True
        trigger_time = time.monotonic() + delay
        print(f"TDS avviato: cattura automatica tra {delay:g} secondi. Carica il salvataggio e resta nella bedroom.", flush=True)
        while time.monotonic() < trigger_time:
            if time.monotonic() >= deadline:
                raise BatchError("timeout before the scheduled capture")
            time.sleep(min(0.5, max(0, trigger_time - time.monotonic())))
        pids = game_pids()
        if len(pids) != 1:
            raise BatchError("TDS is absent or ambiguous at trigger time")
        trigger_pid = pids[0]
        trigger_start = process_start(trigger_pid)
        if not trigger_start:
            raise BatchError("cannot verify the TDS process start time")
        (output / "capture.ready").touch(exist_ok=False)
        trace = output / "bedroom.jsonl"
        while time.monotonic() < deadline and not complete_trace(trace, frames):
            if not game_pids():
                raise BatchError("TDS exited before the complete capture")
            time.sleep(0.5)
        if not complete_trace(trace, frames):
            raise BatchError("capture did not complete before the timeout")
        if not loader_verified(output / "wine.log", executable.rsplit("\\", 1)[0] + r"\d3d8.dll"):
            raise BatchError("Wine log does not establish that the expected proxy and builtin D3D8 were loaded")
        summary = analyze_tds_capture.analyze(trace)
        with (output / "summary.json").open("x", encoding="utf-8") as result_file:
            result_file.write(json.dumps(summary, indent=2) + "\n")
        status = "PARTIAL"
        print(f"Cattura completata: {frames} frame. Analisi: {output / 'summary.json'}", flush=True)
        if terminate:
            terminate_game(time.monotonic() + 10, trigger_pid, trigger_start)
        else:
            print("Chiudi TDS normalmente per rimuovere il proxy temporaneo.", flush=True)
            while game_pids():
                time.sleep(1)
    except (BatchError, OSError, ValueError, subprocess.SubprocessError, KeyboardInterrupt) as exc:
        reason = str(exc) or type(exc).__name__
    finally:
        if installed:
            try:
                # If process inspection fails, leave the proxy in place.
                if not game_seen and launcher is not None and launcher.poll() is None:
                    raise BatchError("Wine launcher is still active; the temporary proxy remains installed")
                identity = deployment["file_identity"]
                remove_proxy(target, deployment["sha256"], game_pids(), (identity["device"], identity["inode"]))
                deployment["temporary_proxy_removed"] = True
            except (BatchError, OSError) as exc:
                reason = (reason + "; " if reason else "") + str(exc)
            (output / "deployment.json").write_text(json.dumps(deployment, indent=2) + "\n", encoding="utf-8")
        result = {"schema": 1, "status": status, "error": reason or None,
                  "scene_confirmation": "unconfirmed", "frames_requested": frames,
                  "host_preflight": host_preflight, "highball_preflight": highball_preflight,
                  "proxy_installed": deployment["proxy_installed"],
                  "temporary_proxy_removed": deployment["temporary_proxy_removed"],
                  "limitations": ["Three consecutive frames use one experimental configuration, not an A/B comparison.",
                                  "The trace does not prove the map or camera; confirm them separately.",
                                  "Images, if captured, are pre-Present backbuffers; presentation transformations and concurrent Present calls are not validated.",
                                  "No full room replay or pixel parity is produced."]}
        (output / "batch.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if reason:
        raise BatchError(f"{reason}; diagnostics: {output}")
    return result


def cleanup(deployment_path):
    path = Path(deployment_path).resolve()
    deployment = json.loads(path.read_text(encoding="utf-8"))
    if deployment.get("proxy_installed") is not True or deployment.get("temporary_proxy_removed") is True:
        raise BatchError("deployment does not identify an installed temporary proxy")
    target = Path(deployment["proxy"])
    if target.name.casefold() != "d3d8.dll" or target.parent.name.casefold() != "system":
        raise BatchError("unexpected proxy path in deployment")
    identity = deployment.get("file_identity")
    if (not isinstance(identity, dict) or type(identity.get("device")) is not int or
            type(identity.get("inode")) is not int):
        raise BatchError("deployment lacks the installed file identity")
    remove_proxy(target, deployment["sha256"], game_pids(), (identity["device"], identity["inode"]))
    deployment["temporary_proxy_removed"] = True
    path.write_text(json.dumps(deployment, indent=2) + "\n", encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="launch one timed capture")
    for name in ("engine", "bottle", "system", "proxy", "output-dir"):
        run.add_argument("--" + name, required=True)
    run.add_argument("--delay", type=float, default=120, help="seconds after TDS starts before triggering")
    run.add_argument("--timeout", type=float, default=360, help="maximum seconds from launch to complete capture")
    run.add_argument("--frames", type=int, choices=(1, 2, 3), default=3)
    run.add_argument("--terminate-after-capture", action="store_true", help="close the one TDS test process after capture")
    run.add_argument("--preflight", action="store_true", help="run the host test suite before launching TDS")
    run.add_argument("--fixture", help="synthetic x86 Highball fixture (requires --dx12-replay)")
    run.add_argument("--dx12-replay", help="synthetic x86 replay executable (requires --fixture)")
    clean = sub.add_parser("cleanup", help="remove the exact temporary proxy after TDS exits")
    clean.add_argument("deployment", help="deployment.json from this run")
    args = parser.parse_args(argv)
    try:
        if args.command == "cleanup":
            cleanup(args.deployment)
            print("Proxy temporaneo rimosso.")
        else:
            execute(args.engine, args.bottle, args.system, args.proxy, args.output_dir,
                    args.delay, args.timeout, args.frames, args.terminate_after_capture,
                    args.preflight, args.fixture, args.dx12_replay)
    except (BatchError, OSError, ValueError, KeyError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
