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
import trace_to_up_packet


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
    if [line for line in lines if line.startswith("private_resource_self_and_cross_aliases=")] != [
            "private_resource_self_and_cross_aliases=PASS"]:
        raise RunError("private-data resource self/cross-reference observation is missing or conflicting")
    if [line for line in lines if line.startswith("private_buffer_sources=")] != ["private_buffer_sources=PASS"]:
        raise RunError("buffer private-data source observation is missing or conflicting")
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
        if [line for line in lines if line.startswith("private_buffer_sources_reset=")] != ["private_buffer_sources_reset=PASS"]:
            raise RunError("buffer private-data reset observation is missing or conflicting")
        external = ("private_external_naive_cycle_and_cleanup=PASS" if mode == "facade" else
                    "private_external_native_dependent_release=PASS")
        if [line for line in lines if line.startswith("private_external_") and not line.startswith("private_external_eager_")] != [external]:
            raise RunError("external private-data lifetime observation is missing or conflicting")
        eager = ("private_external_eager_release_and_cleanup=PASS" if mode == "facade" else
                 "private_external_eager_native_dependent_release=PASS")
        provenance = "preserved"
        if ([line for line in lines if line.startswith("private_external_eager_") and
             not line.startswith(("private_external_eager_reacquire_provenance=", "private_external_eager_stateblock_and_resource="))] != [eager] or
            [line for line in lines if line.startswith("private_external_eager_reacquire_provenance=")] !=
             [f"private_external_eager_reacquire_provenance={provenance}"] or
            [line for line in lines if line.startswith("private_external_eager_stateblock_and_resource=")] !=
             ["private_external_eager_stateblock_and_resource=PASS"]):
            raise RunError("eager-retirement lifetime or provenance observation is missing or conflicting")
        eager_buffer = ("eager_buffer_lifetime=PASS shadow=unavailable provenance=unavailable" if mode == "facade" else
                        "eager_buffer_lifetime=PASS shadow=not_observed provenance=preserved")
        if [line for line in lines if line.startswith("eager_buffer_lifetime=")] != [eager_buffer]:
            raise RunError("eager buffer lifetime or capture limitation is missing or conflicting")
        persistent = ("persistent_eager_buffer=PASS shadow=preserved provenance=preserved reset=unknown" if mode == "facade" else
                      "persistent_eager_buffer=PASS shadow=not_observed provenance=preserved reset=not_observed")
        if [line for line in lines if line.startswith("persistent_eager_buffer=")] != [persistent]:
            raise RunError("persistent eager buffer observation is missing or conflicting")
        prepared = ["prepared_eager_buffer=PASS"] if mode == "facade" else []
        if [line for line in lines if line.startswith("prepared_eager_buffer=")] != prepared:
            raise RunError("prepared getter observation is missing or conflicting")
        capture = ["prepared_capture_draw=PASS"] if mode == "facade" else []
        if [line for line in lines if line.startswith("prepared_capture_draw=")] != capture:
            raise RunError("prepared capture observation is missing or conflicting")
        shader = ["combined_capture_shader_pixel=PASS"] if mode == "facade" else []
        if [line for line in lines if line.startswith("combined_capture_shader_pixel=")] != shader:
            raise RunError("combined shader observation is missing or conflicting")
        hazard = ["combined_external_shader_invalidation=PASS"] if mode == "facade" else []
        if [line for line in lines if line.startswith("combined_external_shader_invalidation=")] != hazard:
            raise RunError("combined external-object invalidation is missing or conflicting")
        if [line for line in lines if line.startswith("private_cross_device_cycle=")] != ["private_cross_device_cycle=PASS"]:
            raise RunError("cross-device private-data observation is missing or conflicting")
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


def inspect_prepared_trace(path):
    try:
        records = [json.loads(line) for line in Path(path).read_text(encoding="utf-8").splitlines()]
    except (OSError, ValueError) as exc:
        raise RunError(f"invalid prepared buffer trace: {exc}") from exc
    inspect_prepared_records(records)


