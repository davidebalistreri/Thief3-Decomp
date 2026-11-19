#!/usr/bin/env python3
"""Run one timed, three-frame diagnostic capture in a Highball virtual desktop.

This is an experimental observation of one proxy configuration. It cannot
verify the map or camera, compare vanilla pixels, or replay the room. The
optional termination targets one matching TDS process. Closing an isolated
prefix also stops its private Steam and Wine processes.
"""

import argparse
import hashlib
import json
import math
import ntpath
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

import analyze_tds_capture
from run_synthetic_up_replay import RunError, check_pe32
from run_winepath_proxy_probe import LOAD_LINE, loaded_d3d8
from tds_save_sandbox import IsolatedSavePrefix, SandboxError


class BatchError(RuntimeError):
    pass


# The launcher used this URL when the confirmed bedroom save was loaded.
# It refers to the game's Current Save, which must be prepared separately.
BEDROOM_SAVE_URL = "SouthQuarter1_int1.gmp?-LoadSave?-ObjectFilter=1"


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


def wine_or_steam_pids():
    try:
        listing = subprocess.check_output(["ps", "-A", "-o", "pid=,comm="], text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise BatchError(f"cannot verify Wine processes: {exc}") from exc
    result = []
    for line in listing.splitlines():
        fields = line.strip().split(None, 1)
        if len(fields) != 2:
            continue
        name = fields[1].casefold().replace("\\", "/")
        if name.endswith("/wineserver") or name.endswith("/steam.exe"):
            result.append(int(fields[0]))
    return result


def readonly_save_profile(system):
    saves = system.parent / "save" / "SaveGames"
    if saves.is_symlink() or not saves.is_dir():
        raise BatchError("read-only save guard requires the existing real SaveGames directory")
    saves = saves.resolve(strict=True)
    # The profile is passed as one argument, not interpreted by a shell.
    path = json.dumps(str(saves), ensure_ascii=False)
    return f"(version 1)(allow default)(deny file-write* (subpath {path}))"


def readonly_save_command(command, profile, env):
    # macOS strips DYLD_* while launching the protected sandbox-exec binary.
    # Restore the renderer search paths inside the sandbox, before Wine starts.
    paths = [f"{name}={env[name]}" for name in
             ("DYLD_FALLBACK_LIBRARY_PATH", "DYLD_FALLBACK_FRAMEWORK_PATH")]
    return ["/usr/bin/sandbox-exec", "-p", profile, "/usr/bin/env", *paths, *command]


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


def env_for(engine, bottle, output, frames, scene_bridge=False, submission_scope=False, live_catalog=None,
            live_only=False, live_static_meshes=False, live_lights=False, gpu_metrics=False, preview_pbr=False,
            preview_candle=False, candle_link_diagnostics=False, wall_displacement=False, preview_gi=False,
            sample_interval_ms=None, remaster_toggle=False):
    capture = {"T3RENDER_CAPTURE_PATH": windows_path(output / "bedroom.jsonl"),
               "T3RENDER_CAPTURE_TRIGGER_PATH": windows_path(output / "capture.ready"),
               "T3RENDER_CAPTURE_FRAMES": str(frames), "T3RENDER_CAPTURE_BUFFERS": "1",
               "T3RENDER_CAPTURE_SCREENSHOTS": "1",
               "T3RENDER_CAPTURE_DECLARATIONS": "1", "T3RENDER_CAPTURE_DECLARATION_HISTORY": "1",
               "T3RENDER_CAPTURE_VERTEX_PROGRAMS": "1", "T3RENDER_CAPTURE_VERTEX_CONSTANTS": "1",
               "T3RENDER_EXPERIMENTAL_COMBINED": "1", "T3RENDER_EXPERIMENTAL_REPEATED_UNLOCK": "1",
               "T3RENDER_EXPERIMENTAL_REPEAT_RECOVERY": "1"}
    if sample_interval_ms is not None:
        capture["T3RENDER_CAPTURE_INTERVAL_MS"] = str(sample_interval_ms)
        capture["T3RENDER_SCENE_LIVE_INITIAL_IMAGES"] = "0"
    if remaster_toggle:
        capture["T3RENDER_SCENE_LIVE_REMASTER_TOGGLE"] = windows_path(output / "remaster.enable")
        capture["T3RENDER_SCENE_LIVE_INITIAL_IMAGES"] = "0"
    if live_only:
        if live_catalog is None:
            raise BatchError("live-only mode requires an offline catalog")
        capture = {"T3RENDER_EXPERIMENTAL_COMBINED": "1", "T3RENDER_SCENE_LIVE_INITIAL_IMAGES": "0"}
    if scene_bridge:
        capture["T3RENDER_SCENE_BRIDGE"] = "1"
    if submission_scope:
        if not scene_bridge:
            raise BatchError("submission scope requires the scene bridge probe")
        capture["T3RENDER_SCENE_BRIDGE_SUBMISSION"] = "1"
    if live_catalog is not None:
        if not scene_bridge or not submission_scope:
            raise BatchError("live viewer requires the scene bridge and submission scope")
        capture.update({"T3RENDER_SCENE_LIVE": "1", "T3RENDER_SCENE_LIVE_DX12": "1",
                        "T3RENDER_LIVE_CATALOG": windows_path(live_catalog),
                        "T3RENDER_SCENE_LIVE_DIAGNOSTIC_DIR": windows_path(output)})
    if live_static_meshes:
        if live_catalog is None:
                raise BatchError("static mesh sampling requires an offline v4, v5 or static-enabled v6 live catalog")
        capture["T3RENDER_SCENE_LIVE_STATIC_MESHES"] = "1"
    if live_lights:
        if live_catalog is None:
            raise BatchError("light candidate sampling requires a live catalog")
        capture["T3RENDER_SCENE_LIVE_LIGHTS"] = "1"
    if gpu_metrics:
        if live_catalog is None:
            raise BatchError("GPU metrics require a live viewer catalog")
        capture["T3RENDER_SCENE_LIVE_GPU_METRICS"] = "1"
    if preview_pbr:
        if live_catalog is None:
            raise BatchError("PBR preview requires a live viewer catalog")
        capture["T3RENDER_SCENE_LIVE_PREVIEW_PBR"] = "1"
    if preview_candle:
        if live_catalog is None or not live_lights or preview_pbr:
            raise BatchError("candle preview requires the live light sampler and excludes the headlight preview")
        capture["T3RENDER_SCENE_LIVE_PREVIEW_CANDLE"] = "1"
    if candle_link_diagnostics:
        if live_catalog is None or not live_lights:
            raise BatchError("candle link diagnostics require the live light sampler")
        capture["T3RENDER_SCENE_LIVE_CANDLE_LINK"] = "1"
    if wall_displacement:
        if live_catalog is None:
            raise BatchError("wall displacement requires a height-enabled live catalog")
        capture["T3RENDER_SCENE_LIVE_WALL_DISPLACEMENT"] = "1"
    if preview_gi:
        if live_catalog is None:
            raise BatchError("GI preview requires a height-enabled live catalog")
        capture["T3RENDER_SCENE_LIVE_PREVIEW_GI"] = "1"
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


class WineMainThreadCrash:
    """Watch the session log for an exception on T3Main's loader thread.

    Wine's log identifies threads, not host PIDs. Report only an unambiguous
    native load of the exact executable followed by an exception on that same
    thread; exceptions on other threads or from other processes remain unknown.
    """
    _thread = re.compile(r"^([0-9a-f]{4,8}):", re.I)
    _exception = re.compile(r"^wine: Unhandled .+ \(thread ([0-9a-f]{4,8})\), starting debugger", re.I)
    _chunk_limit = 2 * 1024 * 1024
    _line_limit = 64 * 1024

    def __init__(self, path, executable):
        self.path = Path(path)
        self.executable = ntpath.normcase(executable)
        self.offset = 0
        self.pending = b""
        self.discarding = False
        self.loads = 0
        self.thread = None

    def check(self):
        with self.path.open("rb") as stream:
            stream.seek(self.offset)
            chunk = stream.read(self._chunk_limit)
            self.offset = stream.tell()
        parts = (self.pending + chunk).split(b"\n")
        remainder = parts.pop()
        for raw in parts:
            if self.discarding:
                self.discarding = False
                continue
            if len(raw) > self._line_limit:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip("\r")
            loader, thread = LOAD_LINE.search(line), self._thread.match(line)
            if loader and thread and loader.group(2).casefold() == "native":
                name = ntpath.normcase(loader.group(1).replace("\\\\", "\\"))
                if name == self.executable:
                    self.loads += 1
                    self.thread = thread.group(1).casefold()
            exception = self._exception.match(line)
            if exception and self.loads == 1 and exception.group(1).casefold() == self.thread:
                raise BatchError("Wine reported an unhandled exception on the T3Main loader thread")
        if self.discarding:
            self.pending = b""
        elif len(remainder) > self._line_limit:
            self.pending = b""
            self.discarding = True
        else:
            self.pending = remainder


def wait_for_one_game(deadline, crash_monitor=None):
    while time.monotonic() < deadline:
        if crash_monitor:
            crash_monitor.check()
        pids = game_pids()
        if len(pids) > 1:
            raise BatchError("multiple TDS processes detected")
        if pids:
            return pids[0]
        time.sleep(0.5)
    raise BatchError("TDS did not start before the timeout")


def require_same_game(expected_pid, expected_start):
    pids = game_pids()
    if not pids:
        raise BatchError("TDS exited before the capture")
    if pids != [expected_pid] or not expected_start or process_start(expected_pid) != expected_start:
        raise BatchError("TDS process changed while waiting")


def wait_for_trigger(deadline, crash_monitor, *, request=None, trigger_time=None, abort=None):
    """Allow normal level-load restarts before binding a process at capture."""
    if (request is None) == (trigger_time is None):
        raise BatchError("capture needs exactly one trigger mode")
    missing_since = None
    while time.monotonic() < deadline:
        crash_monitor.check()
        if abort is not None and abort.is_file():
            raise BatchError("remaster comparison watcher reported a failure")
        now = time.monotonic()
        pids = game_pids()
        if pids:
            missing_since = None
        elif missing_since is None:
            missing_since = now
        elif now - missing_since >= 30:
            raise BatchError("TDS exited and no successor appeared within 30 seconds")
        ready = request.is_file() if request is not None else now >= trigger_time
        if ready and len(pids) == 1:
            start = process_start(pids[0])
            if not start:
                raise BatchError("cannot verify the TDS process start time")
            return pids[0], start
        time.sleep(0.5)
    raise BatchError("capture trigger or an unambiguous TDS process did not arrive before the timeout")


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
            preflight=False, fixture=None, replay=None, scene_bridge=False, submission_scope=False,
            live_catalog=None, live_only=False, live_static_meshes=False, live_lights=False,
            manual_trigger=False, steam_launch=False, resume_bedroom=False, readonly_saves=False,
            isolated_saves=False, gpu_metrics=False, preview_pbr=False, preview_candle=False,
            candle_link_diagnostics=False, wall_displacement=False, preview_gi=False, sample_interval_ms=None,
            remaster_toggle=False):
    if submission_scope and not scene_bridge:
        raise BatchError("submission scope requires the scene bridge probe")
    if live_only and (live_catalog is None or not terminate):
        raise BatchError("live-only mode requires a catalog and termination after the timed test")
    if manual_trigger and live_only:
        raise BatchError("manual capture trigger cannot be used in live-only mode")
    if resume_bedroom and not steam_launch:
        raise BatchError("bedroom resume requires Steam launch")
    if readonly_saves and not steam_launch:
        raise BatchError("read-only save guard requires Steam launch")
    if isolated_saves and (not steam_launch or not terminate or readonly_saves):
        raise BatchError("isolated saves require Steam launch and termination, without the read-only option")
    if live_static_meshes and live_catalog is None:
        raise BatchError("static mesh sampling requires an offline v4, v5 or static-enabled v6 live catalog")
    if live_lights and live_catalog is None:
        raise BatchError("light candidate sampling requires a live catalog")
    if gpu_metrics and live_catalog is None:
        raise BatchError("GPU metrics require a live viewer catalog")
    if preview_pbr and live_catalog is None:
        raise BatchError("PBR preview requires a live viewer catalog")
    if preview_candle and (live_catalog is None or not live_lights or preview_pbr):
        raise BatchError("candle preview requires the live light sampler and excludes the headlight preview")
    if candle_link_diagnostics and (live_catalog is None or not live_lights):
        raise BatchError("candle link diagnostics require the live light sampler")
    if wall_displacement and live_catalog is None:
        raise BatchError("wall displacement requires a height-enabled live catalog")
    if preview_gi and live_catalog is None:
        raise BatchError("GI preview requires a height-enabled live catalog")
    engine, bottle, system, proxy, output = validate_paths(engine, bottle, system, proxy, output)
    steam = bottle / "drive_c/Program Files (x86)/Steam/steam.exe"
    if steam_launch and (not steam.is_file() or system !=
                         steam.parent / "steamapps/common/Thief Deadly Shadows/System"):
        raise BatchError("Steam launch requires the installed Thief Steam System directory")
    if readonly_saves or isolated_saves:
        if sys.platform != "darwin" or not Path("/usr/bin/sandbox-exec").is_file():
            raise BatchError("save isolation requires macOS sandbox-exec")
        if readonly_saves:
            save_profile = readonly_save_profile(system)
        if wine_or_steam_pids():
            raise BatchError("close existing Wine and Steam processes before using save isolation")
    if sample_interval_ms is not None and (
            type(sample_interval_ms) is not int or not 1000 <= sample_interval_ms <= 10000 or
            live_catalog is None or not scene_bridge or not submission_scope or not manual_trigger or live_only):
        raise BatchError("distributed sampling requires a 1000–10000 ms interval, live viewer and manual trigger")
    if remaster_toggle and (not isolated_saves or not manual_trigger or not preview_candle or
                            not candle_link_diagnostics or sample_interval_ms is not None or live_only or
                            preview_pbr or wall_displacement or preview_gi or live_static_meshes or frames != 3):
        raise BatchError("remaster toggle requires isolated saves, manual capture and the candle-link preview only")
    if (not math.isfinite(delay) or not math.isfinite(timeout) or delay < 0 or timeout <= delay or
            type(frames) is not int or not (2 if sample_interval_ms else 1) <= frames <= (15 if sample_interval_ms else 3)):
        raise BatchError("invalid delay, timeout or capture frame count")
    if (fixture is None) != (replay is None):
        raise BatchError("the synthetic fixture and DX12 replay must be supplied together")
    if live_catalog is not None:
        if not scene_bridge or not submission_scope:
            raise BatchError("live viewer requires the scene bridge and submission scope")
        live_catalog = Path(live_catalog).expanduser()
        if (not live_catalog.is_absolute() or any(c in str(live_catalog) for c in (":", "\\", "\n", "\r"))
                or not live_catalog.is_file()):
            raise BatchError("live catalog must be an existing absolute local file")
        live_catalog = live_catalog.resolve()
        if wall_displacement or preview_gi:
            with live_catalog.open("rb") as stream:
                if stream.read(8)!=b"T3LCAT06":
                    raise BatchError("wall displacement and GI preview require a v6 height-enabled live catalog")
        if live_static_meshes:
            with live_catalog.open("rb") as stream:
                magic = stream.read(8)
                if magic not in (b"T3LCAT04", b"T3LCAT05") and not (
                        magic == b"T3LCAT06" and stream.read(4) == b"\x01\x00\x00\x00"):
                    raise BatchError("static mesh sampling requires a v4, v5 or static-enabled v6 live catalog")
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
    env, capture = env_for(engine, bottle, output, frames, scene_bridge=scene_bridge,
                           submission_scope=submission_scope, live_catalog=live_catalog, live_only=live_only,
                           live_static_meshes=live_static_meshes, live_lights=live_lights, gpu_metrics=gpu_metrics,
                           preview_pbr=preview_pbr, preview_candle=preview_candle,
                           candle_link_diagnostics=candle_link_diagnostics, wall_displacement=wall_displacement,
                           preview_gi=preview_gi, sample_interval_ms=sample_interval_ms, remaster_toggle=remaster_toggle)
    executable = "C:\\" + str((system / "T3Main.exe").relative_to((bottle / "drive_c").resolve())).replace("/", "\\")
    deployment = {"schema": 1, "proxy": str(target), "sha256": digest(proxy),
                  "capture_config": capture, "proxy_installed": False, "temporary_proxy_removed": None}
    (output / "deployment.json").write_text(json.dumps(deployment, indent=2) + "\n", encoding="utf-8")
    installed = False
    sandbox = None
    launcher = None
    game_seen = False
    trigger_pid = None
    trigger_start = None
    game_restarted = None
    termination_done = False
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
        if (readonly_saves or isolated_saves) and wine_or_steam_pids():
            raise BatchError("Wine or Steam started during preflight; no proxy was installed")
        if isolated_saves:
            sandbox = IsolatedSavePrefix.prepare(bottle, system)
            env = sandbox.environment(env)
        expected, identity = install_proxy(proxy, target, deployment["sha256"])
        installed = True
        deployment["proxy_installed"] = True
        deployment["file_identity"] = {"device": identity[0], "inode": identity[1]}
        (output / "deployment.json").write_text(json.dumps(deployment, indent=2) + "\n", encoding="utf-8")
        if expected != deployment["sha256"]:
            raise BatchError("proxy changed while it was installed")
        with (output / "wine.log").open("xb") as log:
            launch_target = [r"C:\Program Files (x86)\Steam\steam.exe", "-silent", "-applaunch", "6980"] if steam_launch else [executable]
            if resume_bedroom:
                launch_target.append(BEDROOM_SAVE_URL)
            command = [str(engine / "engine/bin/wine"), r"C:\windows\explorer.exe",
                       "/desktop=T3RenderTDS,1280x800", *launch_target]
            if readonly_saves:
                command = readonly_save_command(command, save_profile, env)
            if sandbox:
                command = sandbox.command(command, env)
                # Even a failed launch can have started the private wineserver.
                sandbox.started = True
            launcher = subprocess.Popen(command, cwd=system, env=env,
                                        stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + timeout
        crash_monitor = WineMainThreadCrash(output / "wine.log", executable)
        game_pid = wait_for_one_game(deadline, crash_monitor)
        game_seen = True
        game_start = process_start(game_pid)
        if not game_start:
            raise BatchError("cannot verify the TDS process start time")
        trigger_time = time.monotonic() + delay
        message = ("TDS avviato: cattura in attesa della richiesta." if manual_trigger else
                   f"TDS avviato: prova live per {delay:g} secondi." if live_only else
                   f"TDS avviato: cattura automatica tra {delay:g} secondi.")
        print(f"{message} Carica il salvataggio e resta nella bedroom.", flush=True)
        trigger_pid, trigger_start = wait_for_trigger(
            deadline, crash_monitor, request=output / "capture.request" if manual_trigger else None,
            trigger_time=None if manual_trigger else trigger_time,
            abort=output / "remaster.failed" if remaster_toggle else None)
        game_restarted = (trigger_pid, trigger_start) != (game_pid, game_start)
        crash_monitor.check()
        require_same_game(trigger_pid, trigger_start)
        if not live_only:
            (output / "capture.ready").touch(exist_ok=False)
            trace = output / "bedroom.jsonl"
            while time.monotonic() < deadline and not complete_trace(trace, frames):
                crash_monitor.check()
                require_same_game(trigger_pid, trigger_start)
                time.sleep(0.5)
            if not complete_trace(trace, frames):
                raise BatchError("capture did not complete before the timeout")
            crash_monitor.check()
            if sample_interval_ms:
                # The worker presents after the final D3D8 trace record is closed.
                time.sleep(2)
                require_same_game(trigger_pid, trigger_start)
            if remaster_toggle:
                toggle_deadline = min(deadline, time.monotonic() + 120)
                while time.monotonic() < toggle_deadline and not (output / "remaster.complete").is_file():
                    if (output / "remaster.failed").is_file():
                        raise BatchError("remaster comparison watcher reported a failure")
                    crash_monitor.check()
                    require_same_game(trigger_pid, trigger_start)
                    time.sleep(.5)
                if not (output / "remaster.complete").is_file():
                    raise BatchError("remaster comparison did not complete before the timeout")
        if not loader_verified(output / "wine.log", executable.rsplit("\\", 1)[0] + r"\d3d8.dll"):
            raise BatchError("Wine log does not establish that the expected proxy and builtin D3D8 were loaded")
        if not live_only:
            summary = analyze_tds_capture.analyze(trace)
            with (output / "summary.json").open("x", encoding="utf-8") as result_file:
                result_file.write(json.dumps(summary, indent=2) + "\n")
        status = "PARTIAL"
        print("Prova live terminata." if live_only else
              f"Cattura completata: {frames} frame. Analisi: {output / 'summary.json'}", flush=True)
        if terminate:
            # Isolated cleanup stops the private wineserver in finally; it
            # never signals a globally discovered PID of uncertain ownership.
            if not sandbox:
                if game_restarted:
                    raise BatchError("TDS restarted before capture; automatic termination withheld; close TDS and run cleanup")
                terminate_game(time.monotonic() + 10, trigger_pid, trigger_start)
            termination_done = True
        else:
            print("Chiudi TDS normalmente per rimuovere il proxy temporaneo.", flush=True)
            while game_pids():
                time.sleep(1)
    except (BatchError, SandboxError, OSError, ValueError, subprocess.SubprocessError, KeyboardInterrupt) as exc:
        reason = str(exc) or type(exc).__name__
    finally:
        if (installed and terminate and not termination_done and not sandbox and trigger_pid is not None and
                trigger_start and game_restarted is False):
            try:
                # On a post-trigger failure, close only the same process that
                # was identified at the trigger. Never terminate a replacement.
                terminate_game(time.monotonic() + 10, trigger_pid, trigger_start)
            except (BatchError, OSError) as exc:
                reason = (reason + "; " if reason else "") + str(exc)
        if sandbox:
            try:
                sandbox.close(engine, env)
            except (SandboxError, OSError) as exc:
                reason = (reason + "; " if reason else "") + str(exc)
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
        if reason:
            status = "FAILED"
        result = {"schema": 1, "status": status, "error": reason or None,
                  "scene_confirmation": "unconfirmed", "frames_requested": 0 if live_only else frames,
                  "scene_bridge_requested": scene_bridge,
                  "submission_scope_requested": submission_scope,
                  "live_viewer_requested": live_catalog is not None,
                  "live_static_meshes_requested": live_static_meshes,
                  "live_light_candidates_requested": live_lights,
                  "gpu_metrics_requested": gpu_metrics,
                  "preview_pbr_requested": preview_pbr,
                  "preview_candle_requested": preview_candle,
                  "candle_link_diagnostics_requested": candle_link_diagnostics,
                  "live_only": live_only,
                  "manual_trigger": manual_trigger, "steam_launch": steam_launch, "resume_bedroom": resume_bedroom,
                  "sample_interval_ms": sample_interval_ms,
                  "remaster_toggle": remaster_toggle,
                  "game_restarted_before_capture": game_restarted,
                  "readonly_saves": readonly_saves, "isolated_saves": isolated_saves,
                  "sandbox_removed": sandbox.removed if sandbox else None,
                  "sandbox_retained": str(sandbox.root) if sandbox and not sandbox.removed else None,
                  "original_saves_unchanged": sandbox.saves_unchanged if sandbox else None,
                  "host_preflight": host_preflight, "highball_preflight": highball_preflight,
                  "proxy_installed": deployment["proxy_installed"],
                  "temporary_proxy_removed": deployment["temporary_proxy_removed"],
                  "limitations": ["Live-only mode produces no D3D8 trace." if live_only else
                                  f"{frames} frame(s) use one experimental configuration, not an A/B comparison.",
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
    run.add_argument("--frames", type=int, default=3, help="1–3 consecutive frames or 2–15 distributed samples")
    run.add_argument("--sample-interval-ms", type=int, help="opt-in distributed capture interval, 1000–10000 ms")
    run.add_argument("--remaster-toggle", action="store_true", help="wait for the same-session flat/remaster comparison")
    run.add_argument("--scene-bridge", action="store_true", help="enable the experimental read-only scene bridge probe")
    run.add_argument("--submission-scope", action="store_true", help="tag draws inside the verified section callsite; requires --scene-bridge")
    run.add_argument("--live-catalog", help="enable the experimental D3D12 viewer with this offline catalog; requires bridge and submission scope")
    run.add_argument("--live-only", action="store_true", help="timed viewer test without the heavy D3D8 trace; requires catalog and termination")
    run.add_argument("--live-static-meshes", action="store_true", help="enable expensive experimental static mesh sampling; requires a v4, v5 or static-enabled v6 catalog")
    run.add_argument("--live-lights", action="store_true", help="sample candidate visible lights; requires a live catalog")
    run.add_argument("--gpu-metrics", action="store_true", help="sample optional viewer GPU timing and memory; requires a live catalog")
    run.add_argument("--preview-pbr", action="store_true", help="use a viewer-only headlight for PBR preview; requires a live catalog")
    run.add_argument("--preview-candle", action="store_true", help="use the sampled candle candidate with artistic lighting; requires --live-lights")
    run.add_argument("--candle-link-diagnostics", action="store_true", help="sample the bedroom candle's visual actor link; requires --live-lights")
    run.add_argument("--wall-displacement", action="store_true", help="use experimental displaced head-wall geometry; requires a v6 catalog")
    run.add_argument("--preview-gi", action="store_true", help="use experimental screen-space wall-to-bed bounce; requires a v6 catalog")
    run.add_argument("--manual-trigger", action="store_true", help="wait for output-dir/capture.request before recording")
    run.add_argument("--steam-launch", action="store_true", help="start the installed Steam game (app 6980) in the virtual desktop")
    run.add_argument("--resume-bedroom", action="store_true", help="pass the observed bedroom Current Save URL through Steam")
    run.add_argument("--readonly-saves", action="store_true", help="deny writes to SaveGames in the launched macOS Wine session")
    run.add_argument("--isolated-saves", action="store_true", help="use a temporary writable save copy and offline guarded Wine prefix")
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
                    args.preflight, args.fixture, args.dx12_replay, args.scene_bridge, args.submission_scope,
                    args.live_catalog, args.live_only, args.live_static_meshes, args.live_lights,
                    args.manual_trigger, args.steam_launch, args.resume_bedroom, args.readonly_saves, args.isolated_saves,
                    args.gpu_metrics, args.preview_pbr, args.preview_candle, args.candle_link_diagnostics, args.wall_displacement,
                    args.preview_gi, args.sample_interval_ms, args.remaster_toggle)
    except (BatchError, SandboxError, OSError, ValueError, KeyError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
