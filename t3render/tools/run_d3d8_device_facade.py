#!/usr/bin/env python3
"""Compare isolated native and device-facade D3D8 fixture processes in Highball.

The fixture creates native resources whose GetDevice escapes the facade. It
does not launch TDS and does not pass wrapped buffers to the escaped device.
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


HEADER = b"P6\n256 256\n255\n"
PPM_SIZE = len(HEADER) + 256 * 256 * 3
HEX_HRESULT = re.compile(r"0x[0-9a-fA-F]{8}\Z")


def expected_parity():
    """Expected operations and concrete state; None means a successful HRESULT."""
    expected = [(name, None) for name in ("GetAdapterDisplayMode", "CreateDevice", "GetDirect3D",
                 "FactoryQIUnknown", "FactoryQIRoundtrip", "DeviceQIUnknown", "DeviceQIRoundtrip",
                 "CreateVertexBuffer", "CreateIndexBuffer", "CreateTexture", "GetBackBuffer",
                 "VertexLock", "VertexUnlock", "IndexLock", "IndexUnlock")]
    for index, name in enumerate(("World", "View", "Projection"), 1):
        values = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                  0.0, 0.0, 1.0, 0.0, float(index), 0.0, 0.0, 1.0]
        bits = "".join(f"{struct.unpack('<I', struct.pack('<f', value))[0]:08x}" for value in values)
        expected.extend([(f"SetTransform{name}", None), (f"GetTransform{name}", None), (name, bits)])
    expected.extend([("SetViewport", None), ("GetViewport", None), ("Viewport", "0,0,256,256,0.0,1.0")])
    for name, value in (("Lighting", 0), ("Cull", 1), ("ZEnable", 0), ("AlphaBlend", 0),
                        ("Dither", 0), ("Fog", 0)):
        expected.extend([(f"SetRender{name}", None), (f"GetRender{name}", None), (f"Render{name}", str(value))])
    for name, value in (("ColorOp", 2), ("ColorArg1", 0), ("AlphaOp", 2), ("AlphaArg1", 0)):
        expected.extend([(f"SetStage{name}", None), (f"GetStage{name}", None), (f"Stage{name}", str(value))])
    expected.extend([(name, None) for name in ("SetVertexShader", "GetVertexShader", "SetPixelShader")])
    expected.append(("FVF", "68"))
    expected.extend([(name, None) for name in ("SetTexture", "GetTexture", "ClearTexture", "SetStreamSource",
                                                "SetIndices", "GetStreamSource", "GetIndices")])
    expected.append(("Bindings stride", "20 base=0"))
    expected.extend([(name, None) for name in ("Clear", "BeginScene", "DrawPrimitive", "DrawIndexedPrimitive",
                                                "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "EndScene",
                                                "BackBufferDesc", "BackBufferLock", "BackBufferUnlock")])
    expected.extend([("BackBufferFormat", "format"), ("Present", None)])
    return expected


def successful_hresult(value):
    return isinstance(value, str) and HEX_HRESULT.fullmatch(value) is not None and int(value, 16) < 0x80000000


def inspect_log(path, mode):
    lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    actual = []
    for line in lines:
        if line.startswith("parity "):
            key, separator, value = line[len("parity "):].partition("=")
            if not separator:
                raise RunError("malformed parity line")
            actual.append((key, value))
    expected = expected_parity()
    if len(actual) != len(expected) or [key for key, _ in actual] != [key for key, _ in expected]:
        raise RunError("fixture operations or state observations are incomplete or out of order")
    for (key, value), (_, wanted) in zip(actual, expected):
        if wanted is None:
            if not successful_hresult(value):
                raise RunError(f"{key} did not succeed: {value}")
        elif wanted == "format":
            if value not in ("21", "22"):
                raise RunError(f"unsupported backbuffer format: {value}")
        elif value != wanted:
            raise RunError(f"incorrect {key}: {value}")
    lifetime = ("factory_and_device_QI_roundtrip=PASS factory_initial_release=PASS" if mode == "facade" else
                "factory_and_device_QI_roundtrip=PASS factory_initial_release=not_applicable")
    if ([line for line in lines if line.startswith("factory_and_device_QI_roundtrip=")] != [lifetime] or
            [line for line in lines if line.startswith("device_facade_fixture=")] !=
            [f"device_facade_fixture=PASS mode={mode}"]):
        raise RunError("COM identity or fixture completion marker is missing")
    if mode == "facade":
        for name in ("surface", "texture", "vertex_buffer", "index_buffer"):
            prefix = f"resource_{name}_native_device_alias="
            matching = [line for line in lines if line.startswith(prefix)]
            expected_prefix = prefix + "PASS hr="
            if (len(matching) != 1 or not matching[0].startswith(expected_prefix) or
                    not successful_hresult(matching[0][len(expected_prefix):])):
                raise RunError(f"native device escape was not verified for {name}")
        if lines.count("resource_get_device_boundary=native_device; wrapped_buffers=not_tested") != 1:
            raise RunError("native resource boundary marker is missing")
    return actual


def read_ppm(path):
    with Path(path).open("rb") as image:
        data = image.read(PPM_SIZE + 1)
    if len(data) != PPM_SIZE or not data.startswith(HEADER):
        raise RunError("fixture PPM does not match the expected dimensions or encoding")
    for x, y, expected in ((30, 30, (228, 59, 59)), (225, 30, (53, 198, 101)),
                           (30, 225, (67, 138, 240)), (225, 160, (230, 201, 72))):
        offset = len(HEADER) + (y * 256 + x) * 3
        if tuple(data[offset:offset + 3]) != expected:
            raise RunError(f"fixture PPM triangle sample differs at {x},{y}")
    return data


def execute(engine, bottle, fixture, output, timeout):
    engine, bottle, fixture, output = map(valid_path, (engine, bottle, fixture, output))
    wine, server, frameworks = engine / "engine/bin/wine", engine / "engine/bin/wineserver", engine / "frameworks"
    if not all(path.is_file() and os.access(path, os.X_OK) for path in (wine, server)) or not frameworks.is_dir():
        raise RunError(f"invalid Highball engine: {engine}")
    z_drive = bottle / "dosdevices/z:"
    if not bottle.is_dir() or not z_drive.is_symlink() or os.readlink(z_drive) != "/":
        raise RunError("bottle must map Z: to /")
    check_pe32(fixture, False)
    if not math.isfinite(timeout) or timeout <= 0:
        raise RunError("timeout must be positive and finite")
    for protected in (engine, bottle, fixture.parent):
        if output.resolve() == protected.resolve() or protected.resolve() in output.resolve().parents:
            raise RunError("output must be outside the engine, bottle and fixture directory")
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
                "DYLD_FALLBACK_FRAMEWORK_PATH": str(frameworks), "WINEDEBUG": "-all",
                "WINEDLLOVERRIDES": "d3d8=b;nvapi64,nvngx=n,b;winemenubuilder.exe=d"})
    result = {"schema": 1, "test": "synthetic_d3d8_device_facade", "status": "FAIL",
              "engine": str(engine), "bottle": str(bottle), "fixture_sha256": sha256(fixture), "runs": {},
              "limitations": ["Fixture only: not integrated with the proxy or TDS.",
                              "Resources are native and GetDevice exposes the native device, bypassing the facade.",
                              "Wrapped buffers are not passed through the escaped device.",
                              "Four synthetic triangles and selected fixed-function states only; no gameplay parity."]}
    stage = "setup"
    try:
        executable = output / fixture.name
        shutil.copyfile(fixture, executable)
        observations, images = {}, {}
        for stage in ("baseline", "facade"):
            log, ppm = output / (stage + ".log"), output / (stage + ".ppm")
            run_stage(stage, [wine, executable, "--" + stage, windows_path(ppm)], output, env, log, timeout)
            observations[stage] = inspect_log(log, stage)
            images[stage] = read_ppm(ppm)
            result["runs"][stage] = {"log": log.name, "log_sha256": sha256(log), "ppm": ppm.name,
                                     "ppm_sha256": sha256(ppm), "parity_records": len(observations[stage])}
        stage = "compare"
        if observations["baseline"] != observations["facade"]:
            raise RunError("native and facade HRESULT/state observations differ")
        if images["baseline"] != images["facade"]:
            raise RunError("native and facade PPM streams differ")
        result["status"] = "PASS"
    except (RunError, OSError, ValueError) as exc:
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
    parser.add_argument("--output-dir", required=True, help="new absolute output directory")
    parser.add_argument("--timeout", type=float, default=45)
    args = parser.parse_args(argv)
    try:
        result = execute(args.engine, args.bottle, args.fixture, args.output_dir, args.timeout)
    except RunError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"PASS: identical PPM SHA-256 {result['runs']['baseline']['ppm_sha256']}; results: {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