def inspect_prepared_records(records):
    try:
        if [record["event"] for record in records] != ["header", "frame_begin", "clear", "clear_result", "draw", "draw_result", "frame_end", "capture_end"]:
            raise ValueError("incomplete or unexpected event sequence")
        header, begin, clear, clear_result, draw, result, end, _ = records
        if (header.get("replayable") is not False or header.get("buffer_capture", {}).get("requested") is not True or
            header.get("declaration_capture", {}).get("requested") is not True or
            header.get("vertex_program_capture", {}).get("requested") is not True or
            header.get("vertex_constant_capture", {}).get("requested") is not True or
            begin.get("frame") != 1 or draw.get("frame") != 1 or
            clear.get("frame") != 1 or clear.get("rect_count") != 0 or clear.get("flags") != 1 or
            clear.get("color") != 0xff111318 or clear_result.get("frame") != 1 or
            clear_result.get("op_id") != clear.get("op_id") or clear_result.get("hr") != 0 or
            draw.get("call") != "DrawIndexedPrimitive" or result.get("frame") != 1 or
            result.get("op_id") != draw.get("op_id") or not draw.get("op_id") or result.get("hr") != 0 or
            result.get("buffer_stable") is not True or result.get("shader_stable") is not True or
            end.get("frame") != 1 or end.get("present_hr") != 0 or
            end.get("draws") != 1 or end.get("replayable") is not False):
            raise ValueError("invalid draw, result or frame")
        declaration = draw["vertex_declaration"]
        program = draw["vertex_program"]
        constants = draw["vertex_constants"]
        count = constants.get("count")
        if (declaration.get("status") != "candidate" or declaration.get("generation") != 1 or
            declaration.get("words") != [0x20000000, 0x40030000, 0x40040005, 0xffffffff] or
            program.get("status") != "candidate" or program.get("generation") != 1 or program.get("version") != "vs_1_1" or
            program.get("words") != [0xfffe0101, 1, 0xc00f0000, 0x90e40000, 1, 0xd00f0000, 0xa0e40000, 0xffff] or
            constants.get("status") != "sampled" or constants.get("start_register") != 0 or
            constants.get("getter_hr") != 0 or constants.get("verify_hr") != 0 or
            type(count) is not int or not 1 <= count <= 96 or not isinstance(constants.get("bits"), list) or
            len(constants["bits"]) != 4 * count or
            constants["bits"][:4] != [0, 0x3f800000, 0, 0x3f800000]):
            raise ValueError("shader declaration, program or constants differ")
        data = draw["buffer_data"]
        vertex, index = data["vertex"], data["index"]
        expected_vertex = b"".join(struct.pack("<ffffI", x, y, 0.5, 1.0, 0xffe43b3b)
                                   for x, y in ((-0.8, 0.8), (0.8, 0.8), (-0.8, -0.8)))
        if (data.get("source") != "application_lock_shadow" or vertex.get("status") != "captured" or
            vertex.get("data") != expected_vertex.hex() or vertex.get("byte_count") != len(expected_vertex) or
            (vertex.get("first_vertex"), vertex.get("vertex_count"), vertex.get("stride"), vertex.get("byte_offset")) != (0, 3, 20, 0) or
            index.get("status") != "captured" or index.get("data") != struct.pack("<3H", 0, 1, 2).hex() or
            (index.get("byte_count"), index.get("index_count"), index.get("index_format"), index.get("byte_offset")) != (6, 3, 101, 0)):
            raise ValueError("buffer snapshot differs from fixture data")
    except (ValueError, KeyError, TypeError, AttributeError) as exc:
        raise RunError(f"invalid prepared buffer trace: {exc}") from exc


