"""Validate observed FVF geometry without asserting frame replayability."""
import copy
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_trace_to_ir as ir_test
import test_trace_to_up_packet as up_test
import trace_geometry_candidates as geometry
import trace_to_ir


def vertices():
    return b"".join(struct.pack("<4fI", *v) for v in (
        (10, 20, 0.5, 1, 0xffff0000), (30, 20, 0.5, 1, 0xff00ff00),
        (20, 40, 0.5, 1, 0xff0000ff)))


def buffered_events():
    draw = ir_test.buffered_fixture()
    draw["vertex_shader"] = 0x44
    draw["streams"][0].update(stride=20, description={"size": 200})
    draw["buffer_data"]["vertex"].update(stride=20, byte_offset=100, byte_count=60, data=vertices().hex())
    draw["buffer_data"]["index"]["data"] = struct.pack("<3H", 4, 2, 3).hex()
    events = ir_test.buffered_events(draw)
    events[-2]["had_reset"] = False
    events[-2]["draws"] = 1
    return events


def immediate_events():
    events = up_fixture()
    draw = events[4]
    draw.update(call="DrawIndexedPrimitiveUP", min_vertex_index=2, num_vertices=3, index_format=101)
    draw["up_data"]["vertex"].update(first_vertex=2, byte_offset=40, data=vertices().hex())
    draw["up_data"]["index"] = {"status": "captured", "index_count": 3, "index_format": 101,
                                "byte_count": 6, "encoding": "hex", "data": struct.pack("<3H", 4, 2, 3).hex()}
    return events


def up_fixture():
    events = up_test.fixture()
    events[0]["up_capture"] = {"encoding": "hex", "component_limit_bytes": 256 * 1024,
                                "frame_limit_bytes": 8 * 1024 * 1024}
    return events


class GeometryCandidateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-geometry-test-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "trace.jsonl"

    def convert(self, events):
        self.path.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
        return geometry.enrich(trace_to_ir.convert(self.path))

    def test_indexed_buffered_and_up_windows_rebase_nonzero_minimum(self):
        for events, source in ((buffered_events(), "buffered"), (immediate_events(), "immediate")):
            with self.subTest(source=source):
                ir = self.convert(events)
                candidate = ir["meshes"][0]["geometry_candidate"]
                self.assertFalse(ir["replayable"])
                self.assertFalse(ir["meshes"][0]["geometry_available"])
                self.assertEqual(candidate["status"], "candidate")
                self.assertEqual(candidate["source"], source)
                self.assertEqual(bytes.fromhex(candidate["vertices"]["data"]), vertices())
                self.assertEqual(struct.unpack("<3H", bytes.fromhex(candidate["indices"]["data"])), (2, 0, 1))
                self.assertEqual(candidate["layout"]["position_kind"], "xyzrhw")
                self.assertEqual(candidate["layout"]["shader_interpretation"], "unverified_fvf_or_programmable_handle")

    def test_nonindexed_up_preserves_draw_order_and_no_indices(self):
        events = up_fixture()
        ir = self.convert(events)
        candidate = ir["meshes"][0]["geometry_candidate"]
        self.assertEqual(candidate["status"], "candidate")
        self.assertIsNone(candidate["indices"])
        self.assertEqual(ir["frames"][0]["commands"][0]["mesh"], ir["meshes"][0]["id"])

    def test_fvf_layout_normal_color_and_nonstandard_texture_dimensions(self):
        fvf = 0x002 | 0x010 | 0x040 | 0x200 | (3 << 16) | (1 << 18)
        layout = geometry.fvf_layout(fvf, 48)
        attrs = layout["attributes"]
        self.assertEqual([(x["semantic"], x["offset_bytes"], x["components"]) for x in attrs],
                         [("position", 0, 3), ("normal", 12, 3), ("diffuse", 24, 4),
                          ("texcoord0", 28, 1), ("texcoord1", 32, 3)])
        self.assertEqual(layout["minimum_stride_bytes"], 44)
        for fvf, stride in ((0x006, 32), (0x112, 12), (0x004 | 0x10, 40),
                            (0x002 | 0x900, 128), (0x002 | 0x1000, 16), (0x002 | (1 << 16), 32)):
            with self.subTest(fvf=fvf), self.assertRaises(geometry.GeometryError):
                geometry.fvf_layout(fvf, stride)

    def test_failed_unstable_or_ambiguous_capture_stays_unavailable(self):
        for case in ("draw_failed", "unstable", "reset", "concurrent", "no_outcome", "no_fvf",
                     "unsupported_fvf", "nonfinite", "bad_indices"):
            events = buffered_events()
            draw, result, end = events[2], events[3], events[4]
            if case == "draw_failed": result["hr"] = -1
            if case == "unstable": result["buffer_stable"] = False
            if case == "reset": end["had_reset"] = True
            if case == "concurrent": end["concurrent_operations"] = True
            if case == "no_outcome": del end["had_reset"]
            if case == "no_fvf": draw.pop("vertex_shader")
            if case == "unsupported_fvf": draw["vertex_shader"] = 0x006
            if case == "nonfinite":
                raw = bytearray(vertices()); raw[:4] = struct.pack("<f", float("nan"))
                draw["buffer_data"]["vertex"]["data"] = raw.hex()
            if case == "bad_indices": draw["buffer_data"]["index"]["data"] = struct.pack("<3H", 4, 2, 8).hex()
            with self.subTest(case=case):
                result = self.convert(events)["meshes"][0]["geometry_candidate"]
                self.assertEqual(result["status"], "unavailable")
                self.assertTrue(result["reason"])

    def test_immediate_draw_requires_original_call_metadata(self):
        for field in ("num_vertices", "primitive_count", "vertex_stride", "index_format", "min_vertex_index"):
            events = immediate_events()
            del events[4][field]
            with self.subTest(field=field):
                candidate = self.convert(events)["meshes"][0]["geometry_candidate"]
                self.assertEqual(candidate["status"], "unavailable")

    def test_nonindexed_draw_requires_original_primitive_count_and_stride(self):
        for field in ("primitive_count", "vertex_stride"):
            events = up_fixture()
            del events[4][field]
            with self.subTest(field=field):
                candidate = self.convert(events)["meshes"][0]["geometry_candidate"]
                self.assertEqual(candidate["status"], "unavailable")

    def test_reset_event_overrides_inconsistent_frame_flag(self):
        events = buffered_events()
        events.insert(4, {"event": "reset", "frame": 0, "reset_hr": 0})
        self.assertFalse(events[-2]["had_reset"])
        candidate = self.convert(events)["meshes"][0]["geometry_candidate"]
        self.assertEqual(candidate["status"], "unavailable")
        self.assertIn("reset", candidate["reason"])

    def test_nonindexed_up_requires_explicit_null_index(self):
        for missing in (True, False):
            events = up_fixture()
            if missing:
                del events[4]["up_data"]["index"]
            else:
                events[4]["up_data"]["index"] = {"status": "captured", "data": "0000"}
            with self.subTest(missing=missing):
                self.assertEqual(self.convert(events)["meshes"][0]["geometry_candidate"]["status"], "unavailable")

    def test_up_capability_component_and_frame_budgets_are_required(self):
        for case in ("missing", "encoding", "component", "frame", "unknown_size", "missing_data", "contradictory", "other_component"):
            events = up_fixture()
            if case == "missing":
                del events[0]["up_capture"]
            if case == "encoding":
                events[0]["up_capture"]["encoding"] = "base64"
            if case == "component":
                events[0]["up_capture"].update(component_limit_bytes=59, frame_limit_bytes=100)
            if case == "frame":
                events[0]["up_capture"].update(component_limit_bytes=60, frame_limit_bytes=60)
                draw = copy.deepcopy(events[4])
                draw["op_id"] = 3
                events[6:6] = [draw, {"event": "draw_result", "frame": 0, "op_id": 3, "hr": -1}]
                events[-2]["draws"] = 2
            if case in ("unknown_size", "missing_data", "contradictory", "other_component"):
                draw = copy.deepcopy(events[4])
                draw["op_id"] = 3
                if case == "unknown_size": del draw["up_data"]["vertex"]["byte_count"]
                if case == "missing_data": del draw["up_data"]
                if case == "contradictory": draw["up_data"]["vertex"]["status"] = "unreadable"
                if case == "other_component":
                    events[0]["up_capture"].update(component_limit_bytes=60, frame_limit_bytes=1024)
                    draw["up_data"]["vertex"].update(byte_count=80, data="00" * 80)
                events[6:6] = [draw, {"event": "draw_result", "frame": 0, "op_id": 3, "hr": -1}]
                events[-2]["draws"] = 2
            with self.subTest(case=case):
                self.assertEqual(self.convert(events)["meshes"][0]["geometry_candidate"]["status"], "unavailable")

    def test_cli_produces_candidate_without_overwriting_existing_output(self):
        self.convert(buffered_events())
        output = Path(self.temp.name) / "result.json"
        command = [sys.executable, "-B", str(HERE.parent / "tools" / "trace_geometry_candidates.py"),
                   str(self.path), "-o", str(output)]
        result = subprocess.run(command, text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        serialized = json.loads(output.read_text())
        self.assertEqual(serialized["meshes"][0]["geometry_candidate"]["status"], "candidate")
        self.assertFalse(serialized["replayable"])
        before = output.read_bytes()
        result = subprocess.run(command, text=True, capture_output=True, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_bytes(), before)

    def test_frame_draw_count_is_required_and_must_match(self):
        for value in (None, 0, 2, True):
            events = buffered_events()
            if value is None:
                del events[-2]["draws"]
            else:
                events[-2]["draws"] = value
            with self.subTest(value=value):
                self.assertEqual(self.convert(events)["meshes"][0]["geometry_candidate"]["status"], "unavailable")


if __name__ == "__main__":
    unittest.main()
