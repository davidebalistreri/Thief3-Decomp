#!/usr/bin/env python3
"""Run the isolated D3D8 identity fixture on Highball, optionally with the proxy.

This uses the given bottle but never launches TDS. Output goes to a new directory;
the bottle can still be updated by Wine itself. Results are runtime-specific.
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


GROUPS = {
    "device.GetBackBuffer": ("device.GetBackBuffer", "IDirect3DSwapChain8"),
    "device.GetRenderTarget": ("device.GetRenderTarget", "IDirect3DSwapChain8"),
    "device.GetDepthStencilSurface": ("device.GetDepthStencilSurface", "IDirect3DSwapChain8"),
    "device.CreateImageSurface": ("device.CreateImageSurface", "IDirect3DSwapChain8"),
    "device.CreateRenderTarget": ("device.CreateRenderTarget", "IDirect3DSwapChain8"),
    "device.CreateDepthStencilSurface": ("device.CreateDepthStencilSurface", "IDirect3DSwapChain8"),
    "texture.GetSurfaceLevel": ("texture.GetSurfaceLevel", "IDirect3DTexture8"),
    "cube.GetCubeMapSurface": ("cube.GetCubeMapSurface", "IDirect3DCubeTexture8"),
    "volume_texture.GetVolumeLevel": ("volume_texture.GetVolumeLevel", "IDirect3DVolumeTexture8"),
    "swapchain.GetBackBuffer": ("swapchain.GetBackBuffer", "IDirect3DSwapChain8"),
}
ROOTS = {
    "device.CreateTexture": ("texture.GetDevice", "texture.GetSurfaceLevel"),
    "device.CreateCubeTexture": ("cube.GetDevice", "cube.GetCubeMapSurface"),
    "device.CreateVolumeTexture": ("volume_texture.GetDevice", "volume_texture.GetVolumeLevel"),
    "device.CreateAdditionalSwapChain": ("swapchain.QueryInterface(IDirect3DDevice8)", "swapchain.GetBackBuffer"),
}
HEXPTR = re.compile(r"0x[0-9a-fA-F]{8}\Z")


def results_from_log(path):
    records = []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith('{"event":'):
            continue
        try:
            records.append(json.loads(line))
        except ValueError as exc:
            raise RunError(f"invalid fixture result: {exc}") from exc
    if (not records or records[0].get("event") != "header" or records[0].get("schema") != 1 or
            records[-1] != {"event": "end", "valid": True} or
            not HEXPTR.fullmatch(str(records[0].get("device"))) or
            not HEXPTR.fullmatch(str(records[0].get("device_iunknown")))):
        raise RunError("incomplete or invalid fixture results")
    indexed = {}
    for record in records[1:-1]:
        event = record.get("event")
        if event in ("clear", "present"):
            key = (event, None, None)
        elif event in ("create", "device", "container", "operation", "resource_identity"):
            if not isinstance(record.get("path"), str) or not record["path"]:
                raise RunError("fixture event has no path")
            if event == "container" and (not isinstance(record.get("iid"), str) or not record["iid"]):
                raise RunError("container result has no IID")
            key = (event, record.get("path"), record.get("iid"))
        else:
            raise RunError("unknown fixture event")
        if key in indexed or not isinstance(record.get("success"), bool) or not HEXPTR.fullmatch(str(record.get("hr"))):
            raise RunError("duplicate or malformed fixture result")
        if record["success"] != (int(record["hr"], 16) < 0x80000000):
            raise RunError("fixture success conflicts with HRESULT")
        if event == "device" and record["success"]:
            if (not HEXPTR.fullmatch(str(record.get("interface"))) or not HEXPTR.fullmatch(str(record.get("iunknown"))) or
                    any(not isinstance(record.get(field), bool) for field in ("same_interface", "same_iunknown", "same_vtable"))):
                raise RunError("invalid device identity result")
            if (record["same_interface"] != (record["interface"] == records[0]["device"]) or
                    record["same_iunknown"] != (record["iunknown"] == records[0]["device_iunknown"]) or
                    (record["same_interface"] and not record["same_vtable"])):
                raise RunError("device identity comparison is inconsistent")
        if event == "container" and record["success"]:
            if (not HEXPTR.fullmatch(str(record.get("iunknown"))) or
                    not isinstance(record.get("same_device_iunknown"), bool) or
                    (record.get("same_parent_iunknown") is not None and
                     not isinstance(record.get("same_parent_iunknown"), bool))):
                raise RunError("invalid container identity result")
            if record["same_device_iunknown"] != (record["iunknown"] == records[0]["device_iunknown"]):
                raise RunError("container identity comparison is inconsistent")
        if event == "resource_identity" and record["success"]:
            if not HEXPTR.fullmatch(str(record.get("iunknown"))) or not isinstance(record.get("same_parent_iunknown"), bool):
                raise RunError("invalid resource identity result")
        if event == "create" and record["success"] and record.get("object_nonnull") is not True:
            raise RunError("successful resource creation returned null")
        indexed[key] = record

    def required(event, path, iid=None):
        key = (event, path, iid)
        if key not in indexed:
            raise RunError(f"missing fixture result: {key}")
        return indexed[key]

    def required_success(event, path, iid=None):
        record = required(event, path, iid)
        if not record["success"]:
            raise RunError(f"fixture call failed: {path} ({record['hr']})")
        return record

    if not required("clear", None)["success"] or not required("present", None)["success"]:
        raise RunError("fixture Clear or Present failed")
    queried = required_success("device", "device.QueryInterface(IDirect3DDevice8)")
    if not queried["same_iunknown"]:
        raise RunError("device QueryInterface returned a different IUnknown identity")
    for path in ("device.GetBackBuffer", "device.GetRenderTarget"):
        required_success("create", path)
    for path, (device_path, child) in ROOTS.items():
        if required("create", path)["success"]:
            if path == "device.CreateAdditionalSwapChain":
                required("device", device_path)  # QI of swapchain to device may be unsupported.
            else:
                required_success("device", device_path)
            required("create", child)
    if required("create", "device.CreateTexture")["success"]:
        bound = required("operation", "device.SetTexture")
        if bound["success"]:
            texture = required_success("create", "device.GetTexture")
            required("operation", "device.SetTexture(null)")
            if texture["success"]:
                required_success("device", "device.GetTexture.GetDevice")
                required("resource_identity", "device.GetTexture.IUnknown")
    for create, (path, iid) in GROUPS.items():
        record = indexed.get(("create", create, None))
        # Child resources are absent only when their parent creation failed.
        if record is None and create not in ROOTS:
            if create.startswith(("texture.", "cube.", "volume_texture.", "swapchain.")):
                continue
            raise RunError(f"missing fixture creation: {create}")
        if record and record["success"]:
            required_success("device", path + ".GetDevice")
            for target in ("IUnknown", "IDirect3DDevice8", iid):
                container = required("container", path + ".GetContainer", target)
                if target == "IDirect3DDevice8" and container["success"]:
                    required("device", path + ".GetContainer:IDirect3DDevice8")
                if target == "IUnknown" and container["success"] and container["same_device_iunknown"]:
                    required_success("device", path + ".GetContainer:IUnknown.QueryInterface(IDirect3DDevice8)")
    suffix = ":QI(IUnknown).QI(IDirect3DDevice8)"
    for (event, path, _), record in list(indexed.items()):
        if event == "device" and record["success"] and not path.endswith(suffix):
            round_trip = required_success("device", path + suffix)
            if round_trip["iunknown"] != record["iunknown"]:
                raise RunError(f"device IUnknown round trip changed COM identity: {path}")
    return records


def validate_proxy_trace(path, expected_device):
    summary = trace_summary.summarize(path)
    events = [json.loads(line) for line in Path(path).read_text(encoding="utf-8").splitlines()]
    if [record.get("event") for record in events] != ["header", "frame_begin", "clear", "clear_result", "frame_end", "capture_end"]:
        raise RunError("proxy trace is not the fixture's single clear/present frame")
    header, begin, clear, clear_result, end, capture_end = events
    capability = header.get("buffer_capture")
    def success(value):
        return (isinstance(value, int) and not isinstance(value, bool) and -(1 << 31) <= value < (1 << 32)
                and (value & 0x80000000) == 0)
    trace_device = begin.get("device")
    if (summary["frame_count"] != 1 or summary["draw_count"] != 0 or
            not isinstance(capability, dict) or capability.get("requested") is not True or
            begin.get("frame") != 0 or not isinstance(trace_device, str) or
            re.fullmatch(r"0x[0-9a-fA-F]{1,8}", trace_device) is None or
            int(trace_device, 16) != int(expected_device, 16) or
            clear.get("frame") != 0 or clear.get("op_id") != 1 or clear.get("flags") != 1 or
            clear.get("color") != 0xff000000 or clear.get("depth_bits") != 0x3f800000 or
            clear_result.get("frame") != 0 or clear_result.get("op_id") != 1 or not success(clear_result.get("hr")) or
            end.get("frame") != 0 or end.get("draws") != 0 or end.get("concurrent_operations") is not False or
            not success(end.get("present_hr")) or capture_end.get("frames") != 1):
        raise RunError("proxy trace does not match a successful buffer-opt-in fixture frame")
    return summary


def execute(engine, bottle, fixture, output, timeout, proxy=None):
    engine, bottle, fixture, output = map(valid_path, (engine, bottle, fixture, output))
    proxy = valid_path(proxy) if proxy else None
    wine, server, frameworks = engine / "engine/bin/wine", engine / "engine/bin/wineserver", engine / "frameworks"
    if not all(path.is_file() and os.access(path, os.X_OK) for path in (wine, server)) or not frameworks.is_dir():
        raise RunError(f"invalid Highball engine: {engine}")
    z_drive = bottle / "dosdevices/z:"
    if not bottle.is_dir() or not z_drive.is_symlink() or os.readlink(z_drive) != "/":
        raise RunError("bottle must map Z: to /")
    check_pe32(fixture, False)
    if proxy:
        check_pe32(proxy, True)
    if not math.isfinite(timeout) or timeout <= 0:
        raise RunError("timeout must be positive and finite")
    for protected in (engine, bottle, fixture.parent, *([proxy.parent] if proxy else [])):
        if output.resolve() == protected.resolve() or protected.resolve() in output.resolve().parents:
            raise RunError("output must be outside the engine, bottle and binary directories")
    try:
        output.mkdir()
    except OSError as exc:
        raise RunError(f"cannot create new output directory: {exc}") from exc
    env = {key: value for key, value in os.environ.items() if not key.startswith("T3RENDER_CAPTURE_")}
    for key in ("WINEDLLPATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"):
        env.pop(key, None)
    env.update({"WINEPREFIX": str(bottle), "WINESERVER": str(server),
                "WINEDLLPATH_PREPEND": f"{bottle}/.highball-dlss/wine:{engine}/renderers/d3dmetal-tsshim/wine:{engine}/renderers/d3dmetal/wine",
                "DYLD_FALLBACK_LIBRARY_PATH": f"{frameworks}:{frameworks}/GStreamer.framework/Versions/1.0/lib",
                "DYLD_FALLBACK_FRAMEWORK_PATH": str(frameworks), "WINEDEBUG": "-all"})
    result = {"schema": 1, "test": "synthetic_d3d8_resource_identity", "status": "FAIL",
              "engine": str(engine), "bottle": str(bottle), "fixture_sha256": sha256(fixture),
              "proxy_sha256": sha256(proxy) if proxy else None, "runs": {},
              "limitations": ["Synthetic resources only; TDS is not launched.",
                              "Results apply only to this runtime, adapter, flags and resource types.",
                              "Interface and vtable comparisons do not prove future calls are intercepted.",
                              "No buffer wrappers are passed to a device and no gameplay parity is tested."]}
    stage = "setup"
    try:
        for stage in (["baseline", "proxy"] if proxy else ["baseline"]):
            directory = output / stage
            directory.mkdir()
            executable = directory / fixture.name
            shutil.copyfile(fixture, executable)
            stage_env = env.copy()
            stage_env["WINEDLLOVERRIDES"] = "d3d8=b;nvapi64,nvngx=n,b;winemenubuilder.exe=d"
            if stage == "proxy":
                shutil.copyfile(proxy, directory / "d3d8.dll")
                stage_env.update({"WINEDLLOVERRIDES": "d3d8=n,b;nvapi64,nvngx=n,b;winemenubuilder.exe=d",
                                  "T3RENDER_CAPTURE_FRAME": "0", "T3RENDER_CAPTURE_BUFFERS": "1",
                                  "T3RENDER_CAPTURE_PATH": windows_path(output / "capture.jsonl")})
            log = output / (stage + ".log")
            run_stage(stage, [wine, executable], directory, stage_env, log, timeout)
            result["runs"][stage] = {"log": log.name, "log_sha256": sha256(log), "observations": results_from_log(log)}
            if stage == "proxy":
                trace = output / "capture.jsonl"
                validate_proxy_trace(trace, result["runs"][stage]["observations"][0]["device"])
                result["runs"][stage]["trace_sha256"] = sha256(trace)
        result["status"] = "PASS"
    except (RunError, OSError, ValueError, trace_summary.TraceError) as exc:
        result["failed_stage"] = stage
        result["error"] = str(exc)
    try:
        with (output / "result.json").open("x", encoding="utf-8") as destination:
            json.dump(result, destination, indent=2, ensure_ascii=False)
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
    parser.add_argument("--proxy", help="optional x86 d3d8.dll to test with buffer observation enabled")
    parser.add_argument("--output-dir", required=True, help="new absolute directory outside engine, bottle and binaries")
    parser.add_argument("--timeout", type=float, default=45)
    args = parser.parse_args(argv)
    try:
        execute(args.engine, args.bottle, args.fixture, args.output_dir, args.timeout, args.proxy)
    except RunError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"PASS: synthetic identity observations written to {args.output_dir}/result.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