def prepared_replay_packet(records, *, synthetic_fixture=False):
    if synthetic_fixture is not True:
        raise RunError("explicit synthetic fixture attestation is required")
    inspect_prepared_records(records)
    header, begin, clear, _, draw, _, end, capture_end = records
    require = trace_to_up_packet.require
    require(header.get("operation_results") == 1 and capture_end.get("frames") == 1,
            "fixture requires paired operations in one frame")
    require(end.get("concurrent_operations") is False and end.get("had_reset") is False,
            "fixture frame has concurrent operations or Reset")
    require(type(clear.get("op_id")) is int and type(draw.get("op_id")) is int and
            0 < clear["op_id"] < draw["op_id"], "operation order is invalid")
    require(clear.get("rects_status") == "none" and clear.get("rects") == [], "restricted clear is unsupported")
    presentation = begin.get("presentation")
    require(isinstance(presentation, dict) and presentation.get("width") == 256 and presentation.get("height") == 256 and
            presentation.get("format") in (21, 22) and presentation.get("multisample") == 0 and
            presentation.get("count") == 1 and presentation.get("windowed") is True,
            "unsupported backbuffer")
    viewport = draw.get("viewport")
    require(isinstance(viewport, dict) and viewport == {"x": 0, "y": 0, "width": 256, "height": 256,
                                                       "min_z": 0, "max_z": 1}, "unsupported viewport")
    require(draw.get("primitive_type") == 4 and draw.get("primitive_count") == 1 and
            draw.get("min_vertex_index") == 0 and draw.get("num_vertices") == 3 and draw.get("start_index") == 0 and
            draw.get("pixel_shader") == 0 and type(draw.get("vertex_shader")) is int and draw["vertex_shader"] != 0,
            "unsupported draw parameters or shader binding")
    trace_to_up_packet.require_states(draw)
    data = draw["buffer_data"]
    streams, indices = draw.get("streams"), draw.get("indices")
    require(isinstance(streams, list) and streams and isinstance(streams[0], dict) and isinstance(indices, dict) and
            streams[0].get("stream") == 0 and streams[0].get("hr") == 0 and streams[0].get("stride") == 20 and
            streams[0].get("buffer") == data["vertex"].get("resource") and streams[0].get("buffer") is not None and
            indices.get("hr") == 0 and indices.get("base_vertex_index") == 0 and
            indices.get("buffer") == data["index"].get("resource") and indices.get("buffer") is not None,
            "observed buffer bindings disagree")
    vertex_bytes = bytes.fromhex(data["vertex"]["data"])
    index_values = struct.unpack("<3H", bytes.fromhex(data["index"]["data"]))
    color = struct.unpack("<4f", struct.pack("<4I", *draw["vertex_constants"]["bits"][:4]))
    argb = sum(round(component * 255) << shift for component, shift in zip(color, (16, 8, 0, 24)))
    vertices = []
    for index in index_values:
        x, y, z, w, _ = struct.unpack_from("<ffffI", vertex_bytes, index * 20)
        require(w == 1.0, "fixture shader requires clip-space w=1")
        vertices.append((x, y, z, w, argb))
    identity = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
    trace_to_up_packet.validate_camera_triangle(vertices, [identity] * 3)
    line = " ".join(f"{value:.9g}" for value in identity)
    lines = ["T3R_XYZ_V1", "256 256", str(clear["color"]), line, line, line]
    lines.extend(f"{x:.9g} {y:.9g} {z:.9g} {color_value}" for x, y, z, _, color_value in vertices)
    return "\n".join(lines) + "\n"


def inspect_prepared_image(path):
    data = Path(path).read_bytes()
    if len(data) != PPM_SIZE or not data.startswith(HEADER):
        raise RunError("combined capture image has an invalid format")
    pixel = len(HEADER) + (64 * 256 + 64) * 3
    if (data[len(HEADER):len(HEADER) + 3] != b"\x11\x13\x18" or
        data[pixel:pixel + 3] != b"\x00\xff\x00" or
        sum(data[i:i + 3] == b"\x00\xff\x00" for i in range(len(HEADER), len(data), 3)) < 1000):
        raise RunError("combined capture image is missing the green shader pixel")


