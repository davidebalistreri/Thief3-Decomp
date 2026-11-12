import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import trace_summary
import trace_to_ir


HEADER = {"event": "header", "format": "t3render-d3d8-diagnostic", "version": 1}
END = {"event": "capture_end", "frames": 1}
HEADER_BUFFER = dict(HEADER, operation_results=1, clear_rect_limit=256,
                     buffer_capture={"requested": True, "encoding": "hex",
                                     "component_limit_bytes": 256 * 1024, "frame_limit_bytes": 8 * 1024 * 1024})


def buffered_fixture(frame=0, op_id=1):
    return {"event": "draw", "frame": frame, "op_id": op_id, "call": "DrawIndexedPrimitive",
            "primitive_type": 4, "primitive_count": 1, "min_vertex_index": 2, "num_vertices": 3, "start_index": 1,
            "streams": [{"stream": 0, "hr": 0, "buffer": "0xaaa", "stride": 4, "description": {"size": 64}}],
            "indices": {"hr": 0, "buffer": "0xbbb", "base_vertex_index": 3,
                        "description": {"format": 101, "size": 32}},
            "buffer_data": {"source": "application_lock_shadow",
                            "vertex": {"status": "captured", "resource": "0xaaa", "stream": 0,
                                       "first_vertex": 5, "vertex_count": 3, "stride": 4,
                                       "byte_offset": 20, "byte_count": 12, "encoding": "hex",
                                       "data": "1415161718191a1b1c1d1e1f"},
                            "index": {"status": "captured", "resource": "0xbbb", "index_count": 3,
                                      "index_format": 101, "byte_offset": 2, "byte_count": 6,
                                      "encoding": "hex", "data": "020003000400"}}}


def buffered_events(draw=None, result=None, header=None):
    draw = buffered_fixture() if draw is None else draw
    result = {"event": "draw_result", "frame": draw["frame"], "op_id": draw["op_id"],
              "hr": 0, "buffer_stable": True} if result is None else result
    return [HEADER_BUFFER if header is None else header, {"event": "frame_begin", "frame": draw["frame"]},
            draw, result, {"event": "frame_end", "frame": draw["frame"], "present_hr": 0,
                           "concurrent_operations": False}, END]


class TraceToIrTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-ir-test-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "trace.jsonl"

    def write(self, events):
        self.path.write_text("\n".join(json.dumps(e) for e in events) + "\n", encoding="utf-8")

    def test_multiple_draw_snapshots_preserve_association_and_buffered_arguments(self):
        view = list(range(16))
        draw = {
            "event": "draw", "frame": 7, "call": "DrawIndexedPrimitive", "primitive_type": 4,
            "primitive_count": 2, "min_vertex_index": 3, "num_vertices": 7, "start_index": 11,
            "index_format": 0,  # Not applicable for a buffered draw.
            "viewport": {"x": 0, "y": 1, "width": 640, "height": 480, "min_z": 0, "max_z": 1},
            "transforms": {"view": view, "projection": None, "world": [view, None], "texture": []},
            "material": {"diffuse": [1, 0.5, 0.25, 1], "power": 8},
            "stages": [
                {"stage": 0, "texture": {"id": "0x11", "width": 64, "height": 32, "levels": 4}},
                {"stage": 1, "texture": None}, {"stage": 2, "texture": None, "texture_hr": -1},
            ],
            "lights": [{"index": 5, "type": 1, "enabled": True, "position": [1, 2, 3]}],
            "streams": [
                {"stream": 0, "hr": 0, "buffer": "0x22", "stride": 24, "description": {"size": 100}},
                {"stream": 1, "hr": 0, "buffer": None, "stride": 0},
                {"stream": 2, "hr": -1, "buffer": None},
            ],
            "indices": {"hr": 0, "buffer": "0x33", "base_vertex_index": 5,
                        "description": {"format": 101, "size": 12}},
            "render_states": {"7": 1}, "unknown_future_field": {"preserve": True},
        }
        second = {"event": "draw", "frame": 7, "call": "DrawPrimitive", "primitive_type": 2,
                  "start_vertex": 4, "primitive_count": 1, "lights": [], "material": None}
        self.write([HEADER, {"event": "device_note", "opaque": 9}, {"event": "frame_begin", "frame": 7},
                    draw, {"event": "reset", "frame": 7, "reset_hr": 0}, second,
                    {"event": "frame_end", "frame": 7, "present_hr": 0}, END])
        result = trace_to_ir.convert(self.path)
        self.assertEqual(result["source"]["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())
        self.assertFalse(result["runtime_ir"])
        self.assertFalse(result["replayable"])
        self.assertFalse(result["extraction_complete"])
        first, latter = result["frames"][0]["commands"]
        self.assertEqual((first["camera"], first["mesh"], first["material"]), ("camera:0", "mesh:0", "material:0"))
        self.assertEqual((latter["camera"], latter["mesh"], latter["material"]), ("camera:1", "mesh:1", "material:1"))
        self.assertEqual([first["source_line"], latter["source_line"]], [4, 6])
        self.assertEqual(first["topology"], "triangle_list")
        self.assertEqual(first["parameters"]["minimum_vertex"], 3)
        self.assertEqual(first["parameters"]["vertex_count"], 7)
        self.assertEqual(first["parameters"]["first_index"], 11)
        self.assertIsNone(first["parameters"]["immediate_index_format"])
        self.assertEqual(result["meshes"][0]["index_buffer_observed"]["base_vertex_index"], 5)
        self.assertEqual(result["meshes"][0]["index_buffer_observed"]["index_format"], "uint16")
        self.assertFalse(result["meshes"][0]["geometry_available"])
        self.assertEqual([s["status"] for s in result["meshes"][0]["vertex_streams_observed"]],
                         ["bound", "unbound", "unknown"])
        self.assertEqual(result["meshes"][0]["vertex_streams_observed"][0]["resource"]["id"],
                         {"scope": "capture", "token": "0x22"})
        self.assertEqual([s["status"] for s in result["materials"][0]["texture_slots_observed"]],
                         ["bound", "unbound", "unknown"])
        self.assertIsNone(result["materials"][1]["legacy_parameters_observed"])
        self.assertIsNone(result["materials"][0]["pbr_parameters"])
        self.assertEqual(result["cameras"][0]["view_matrix_observed"]["values"], view)
        self.assertIsNone(result["cameras"][0]["effective_for_draw"])
        self.assertIsNone(first["transforms_observed"]["world"][1])
        self.assertEqual(first["lights"], {"observed": ["light:0:0"], "complete": False})
        self.assertEqual(latter["lights"], {"observed": [], "complete": False})
        self.assertEqual(result["lights"][0]["position"], [1, 2, 3])
        self.assertEqual(first["compatibility"]["d3d8"]["draw"], draw)
        self.assertEqual(result["frames"][0]["compatibility"]["d3d8"]["events"][0]["source_line"], 5)
        self.assertEqual(result["compatibility"]["d3d8"]["unmapped_events"][0]["source_line"], 2)

    def test_up_draw_does_not_use_previously_bound_buffers(self):
        draw = {"event": "draw", "frame": 0, "call": "DrawIndexedPrimitiveUP", "primitive_type": 4,
                "min_vertex_index": 0, "num_vertices": 3, "primitive_count": 1, "vertex_stride": 20,
                "index_format": 102, "streams": [{"stream": 0, "hr": 0, "buffer": "0x123"}],
                "indices": {"hr": 0, "buffer": "0x456", "description": {"format": 101}}}
        self.write([HEADER, {"event": "frame_begin", "frame": 0}, draw,
                    {"event": "frame_end", "frame": 0, "present_hr": 0}, END])
        result = trace_to_ir.convert(self.path)
        command = result["frames"][0]["commands"][0]
        self.assertEqual(result["meshes"][0]["geometry_source"], "immediate")
        self.assertNotIn("buffered_data_observed", result["meshes"][0])
        self.assertIsNone(result["meshes"][0]["vertex_streams_observed"])
        self.assertIsNone(result["meshes"][0]["index_buffer_observed"])
        self.assertEqual(result["meshes"][0]["immediate_data_observed"]["vertex"]["status"], "unknown")
        self.assertEqual(result["meshes"][0]["immediate_data_observed"]["index"]["status"], "unknown")
        self.assertEqual(command["parameters"]["immediate_vertex_stride_bytes"], 20)
        self.assertEqual(command["parameters"]["immediate_index_format"], "uint32")
        self.assertEqual(command["compatibility"]["d3d8"]["draw"]["indices"]["buffer"], "0x456")

    def test_up_bytes_are_exposed_as_opaque_observations(self):
        draw = {"event": "draw", "frame": 0, "call": "DrawIndexedPrimitiveUP", "primitive_type": 4,
                "primitive_count": 1, "min_vertex_index": 2, "num_vertices": 3, "vertex_stride": 4,
                "index_format": 101, "up_data": {
                    "vertex": {"status": "captured", "first_vertex": 2, "vertex_count": 3, "stride": 4,
                               "byte_offset": 8, "byte_count": 12, "encoding": "hex",
                               "data": "01020304aabbccdd01234567"},
                    "index": {"status": "captured", "index_count": 3, "index_format": 101,
                              "byte_count": 6, "encoding": "hex", "data": "020003000400"}}}
        self.write([HEADER, {"event": "frame_begin", "frame": 0}, draw,
                    {"event": "frame_end", "frame": 0, "present_hr": 0}, END])
        result = trace_to_ir.convert(self.path)
        mesh = result["meshes"][0]
        data = mesh["immediate_data_observed"]
        self.assertEqual(data["vertex"], {"status": "captured", "first_vertex": 2, "vertex_count": 3,
                                          "stride_bytes": 4, "byte_offset": 8, "byte_count": 12,
                                          "encoding": "hex", "data": "01020304aabbccdd01234567"})
        self.assertEqual(data["index"], {"status": "captured", "index_count": 3, "index_format": "uint16",
                                         "byte_count": 6, "encoding": "hex", "data": "020003000400"})
        self.assertFalse(mesh["geometry_available"])
        self.assertFalse(result["replayable"])
        self.assertEqual(result["frames"][0]["commands"][0]["compatibility"]["d3d8"]["draw"]["up_data"],
                         draw["up_data"])

    def test_up_empty_and_unindexed_are_distinct_from_unknown(self):
        draw = {"event": "draw", "frame": 0, "call": "DrawPrimitiveUP", "primitive_type": 4,
                "primitive_count": 0, "vertex_stride": 20, "up_data": {
                    "vertex": {"status": "empty", "first_vertex": 0, "vertex_count": 0, "stride": 20,
                               "byte_offset": 0, "byte_count": 0, "encoding": "hex", "data": ""},
                    "index": None}}
        self.write([HEADER, {"event": "frame_begin", "frame": 0}, draw,
                    {"event": "frame_end", "frame": 0, "present_hr": 0}, END])
        data = trace_to_ir.convert(self.path)["meshes"][0]["immediate_data_observed"]
        self.assertEqual(data["vertex"]["status"], "empty")
        self.assertEqual(data["vertex"]["data"], "")
        self.assertIsNone(data["index"])

    def test_up_index32_and_unindexed_nonempty(self):
        first = {"event": "draw", "frame": 0, "call": "DrawPrimitiveUP", "primitive_type": 2,
                 "primitive_count": 1, "vertex_stride": 1, "up_data": {
                     "vertex": {"status": "captured", "first_vertex": 0, "vertex_count": 2, "stride": 1,
                                "byte_offset": 0, "byte_count": 2, "encoding": "hex", "data": "aBcd"},
                     "index": None}}
        second = {"event": "draw", "frame": 0, "call": "DrawIndexedPrimitiveUP", "primitive_type": 1,
                  "primitive_count": 1, "min_vertex_index": 3, "num_vertices": 1, "vertex_stride": 1,
                  "index_format": 102, "up_data": {
                      "vertex": {"status": "captured", "first_vertex": 3, "vertex_count": 1, "stride": 1,
                                 "byte_offset": 3, "byte_count": 1, "encoding": "hex", "data": "ff"},
                      "index": {"status": "captured", "index_count": 1, "index_format": 102,
                                "byte_count": 4, "encoding": "hex", "data": "03000000"}}}
        self.write([HEADER, {"event": "frame_begin", "frame": 0}, first, second,
                    {"event": "frame_end", "frame": 0, "present_hr": 0}, END])
        meshes = trace_to_ir.convert(self.path)["meshes"]
        self.assertEqual(meshes[0]["immediate_data_observed"]["vertex"]["data"], "aBcd")
        self.assertIsNone(meshes[0]["immediate_data_observed"]["index"])
        self.assertEqual(meshes[1]["immediate_data_observed"]["index"]["index_format"], "uint32")
        self.assertEqual(meshes[1]["immediate_data_observed"]["index"]["data"], "03000000")

    def test_up_invalid_bytes_or_metadata_remain_unknown(self):
        valid = {"event": "draw", "frame": 0, "call": "DrawIndexedPrimitiveUP", "primitive_type": 4,
                 "primitive_count": 1, "min_vertex_index": 0, "num_vertices": 3, "vertex_stride": 1,
                 "index_format": 101, "up_data": {
                     "vertex": {"status": "captured", "first_vertex": 0, "vertex_count": 3, "stride": 1,
                                "byte_offset": 0, "byte_count": 3, "encoding": "hex", "data": "aabbcc"},
                     "index": {"status": "captured", "index_count": 3, "index_format": 101,
                               "byte_count": 6, "encoding": "hex", "data": "000001000200"}}}
        variants = [
            ("vertex", "data", "aabbc"), ("vertex", "data", "aabbgg"),
            ("vertex", "byte_count", 2), ("vertex", "byte_count", True),
            ("vertex", "byte_offset", 1), ("vertex", "vertex_count", 2),
            ("vertex", "stride", 0), ("vertex", "first_vertex", -1),
            ("vertex", "encoding", "base64"), ("vertex", "status", "empty"),
            ("vertex", "status", "unreadable"),
            ("index", "data", "00000100020g"), ("index", "byte_count", 5),
            ("index", "index_count", 2), ("index", "index_format", 999),
            ("index", "status", "overflow"), ("index", "encoding", "base64"),
        ]
        for component, key, value in variants:
            with self.subTest(component=component, key=key, value=value):
                draw = copy.deepcopy(valid)
                draw["up_data"][component][key] = value
                self.write([HEADER, {"event": "frame_begin", "frame": 0}, draw,
                            {"event": "frame_end", "frame": 0, "present_hr": 0}, END])
                result = trace_to_ir.convert(self.path)
                data = result["meshes"][0]["immediate_data_observed"]
                self.assertEqual(data[component]["status"], "unknown")
                self.assertIsNone(data[component]["data"])
                other = "index" if component == "vertex" else "vertex"
                self.assertEqual(data[other]["status"], "captured")
                self.assertEqual(result["frames"][0]["commands"][0]["compatibility"]["d3d8"]
                                 ["draw"]["up_data"][component][key], value)

    def test_buffered_bytes_are_exposed_only_as_verified_observations(self):
        draw = buffered_fixture()
        self.write(buffered_events(draw))
        result = trace_to_ir.convert(self.path)
        mesh = result["meshes"][0]
        observed = mesh["buffered_data_observed"]
        self.assertEqual((observed["source"], observed["draw_hresult"], observed["buffer_stable"]),
                         ("application_lock_shadow", 0, True))
        self.assertEqual(observed["vertex"],
                         {"status": "captured", "resource": {"scope": "capture", "token": "0xaaa"}, "stream": 0,
                          "first_vertex": 5, "vertex_count": 3, "stride_bytes": 4, "byte_offset": 20,
                          "byte_count": 12, "encoding": "hex", "data": "1415161718191a1b1c1d1e1f"})
        self.assertEqual(observed["index"],
                         {"status": "captured", "resource": {"scope": "capture", "token": "0xbbb"},
                          "index_count": 3, "index_format": "uint16", "byte_offset": 2,
                          "byte_count": 6, "encoding": "hex", "data": "020003000400"})
        self.assertFalse(mesh["geometry_available"])
        self.assertFalse(result["replayable"])
        self.assertEqual(result["frames"][0]["commands"][0]["compatibility"]["d3d8"]["draw"]["buffer_data"],
                         draw["buffer_data"])

    def test_unstable_or_failed_buffered_draw_never_exposes_bytes(self):
        draw = buffered_fixture()
        for hr, stable in ((0, False), (-1, True), (0x80004005, True), (-1, False)):
            with self.subTest(hr=hr, stable=stable):
                outcome = {"event": "draw_result", "frame": 0, "op_id": 1, "hr": hr, "buffer_stable": stable}
                self.write(buffered_events(draw, outcome))
                result = trace_to_ir.convert(self.path)
                data = result["meshes"][0]["buffered_data_observed"]
                self.assertEqual((data["draw_hresult"], data["buffer_stable"]), (hr, stable))
                for component in ("vertex", "index"):
                    self.assertEqual(data[component]["status"], "unknown")
                    self.assertIsNone(data[component]["data"])
                self.assertEqual(result["frames"][0]["compatibility"]["d3d8"]["events"][0]["event"], outcome)
                self.assertEqual(result["frames"][0]["commands"][0]["compatibility"]["d3d8"]["draw"], draw)

    def test_buffered_component_metadata_and_binding_are_fail_closed(self):
        mutations = [
            ("vertex", lambda d: d["buffer_data"]["vertex"].update(resource="0xccc")),
            ("vertex", lambda d: d["streams"][0].update(buffer="0x0")),
            ("vertex", lambda d: d["streams"][0].update(buffer="not-a-pointer")),
            ("vertex", lambda d: d["buffer_data"]["vertex"].update(stream=1)),
            ("vertex", lambda d: d["buffer_data"]["vertex"].update(stream=False)),
            ("vertex", lambda d: d["buffer_data"]["vertex"].update(byte_offset=19)),
            ("vertex", lambda d: d["buffer_data"]["vertex"].update(byte_count=11)),
            ("vertex", lambda d: d["buffer_data"]["vertex"].update(vertex_count=2)),
            ("vertex", lambda d: d["buffer_data"]["vertex"].update(stride=0)),
            ("vertex", lambda d: d["buffer_data"]["vertex"].update(data="14" * 11 + "gg")),
            ("vertex", lambda d: d["streams"][0].update(hr=0x80004005)),
            ("vertex", lambda d: d["streams"][0]["description"].update(size=31)),
            ("vertex", lambda d: d["streams"].append(copy.deepcopy(d["streams"][0]))),
            ("vertex", lambda d: d["indices"].update(base_vertex_index=4)),
            ("vertex", lambda d: d["indices"].update(base_vertex_index=0xffffffff)),
            ("index", lambda d: d["buffer_data"]["index"].update(resource="0xccc")),
            ("index", lambda d: d["indices"].update(buffer="0x0")),
            ("index", lambda d: d["buffer_data"]["index"].update(byte_offset=0)),
            ("index", lambda d: d["buffer_data"]["index"].update(byte_count=5)),
            ("index", lambda d: d["buffer_data"]["index"].update(index_count=2)),
            ("index", lambda d: d["buffer_data"]["index"].update(index_format=102)),
            ("index", lambda d: d["buffer_data"]["index"].update(data="020003000500")),
            ("index", lambda d: d["buffer_data"]["index"].update(data="0200030004gg")),
            ("index", lambda d: d["indices"]["description"].update(size=7)),
            ("index", lambda d: d["indices"]["description"].update(format=102)),
            ("index", lambda d: d.update(min_vertex_index=0xffffffff, num_vertices=2)),
        ]
        for component, mutate in mutations:
            with self.subTest(component=component, mutation=mutate):
                draw = buffered_fixture(); mutate(draw)
                self.write(buffered_events(draw))
                result = trace_to_ir.convert(self.path)
                value = result["meshes"][0]["buffered_data_observed"][component]
                self.assertEqual(value["status"], "unknown")
                self.assertIsNone(value["data"])
                self.assertEqual(result["frames"][0]["commands"][0]["compatibility"]["d3d8"]["draw"], draw)

    def test_buffered_capability_source_and_empty_draw_are_conservative(self):
        for mutation in (lambda h: h.pop("buffer_capture"),
                         lambda h: h["buffer_capture"].update(requested=False),
                         lambda h: h["buffer_capture"].update(encoding="base64"),
                         lambda h: h["buffer_capture"].update(requested=1),
                         lambda h: h["buffer_capture"].update(component_limit_bytes=0),
                         lambda h: h["buffer_capture"].update(component_limit_bytes=256 * 1024 + 1),
                         lambda h: h["buffer_capture"].update(frame_limit_bytes=1),
                         lambda h: h["buffer_capture"].update(frame_limit_bytes=8 * 1024 * 1024 + 1)):
            with self.subTest(mutation=mutation):
                header = copy.deepcopy(HEADER_BUFFER); mutation(header)
                self.write(buffered_events(header=header))
                observed = trace_to_ir.convert(self.path)["meshes"][0]["buffered_data_observed"]
                self.assertEqual((observed["vertex"]["status"], observed["index"]["status"]), ("unknown", "unknown"))
        draw = buffered_fixture(); draw["buffer_data"]["source"] = "other"
        self.write(buffered_events(draw))
        observed = trace_to_ir.convert(self.path)["meshes"][0]["buffered_data_observed"]
        self.assertIsNone(observed["source"])
        self.assertEqual(observed["vertex"]["status"], "unknown")

        draw = buffered_fixture(); draw.update(call="DrawPrimitive", primitive_count=0, start_vertex=1)
        draw["buffer_data"]["vertex"].update(status="empty", first_vertex=1, vertex_count=0,
                                             byte_offset=4, byte_count=0, data="")
        draw["buffer_data"]["index"] = None
        self.write(buffered_events(draw))
        observed = trace_to_ir.convert(self.path)["meshes"][0]["buffered_data_observed"]
        self.assertEqual(observed["vertex"]["status"], "empty")
        self.assertEqual(observed["vertex"]["data"], "")
        self.assertIsNone(observed["index"])

    def test_buffered_uint32_indices_and_nonindexed_vertices(self):
        draw = buffered_fixture()
        draw["indices"]["description"]["format"] = 102
        draw["buffer_data"]["index"].update(index_format=102, byte_offset=4, byte_count=12,
                                           data="020000000300000004000000")
        self.write(buffered_events(draw))
        observed = trace_to_ir.convert(self.path)["meshes"][0]["buffered_data_observed"]
        self.assertEqual((observed["index"]["status"], observed["index"]["index_format"],
                          observed["index"]["byte_offset"]), ("captured", "uint32", 4))
        self.assertEqual(observed["index"]["data"], "020000000300000004000000")

        draw = buffered_fixture()
        draw.update(call="DrawPrimitive", primitive_type=2, primitive_count=1, start_vertex=1)
        draw["buffer_data"]["vertex"].update(first_vertex=1, vertex_count=2, byte_offset=4,
                                             byte_count=8, data="0405060708090a0b")
        draw["buffer_data"]["index"] = None
        self.write(buffered_events(draw))
        observed = trace_to_ir.convert(self.path)["meshes"][0]["buffered_data_observed"]
        self.assertEqual((observed["vertex"]["status"], observed["vertex"]["byte_offset"],
                          observed["vertex"]["data"]), ("captured", 4, "0405060708090a0b"))
        self.assertIsNone(observed["index"])

    def test_buffer_results_pair_by_operation_not_event_order_or_reused_frame_id(self):
        a = buffered_fixture(frame=7, op_id=1)
        b = buffered_fixture(frame=7, op_id=2)
        b["buffer_data"]["vertex"]["data"] = "ff" * 12
        c = buffered_fixture(frame=7, op_id=1)
        c["buffer_data"]["vertex"]["data"] = "ee" * 12
        self.write([HEADER_BUFFER, {"event": "frame_begin", "frame": 7}, a, b,
                    {"event": "draw_result", "frame": 7, "op_id": 2, "hr": 0, "buffer_stable": True},
                    {"event": "draw_result", "frame": 7, "op_id": 1, "hr": 0, "buffer_stable": False},
                    {"event": "frame_end", "frame": 7, "present_hr": 0, "concurrent_operations": True},
                    {"event": "frame_begin", "frame": 7}, c,
                    {"event": "draw_result", "frame": 7, "op_id": 1, "hr": 0, "buffer_stable": True},
                    {"event": "frame_end", "frame": 7, "present_hr": 0, "concurrent_operations": False},
                    dict(END, frames=2)])
        result = trace_to_ir.convert(self.path)
        first, second, third = (mesh["buffered_data_observed"]["vertex"] for mesh in result["meshes"])
        self.assertEqual(first["status"], "unknown")
        self.assertEqual(second["data"], "ff" * 12)
        self.assertEqual(third["data"], "ee" * 12)

    def test_buffer_frame_budget_sums_components_draws_and_failed_attempts(self):
        header = copy.deepcopy(HEADER_BUFFER)
        header["buffer_capture"].update(component_limit_bytes=12, frame_limit_bytes=24)
        for first_hr, first_stable in ((0, True), (-1, True), (0, False)):
            with self.subTest(first_hr=first_hr, first_stable=first_stable):
                first = buffered_fixture(op_id=1)
                second = buffered_fixture(op_id=2)
                next_frame = buffered_fixture(frame=1, op_id=1)
                self.write([header, {"event": "frame_begin", "frame": 0},
                            first, {"event": "draw_result", "frame": 0, "op_id": 1,
                                    "hr": first_hr, "buffer_stable": first_stable},
                            second, {"event": "draw_result", "frame": 0, "op_id": 2,
                                     "hr": 0, "buffer_stable": True},
                            {"event": "frame_end", "frame": 0, "present_hr": 0,
                             "concurrent_operations": False},
                            {"event": "frame_begin", "frame": 1},
                            next_frame, {"event": "draw_result", "frame": 1, "op_id": 1,
                                         "hr": 0, "buffer_stable": True},
                            {"event": "frame_end", "frame": 1, "present_hr": 0,
                             "concurrent_operations": False}, dict(END, frames=2)])
                result = trace_to_ir.convert(self.path)
                # Each draw emits 12 + 6 bytes; 36 exceeds this frame's cap.
                for mesh in result["meshes"][:2]:
                    for component in ("vertex", "index"):
                        self.assertEqual(mesh["buffered_data_observed"][component]["status"], "unknown")
                        self.assertIsNone(mesh["buffered_data_observed"][component]["data"])
                self.assertEqual(result["frames"][0]["commands"][0]["compatibility"]["d3d8"]["draw"], first)
                self.assertEqual(result["meshes"][2]["buffered_data_observed"]["vertex"]["status"], "captured")
                self.assertEqual(result["meshes"][2]["buffered_data_observed"]["index"]["status"], "captured")

    def test_buffer_frame_budget_rejects_inconsistent_claims_and_counts_only_snapshots(self):
        header = copy.deepcopy(HEADER_BUFFER)
        header["buffer_capture"].update(component_limit_bytes=12, frame_limit_bytes=12)
        # Even one indexed draw exceeds 12 bytes when both captured components are summed.
        draw = buffered_fixture()
        self.write(buffered_events(draw, header=header))
        observed = trace_to_ir.convert(self.path)["meshes"][0]["buffered_data_observed"]
        self.assertEqual((observed["vertex"]["status"], observed["index"]["status"]), ("unknown", "unknown"))
        header["buffer_capture"]["frame_limit_bytes"] = 18
        self.write(buffered_events(draw, header=header))
        observed = trace_to_ir.convert(self.path)["meshes"][0]["buffered_data_observed"]
        self.assertEqual((observed["vertex"]["status"], observed["index"]["status"]), ("captured", "captured"))
        header["buffer_capture"]["frame_limit_bytes"] = 12
        # A shadow the producer could not read consumes no encoded-byte budget.
        draw["buffer_data"]["vertex"].update(status="unknown", data=None)
        self.write(buffered_events(draw, header=header))
        observed = trace_to_ir.convert(self.path)["meshes"][0]["buffered_data_observed"]
        self.assertEqual((observed["vertex"]["status"], observed["index"]["status"]), ("unknown", "captured"))
        # Captured status with unmeasurable data makes the entire frame budget unknown.
        draw["buffer_data"]["vertex"].update(status="captured")
        self.write(buffered_events(draw, header=header))
        observed = trace_to_ir.convert(self.path)["meshes"][0]["buffered_data_observed"]
        self.assertEqual((observed["vertex"]["status"], observed["index"]["status"]), ("unknown", "unknown"))

    def test_buffered_draw_without_matching_result_is_rejected(self):
        events = buffered_events()
        for result in (None,
                       {"event": "draw_result", "frame": 0, "op_id": 2, "hr": 0, "buffer_stable": True},
                       {"event": "draw_result", "frame": 1, "op_id": 1, "hr": 0, "buffer_stable": True},
                       {"event": "draw_result", "frame": 0, "op_id": 1, "hr": 0}):
            with self.subTest(result=result):
                invalid = copy.deepcopy(events)
                if result is None:
                    del invalid[3]
                else:
                    invalid[3] = result
                self.write(invalid)
                with self.assertRaises(trace_summary.TraceError):
                    trace_to_ir.convert(self.path)

    def test_multiple_frames_keep_snapshot_ids_distinct(self):
        draw = {"event": "draw", "frame": 3, "call": "DrawPrimitive", "primitive_count": 1}
        self.write([HEADER, {"event": "frame_begin", "frame": 3}, draw,
                    {"event": "frame_end", "frame": 3, "present_hr": 0},
                    {"event": "frame_begin", "frame": 3}, dict(draw, primitive_count=2),
                    {"event": "frame_end", "frame": 3, "present_hr": 0}, dict(END, frames=2)])
        result = trace_to_ir.convert(self.path)
        first, second = (frame["commands"][0] for frame in result["frames"])
        self.assertEqual([first["mesh"], second["mesh"]], ["mesh:0", "mesh:1"])
        self.assertEqual([first["parameters"]["primitive_count"], second["parameters"]["primitive_count"]], [1, 2])
        self.assertEqual([frame["frame"] for frame in result["frames"]], [3, 3])

    def test_missing_and_unknown_fields_are_not_filled_in(self):
        draw = {"event": "draw", "frame": 1, "call": "FutureDraw", "primitive_type": 999,
                "lights": [{"type": 999}], "streams": [{"stream": 0, "buffer": "0x1"}],
                "unrecognized": "stays in raw extension"}
        self.write([HEADER, {"event": "frame_begin", "frame": 1}, draw,
                    {"event": "frame_end", "frame": 1, "present_hr": 0}, END])
        result = trace_to_ir.convert(self.path)
        command = result["frames"][0]["commands"][0]
        self.assertIsNone(command["topology"])
        self.assertIsNone(command["indexed"])
        self.assertIsNone(result["meshes"][0]["geometry_source"])
        self.assertNotIn("buffered_data_observed", result["meshes"][0])
        self.assertIsNone(result["meshes"][0]["vertex_streams_observed"])
        self.assertIsNone(result["cameras"][0]["view_matrix_observed"])
        self.assertIsNone(result["materials"][0]["texture_slots_observed"])
        self.assertIsNone(result["lights"][0]["kind"])
        self.assertEqual(command["compatibility"]["d3d8"]["draw"]["unrecognized"], "stays in raw extension")

    def test_getter_hresult_signed_unsigned_and_out_of_range(self):
        hrs = [-2147467259, 0x80004005, -2147483649, 0x100000000, True, 0, 1]
        draw = {"event": "draw", "frame": 0, "call": "DrawIndexedPrimitive",
                "streams": [{"stream": i, "hr": hr, "buffer": "0x123"} for i, hr in enumerate(hrs)],
                "indices": {"hr": 0x80004005, "buffer": "0x456"},
                "stages": [{"stage": i, "texture_hr": hr, "texture": {"id": "0x789"}}
                           for i, hr in enumerate(hrs)] + [
                    {"stage": 8, "texture": None}, {"stage": 9, "texture_hr": 0, "texture": None},
                    {"stage": 10, "texture_hr": 0x80004005, "texture": None},
                ]}
        self.write([HEADER, {"event": "frame_begin", "frame": 0}, draw,
                    {"event": "frame_end", "frame": 0, "present_hr": 0}, END])
        result = trace_to_ir.convert(self.path)
        streams = result["meshes"][0]["vertex_streams_observed"]
        self.assertEqual([s["status"] for s in streams], ["unknown"] * 5 + ["bound", "bound"])
        self.assertTrue(all(s["resource"] is None for s in streams[:5]))
        self.assertEqual(result["meshes"][0]["index_buffer_observed"]["status"], "unknown")
        slots = result["materials"][0]["texture_slots_observed"]
        self.assertEqual([s["status"] for s in slots],
                         ["unknown"] * 5 + ["bound", "bound", "unbound", "unbound", "unknown"])
        self.assertTrue(all(s["resource"] is None for s in slots[:5]))

    def test_input_must_pass_existing_trace_validator(self):
        events = [HEADER, {"event": "frame_begin", "frame": 1},
                  {"event": "draw", "frame": 1, "call": "DrawPrimitive"},
                  {"event": "frame_end", "frame": 1, "present_hr": 0}]
        self.write(events)  # no capture_end
        with self.assertRaisesRegex(trace_summary.TraceError, "missing capture_end"):
            trace_to_ir.convert(self.path)
        self.write(events + [dict(END, frames=2)])
        with self.assertRaisesRegex(trace_summary.TraceError, "does not match"):
            trace_to_ir.convert(self.path)
        self.write([HEADER, {"event": "frame_begin", "frame": 1},
                    {"event": "draw", "frame": 2, "call": "DrawPrimitive"}])
        with self.assertRaisesRegex(trace_summary.TraceError, "does not match active frame"):
            trace_to_ir.convert(self.path)

    def test_cli_does_not_overwrite_existing_file_or_write_invalid_output(self):
        self.write([HEADER, {"event": "frame_begin", "frame": 0},
                    {"event": "draw", "frame": 0, "call": "DrawPrimitive"},
                    {"event": "frame_end", "frame": 0, "present_hr": 0}, END])
        out = Path(self.temp.name) / "out.json"
        cmd = [sys.executable, "-B", str(ROOT / "tools" / "trace_to_ir.py"), str(self.path), "-o", str(out)]
        self.assertEqual(subprocess.run(cmd, capture_output=True).returncode, 0)
        self.assertEqual(json.loads(out.read_text())["schema"], "t3render.render-ir.offline")
        out.write_text("existing data")
        self.assertEqual(subprocess.run(cmd, capture_output=True).returncode, 1)
        self.assertEqual(out.read_text(), "existing data")
        out.unlink()
        self.path.write_text("broken input")
        self.assertEqual(subprocess.run(cmd, capture_output=True).returncode, 1)
        self.assertFalse(out.exists())

    def test_nonfinite_overflow_rejected_and_unicode_line_separator_preserved(self):
        self.write([HEADER, {"event": "frame_begin", "frame": 0},
                    {"event": "draw", "frame": 0, "call": "DrawPrimitive", "note": "x\u2028y"},
                    {"event": "frame_end", "frame": 0, "present_hr": 0}, END])
        data = self.path.read_text().replace("x\\u2028y", "x\u2028y")
        self.path.write_text(data)
        self.assertEqual(trace_to_ir.convert(self.path)["frames"][0]["commands"][0]
                         ["compatibility"]["d3d8"]["draw"]["note"], "x\u2028y")
        self.path.write_text(data.replace('"note": "x\u2028y"', '"value": 1e400'))
        with self.assertRaisesRegex(ValueError, "finite range"):
            trace_to_ir.convert(self.path)


if __name__ == "__main__":
    unittest.main()
