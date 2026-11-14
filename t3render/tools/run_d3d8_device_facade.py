#!/usr/bin/env python3
"""Compare isolated native and device-facade D3D8 fixture processes in Highball.

The optional fixtures add buffer and selected texture/surface facades. Other
resource paths remain incomplete. No fixture launches TDS or passes wrappers
to an escaped native device.
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
PRIVATE_KINDS = ("vertex", "index", "texture", "backbuffer", "surface_level", "cube", "volume_texture", "volume")
PRIVATE_RECORD = re.compile(
    r"private_data kind=(?P<kind>[a-z_]+) "
    r"missing=(?P<missing>0x[0-9a-fA-F]{8}) set=(?P<set>0x[0-9a-fA-F]{8}) "
    r"query=(?P<query>0x[0-9a-fA-F]{8}) query_size=(?P<query_size>\d+) "
    r"small=(?P<small>0x[0-9a-fA-F]{8}) small_size=(?P<small_size>\d+) "
    r"get=(?P<get>0x[0-9a-fA-F]{8}) match=(?P<match>[01]) free=(?P<free>0x[0-9a-fA-F]{8}) "
    r"after_free=(?P<after_free>0x[0-9a-fA-F]{8}) "
    r"object_set=(?P<object_set>0x[0-9a-fA-F]{8}) object_get=(?P<object_get>0x[0-9a-fA-F]{8}) "
    r"object_same=(?P<object_same>[01]) object_free=(?P<object_free>0x[0-9a-fA-F]{8}) "
    r"refs=(?P<refs>\d+,\d+,\d+,\d+)\Z")


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


def inspect_private_data(lines, mode):
    records = [PRIVATE_RECORD.fullmatch(line) for line in lines if line.startswith("private_data ")]
    if any(record is None for record in records) or tuple(record['kind'] for record in records) != PRIVATE_KINDS:
        raise RunError("private-data resource observations are missing, duplicated or malformed")
    observations = []
    for record in records:
        if mode == "facade":
            if (any(record[key].lower() != "0x80004001" for key in ("missing", "object_set", "object_get", "object_free")) or
                    successful_hresult(record['small']) or successful_hresult(record['after_free']) or
                    any(not successful_hresult(record[key]) for key in ("set", "query", "get", "free")) or
                    (record['query_size'], record['small_size'], record['match'], record['object_same'], record['refs']) !=
                    ("4", "4", "1", "0", "1,1,1,1")):
                raise RunError(f"unexpected private-data facade behavior for {record['kind']}")
        elif (successful_hresult(record['missing']) or successful_hresult(record['small']) or successful_hresult(record['after_free']) or
              any(not successful_hresult(record[key]) for key in ("set", "query", "get", "free", "object_set", "object_get", "object_free")) or
              (record['query_size'], record['small_size'], record['match'], record['object_same'], record['refs']) !=
              ("4", "4", "1", "1", "2,3,2,1")):
            raise RunError(f"unexpected native private-data behavior for {record['kind']}")
        for key in ("set", "query", "query_size", "small", "small_size", "get", "match", "free", "after_free"):
            observations.append((f"PrivateBlob.{record['kind']}.{key}", record[key].lower()))
    aliases = [line for line in lines if line.startswith("private_data_alias ")]
    expected = "private_data_alias set=0x00000000 get=0x00000000 same=1 parent=1 free=0x00000000"
    if aliases != [expected]:
        raise RunError("private-data resource-alias observation is missing or unexpected")
    indices = [line for line in lines if line.startswith("private_data_index_alias ")]
    if indices != ["private_data_index_alias set=0x00000000 get=0x00000000 same=1 parent=1"]:
        raise RunError("private-data index-buffer alias observation is missing or unexpected")
    return observations


def inspect_log(path, mode, buffer_facade=False, resource_facade=False):
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
    if resource_facade:
        # Compare observed blob operations; unknown/COM results intentionally
        # differ and are validated separately as a compatibility gap.
        actual.extend(inspect_private_data(lines, mode))
        lifetime = ("private_lifetime_naive_cycle_and_cleanup=PASS" if mode == "facade" else
                    "private_lifetime_native_dependent_release=PASS")
        if [line for line in lines if line.startswith("private_lifetime_")] != [lifetime]:
            raise RunError("private-data resource lifetime observation is missing or conflicting")
        if [line for line in lines if line.startswith("private_translated_lifetime=")] != ["private_translated_lifetime=PASS"]:
            raise RunError("translated private-data lifetime observation is missing or conflicting")
        for marker in ("resource_facade_textured_pixels=PASS", "resource_facade_stateblock_texture_identities=PASS",
                       "resource_facade_volume_roundtrips=PASS",
                       "resource_facade_surface_copy_and_target=PASS", "resource_facade_swapchain_roundtrips=PASS",
                       "resource_facade_reset_roundtrips=PASS", "resource_facade_reset_pixel=0xe43b3b",
                       "resource_facade_live_reset_roundtrips=PASS"):
            prefix = marker.partition("=")[0] + "="
            if [line for line in lines if line.startswith(prefix)] != [marker]:
                raise RunError(f"resource facade marker missing or conflicting: {marker}")
        for label, name in (("SwapChainContainerHRESULT", "container"), ("SwapChainDeviceHRESULT", "device"),
                            ("SwapChainContainerDeviceHRESULT", "container_device")):
            prefix = f"resource_facade_swapchain_{name}_hr="
            values = [line[len(prefix):] for line in lines if line.startswith(prefix)]
            if len(values) != 1 or HEX_HRESULT.fullmatch(values[0]) is None:
                raise RunError(f"swapchain {name} HRESULT is missing or invalid")
            actual.append((label, values[0]))
        prefix = "resource_facade_reset_hr="
        reset = [line[len(prefix):] for line in lines if line.startswith(prefix)]
        if len(reset) != 1 or not successful_hresult(reset[0]):
            raise RunError("reset HRESULT is missing or unsuccessful")
        actual.append(("ResetHRESULT", reset[0]))
        for label, name in (("LiveResetHRESULT", "live"), ("RecoveryResetHRESULT", "recovery")):
            prefix = f"resource_facade_{name}_reset_hr="
            values = [line[len(prefix):] for line in lines if line.startswith(prefix)]
            if (len(values) != 1 or HEX_HRESULT.fullmatch(values[0]) is None or
                    (name == "recovery" and not successful_hresult(values[0]))):
                raise RunError(f"{name} reset HRESULT is missing or invalid")
            actual.append((label, values[0]))
    if mode == "facade":
        names = (() if resource_facade else ("surface", "texture") if buffer_facade else
                 ("surface", "texture", "vertex_buffer", "index_buffer"))
        for name in names:
            prefix = f"resource_{name}_native_device_alias="
            matching = [line for line in lines if line.startswith(prefix)]
            expected_prefix = prefix + "PASS hr="
            if (len(matching) != 1 or not matching[0].startswith(expected_prefix) or
                    not successful_hresult(matching[0][len(expected_prefix):])):
                raise RunError(f"native device escape was not verified for {name}")
        if buffer_facade or resource_facade:
            markers = ["buffer_parent_facade_roundtrip=PASS", "buffer_shadow_writeonly_and_draw_window=PASS",
                       "buffer_stateblock_retained_identity=PASS", "buffer_facade_final_close=PASS"]
            if resource_facade:
                markers.extend(("resource_get_device_boundary=facade_for_tested_types; other_types=unverified",
                                "resource_facade_2d_cube_surface_roundtrips=PASS", "resource_facade_cube_binding=PASS",
                                "resource_facade_device_unwraps_buffers=PASS", "resource_facade_rejects_unknown_inputs=PASS",
                                "resource_facade_post_reset_buffer_epoch=PASS", "resource_facade_recovery_buffer_epoch=PASS",
                                "resource_facade_stateblock_buffer_shadows=PASS", "resource_facade_failed_stateblock_invalidates=PASS",
                                "resource_facade_final_close=PASS"))
            else:
                markers.append("resource_get_device_boundary=native_nonbuffer; wrapped_buffers=facade")
            for marker in markers:
                prefix = marker.partition("=")[0] + "="
                if [line for line in lines if line.startswith(prefix)] != [marker]:
                    raise RunError(f"facade marker missing or conflicting: {marker}")
        elif lines.count("resource_get_device_boundary=native_device; wrapped_buffers=not_tested") != 1:
            raise RunError("native resource boundary marker is missing")
    return actual


def read_ppm(path, resource_facade=False):
    with Path(path).open("rb") as image:
        data = image.read(PPM_SIZE + 1)
    if len(data) != PPM_SIZE or not data.startswith(HEADER):
        raise RunError("fixture PPM does not match the expected dimensions or encoding")
    samples = [(30, 30, (228, 59, 59)), (225, 30, (53, 198, 101)),
               (30, 225, (67, 138, 240)), (225, 160, (230, 201, 72))]
    if resource_facade:
        samples.extend(((230, 210, (18, 52, 208)), (160, 210, (36, 228, 206))))
    for x, y, expected in samples:
        offset = len(HEADER) + (y * 256 + x) * 3
        if tuple(data[offset:offset + 3]) != expected:
            raise RunError(f"fixture PPM triangle sample differs at {x},{y}")
    return data


def execute(engine, bottle, fixture, output, timeout, buffer_facade=False, resource_facade=False):
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
    test = "synthetic_d3d8_resource_facade" if resource_facade else (
        "synthetic_d3d8_buffer_facade" if buffer_facade else "synthetic_d3d8_device_facade")
    result = {"schema": 1, "test": test, "status": "FAIL",
              "engine": str(engine), "bottle": str(bottle), "fixture_sha256": sha256(fixture), "runs": {},
              "limitations": ["Fixture only: not integrated with the proxy or TDS.",
                              "Resources are native and GetDevice exposes the native device, bypassing the facade.",
                              "Wrapped buffers are not passed through the escaped device.",
                              "Four synthetic triangles and selected fixed-function states only; no gameplay parity."]}
    if buffer_facade:
        result["limitations"] = ["Fixture only: not integrated with the proxy or TDS.",
                                 "VB/IB GetDevice returns the facade; other resources expose the native device.",
                                 "Unknown private-data provenance, IUnknown data and unknown VB/IB interfaces fail closed; this differs from native behavior.",
                                 "One retained-vertex state-block path preserves identity but invalidates bytes; reset and runtime concurrency are not exercised.",
                                 "Four synthetic triangles and selected fixed-function states only; no gameplay parity."]
    if resource_facade:
        result["limitations"] = ["Fixture only: not integrated with the proxy or TDS.",
                                 "Only selected 2D, cube, volume, surface, swapchain and buffer paths were exercised; other interfaces remain unverified.",
                                 "Only known VB/IB IUnknown values from non-buffer resource facades are translated; other COM private data and unknown interfaces fail closed.",
                                 "One diagnostic intentionally stores a wrapped IUnknown on a borrowed native texture, then breaks the cycle; this is not an application path.",
                                 "Two reset scenarios were exercised; runtime concurrency and comprehensive state-block behavior remain untested.",
                                 "Six synthetic triangles and selected fixed-function states only; no gameplay parity."]
    stage = "setup"
    try:
        executable = output / fixture.name
        shutil.copyfile(fixture, executable)
        observations, images = {}, {}
        for stage in ("baseline", "facade"):
            log, ppm = output / (stage + ".log"), output / (stage + ".ppm")
            run_stage(stage, [wine, executable, "--" + stage, windows_path(ppm)], output, env, log, timeout)
            observations[stage] = inspect_log(log, stage, buffer_facade, resource_facade)
            images[stage] = read_ppm(ppm, resource_facade)
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
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--buffer-facade", action="store_true", help="validate the separate buffer facade fixture")
    group.add_argument("--resource-facade", action="store_true", help="validate the separate combined resource fixture")
    args = parser.parse_args(argv)
    try:
        result = execute(args.engine, args.bottle, args.fixture, args.output_dir, args.timeout,
                         args.buffer_facade, args.resource_facade)
    except RunError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"PASS: identical PPM SHA-256 {result['runs']['baseline']['ppm_sha256']}; results: {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
