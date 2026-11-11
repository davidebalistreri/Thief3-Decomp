"""Fail-closed tests for the explicitly attested synthetic UP fixture."""

import copy
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("trace_to_up_packet", TOOLS / "trace_to_up_packet.py")
packet = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packet)


VERTICES = [
    (25.6, 217.6, 0.0, 1.0, 0xffff0000),
    (128.0, 25.6, 0.0, 1.0, 0xffff0000),
    (230.4, 217.6, 0.0, 1.0, 0xffff0000),
]


def encoded_vertices(vertices=VERTICES):
    return b"".join(struct.pack("<4fI", *vertex) for vertex in vertices).hex()


def fixture():
    states = dict(packet.REQUIRED_RENDER_STATES)
    stage0 = {"stage": 0, "texture": None, "states": dict(packet.STAGE0_STATES)}
    stage1 = {"stage": 1, "texture": None, "states": {"1": 1}}
    vertex = {"status": "captured", "first_vertex": 0, "vertex_count": 3, "stride": 20,
              "byte_offset": 0, "byte_count": 60, "encoding": "hex", "data": encoded_vertices()}
    return [
        {"event": "header", "format": "t3render-d3d8-diagnostic", "version": 1,
         "operation_results": 1, "clear_rect_limit": 256},
        {"event": "frame_begin", "frame": 0, "presentation": {"width": 256, "height": 256,
         "format": 22, "count": 1, "multisample": 0, "windowed": True}},
        {"event": "clear", "frame": 0, "op_id": 1, "rect_count": 0, "rects_status": "none",
         "rects": [], "flags": 1, "color": 0xff000000, "depth": 1.0, "depth_bits": 0x3f800000, "stencil": 0},
        {"event": "clear_result", "frame": 0, "op_id": 1, "hr": 0},
        {"event": "draw", "frame": 0, "op_id": 2, "call": "DrawPrimitiveUP", "primitive_type": 4,
         "primitive_count": 1, "vertex_stride": 20, "vertex_shader": 0x44, "pixel_shader": 0,
         "viewport": {"x": 0, "y": 0, "width": 256, "height": 256, "min_z": 0.0, "max_z": 1.0},
         "render_states": states, "stages": [stage0, stage1], "up_data": {"vertex": vertex, "index": None}},
        {"event": "draw_result", "frame": 0, "op_id": 2, "hr": 0},
        {"event": "frame_end", "frame": 0, "draws": 1, "present_hr": 0, "had_reset": False,
         "concurrent_operations": False},
        {"event": "capture_end", "frames": 1},
    ]


class TraceToUpPacketTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-up-packet-test-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "input.jsonl"

    def write(self, events):
        self.path.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")

    def convert(self, events, attested=True):
        self.write(events)
        return packet.convert(self.path, synthetic_fixture=attested)

    def invalid(self, events, message):
        with self.assertRaisesRegex((packet.PacketError, ValueError), message):
            self.convert(events)

    def test_complete_fixture_preserves_recorded_float32_and_argb(self):
        result = self.convert(fixture())
        lines = result.splitlines()
        self.assertEqual(lines[:3], ["T3R_UP_V1", "256 256", str(0xff000000)])
        self.assertEqual(len(lines), 6)
        for line, original in zip(lines[3:], VERTICES):
            values = line.split()
            self.assertEqual(len(values), 5)
            recorded = struct.unpack("<4fI", struct.pack("<4fI", *original))
            self.assertEqual(struct.pack("<4f", *map(float, values[:4])), struct.pack("<4f", *recorded[:4]))
            self.assertEqual(int(values[4]), recorded[4])

    def test_attestation_is_mandatory_and_output_is_exclusive(self):
        events = fixture()
        with self.assertRaisesRegex(packet.PacketError, "--synthetic-fixture"):
            self.convert(events, attested=False)
        self.write(events)
        output = Path(self.temp.name) / "packet.txt"
        output.write_text("keep", encoding="ascii")
        result = subprocess.run([sys.executable, "-B", str(TOOLS / "trace_to_up_packet.py"),
                                 str(self.path), "--synthetic-fixture", "-o", str(output)],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(output.read_text(), "keep")
        self.assertNotIn("Traceback", result.stderr)

    def test_rejects_old_or_incomplete_operation_capture(self):
        cases = []
        old = fixture(); del old[0]["operation_results"]; cases.append((old, "capability"))
        for remove in (2, 3, 4, 5, 6, 7):
            events = fixture(); events.pop(remove); cases.append((events, ".+"))
        extra = fixture(); extra.insert(4, {"event": "copy_rects", "frame": 0}); cases.append((extra, "ordered"))
        reset = fixture(); reset.insert(4, {"event": "reset", "frame": 0, "reset_hr": 0}); cases.append((reset, "ordered"))
        for events, error in cases:
            with self.subTest(events=events): self.invalid(events, error)

    def test_rejects_mismatched_ids_results_and_concurrency(self):
        for index, key, value, error in [
            (2, "op_id", 0, "operation|identifiers"), (3, "op_id", 99, "operation|identifiers"),
            (4, "op_id", 1, "operation|identifiers"), (5, "op_id", 3, "operation|identifiers"),
            (2, "frame", 4, "frame"), (3, "hr", -2147467259, "did not succeed"),
            (5, "hr", 0x80004005, "did not succeed"), (6, "present_hr", -1, "did not succeed"),
            (6, "concurrent_operations", True, "concurrent"), (6, "had_reset", True, "reset"),
            (6, "draws", 2, "draw count"),
        ]:
            events = fixture(); events[index][key] = value
            with self.subTest(index=index, key=key): self.invalid(events, error)
        for key in ("concurrent_operations", "had_reset"):
            events = fixture(); del events[6][key]
            with self.subTest(missing=key): self.invalid(events, "concurrent")

    def test_rejects_unsupported_clear_viewport_and_presentation(self):
        for index, key, value, error in [
            (2, "rect_count", 1, "clear"), (2, "rects_status", "captured", "clear"),
            (2, "rects", None, "clear"), (2, "flags", 3, "clear"), (2, "color", 0, "opaque"),
        ]:
            events = fixture(); events[index][key] = value
            with self.subTest(key=key): self.invalid(events, error)
        for key, value in [("width", 640), ("height", 480), ("format", 0), ("count", 2),
                           ("windowed", False), ("multisample", 1)]:
            events = fixture(); events[1]["presentation"][key] = value
            with self.subTest(presentation=key): self.invalid(events, "fixture|backbuffer")
        for key, value in [("x", 1), ("y", 1), ("width", 257), ("height", 257),
                           ("min_z", 0.1), ("max_z", 0.9)]:
            events = fixture(); events[4]["viewport"][key] = value
            with self.subTest(viewport=key): self.invalid(events, "viewport")

    def test_rejects_unsupported_draw_state_shader_and_texture(self):
        for key, value in [("call", "DrawPrimitive"), ("primitive_type", 5), ("primitive_count", 2),
                           ("vertex_stride", 24), ("vertex_shader", 0), ("pixel_shader", 123)]:
            events = fixture(); events[4][key] = value
            with self.subTest(draw=key): self.invalid(events, "fixture|shader")
        for key, expected in packet.REQUIRED_RENDER_STATES.items():
            events = fixture(); events[4]["render_states"][key] = expected + 1
            with self.subTest(state=key): self.invalid(events, "render state")
            events = fixture(); del events[4]["render_states"][key]
            with self.subTest(missing_state=key): self.invalid(events, "render state")
        for edit in ("bound", "unknown", "failed", "enabled", "duplicate"):
            events = fixture(); stage = events[4]["stages"][1]
            if edit == "bound": stage["texture"] = {"id": "0x123"}
            if edit == "unknown": del stage["texture"]
            if edit == "failed": stage["texture_hr"] = -1
            if edit == "enabled": stage["states"]["1"] = 2
            if edit == "duplicate": stage["stage"] = 0
            with self.subTest(stage=edit): self.invalid(events, "texture")

    def test_up_bytes_are_complete_finite_and_supported(self):
        for key, value in [("status", "unreadable"), ("byte_count", 59), ("encoding", "base64"),
                           ("vertex_count", 2), ("first_vertex", 1), ("byte_offset", 20),
                           ("stride", 24), ("data", "0" * 118), ("data", "zz" * 60)]:
            events = fixture(); events[4]["up_data"]["vertex"][key] = value
            with self.subTest(key=key): self.invalid(events, "UP vertex")
        for vertices, error in [
            ([(float("nan"), 1, 0, 1, 0xffff0000)] * 3, "invalid floats"),
            ([(1, 1, 0, 0, 0xffff0000)] * 3, "invalid floats"),
            ([(1, 1, 1, 1, 0xffff0000)] * 3, "fixture space"),
            ([(1, 1, 0, 2, 0xffff0000)] * 3, "fixture space"),
            ([(1, 1, 0, 1, 0x00ff0000)] * 3, "opaque"),
            ([VERTICES[0], VERTICES[1], (230.4, 217.6, 0, 1, 0xff00ff00)], "uniform"),
        ]:
            events = fixture(); events[4]["up_data"]["vertex"]["data"] = encoded_vertices(vertices)
            with self.subTest(vertices=vertices): self.invalid(events, error)
        events = fixture(); events[4]["up_data"]["index"] = {"data": "00"}
        self.invalid(events, "non-indexed")
        events = fixture(); events[2]["color"] = 0xffff0000
        self.invalid(events, "differ")
        events = fixture(); events[4]["up_data"]["vertex"]["data"] = encoded_vertices([VERTICES[0]] * 3)
        self.invalid(events, "degenerate")

    def test_invalid_input_never_creates_output(self):
        events = fixture(); events[3]["hr"] = -1; self.write(events)
        output = Path(self.temp.name) / "new-packet.txt"
        result = subprocess.run([sys.executable, "-B", str(TOOLS / "trace_to_up_packet.py"),
                                 str(self.path), "--synthetic-fixture", "-o", str(output)],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertFalse(output.exists())
        self.assertIn("did not succeed", result.stderr)


if __name__ == "__main__":
    unittest.main()