def execute(engine, bottle, fixture, output, timeout, buffer_facade=False, resource_facade=False, dx12_replay=None):
    engine, bottle, fixture, output = map(valid_path, (engine, bottle, fixture, output))
    replay = valid_path(dx12_replay) if dx12_replay else None
    if replay and not resource_facade:
        raise RunError("DX12 replay is supported only for the resource fixture")
    wine, server, frameworks = engine / "engine/bin/wine", engine / "engine/bin/wineserver", engine / "frameworks"
    if not all(path.is_file() and os.access(path, os.X_OK) for path in (wine, server)) or not frameworks.is_dir():
        raise RunError(f"invalid Highball engine: {engine}")
    z_drive = bottle / "dosdevices/z:"
    if not bottle.is_dir() or not z_drive.is_symlink() or os.readlink(z_drive) != "/":
        raise RunError("bottle must map Z: to /")
    check_pe32(fixture, False)
    if replay:
        check_pe32(replay, False)
    if not math.isfinite(timeout) or timeout <= 0:
        raise RunError("timeout must be positive and finite")
    for protected in (engine, bottle, fixture.parent, *([replay.parent] if replay else [])):
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
                                 "The default retaining facade rejects external COM values; the separate persistent eager experiment forwards them and invalidates capture evidence.",
                                 "One diagnostic intentionally stores a wrapped IUnknown on a borrowed native texture, then breaks the cycle; this is not an application path.",
                                 "One retaining-policy diagnostic deliberately stores an external COM object holding the facade device in a borrowed native texture, then breaks its cycle.",
                                 "The separate eager-retirement diagnostic attaches a reserved-GUID marker to native resources; it is not production-transparent.",
                                 "The eager-buffer diagnostic loses WRITEONLY shadow bytes and private-data provenance after wrapper retirement.",
                                 "The separate persistent-buffer diagnostic retains shadows across selected state-block reacquisition but invalidates them on reset.",
                                 "Two reset scenarios were exercised; runtime concurrency and comprehensive state-block behavior remain untested.",
                                 "Six synthetic triangles and selected fixed-function states only; no gameplay parity."]
        result["dx12_replay"] = {"status": "not_run"}
        result["limitations"].append(
            "The combined draw is single-threaded and uses one exact vs_1_1 program; other shader instructions and texture contents are unsupported.")
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
            if resource_facade and stage == "facade":
                trace = Path(str(ppm) + ".trace.jsonl")
                inspect_prepared_trace(trace)
                trace_image = Path(str(trace) + ".ppm")
                inspect_prepared_image(trace_image)
                result["runs"][stage]["prepared_trace"] = trace.name
                result["runs"][stage]["prepared_trace_sha256"] = sha256(trace)
                result["runs"][stage]["prepared_image"] = trace_image.name
                result["runs"][stage]["prepared_image_sha256"] = sha256(trace_image)
        stage = "compare"
        if observations["baseline"] != observations["facade"]:
            raise RunError("native and facade HRESULT/state observations differ")
        if images["baseline"] != images["facade"]:
            raise RunError("native and facade PPM streams differ")
        if replay:
            stage = "prepared_dx12_replay"
            records = trace_to_up_packet.validated_records(trace)
            packet = output / "prepared-replay.txt"
            with packet.open("x", encoding="ascii", newline="\n") as destination:
                destination.write(prepared_replay_packet(records, synthetic_fixture=True))
            replay_image, replay_log = output / "prepared-dx12.ppm", output / "prepared-dx12.log"
            run_stage(stage, [wine, replay, windows_path(packet), "--output", windows_path(replay_image)],
                      output, env, replay_log, timeout)
            inspect_prepared_image(replay_image)
            if replay_image.read_bytes() != trace_image.read_bytes():
                raise RunError("synchronized D3D8 and DX12 synthetic images differ")
            result["dx12_replay"] = {"status": "PASS", "packet": packet.name, "packet_sha256": sha256(packet),
                                     "replay_executable_sha256": sha256(replay), "image": replay_image.name,
                                     "image_sha256": sha256(replay_image), "log_sha256": sha256(replay_log)}
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
    parser.add_argument("--dx12-replay", help="absolute path to the synthetic DX12 replay executable")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--buffer-facade", action="store_true", help="validate the separate buffer facade fixture")
    group.add_argument("--resource-facade", action="store_true", help="validate the separate combined resource fixture")
    args = parser.parse_args(argv)
    try:
        result = execute(args.engine, args.bottle, args.fixture, args.output_dir, args.timeout,
                         args.buffer_facade, args.resource_facade, args.dx12_replay)
    except RunError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if result.get("dx12_replay", {}).get("status") == "PASS":
        print(f"PASS: synchronized D3D8/DX12 RGB SHA-256 {result['dx12_replay']['image_sha256']}; results: {args.output_dir}")
    else:
        print(f"PASS: identical PPM SHA-256 {result['runs']['baseline']['ppm_sha256']}; results: {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
