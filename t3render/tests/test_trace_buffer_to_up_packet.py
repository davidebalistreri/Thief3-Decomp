"""Fail-closed conversion of one proxy-observed indexed fixture draw."""

import copy
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_trace_to_up_packet import fixture as up_fixture, VERTICES

TOOLS = HERE.parent / "tools"
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("trace_buffer_to_up_packet", TOOLS / "trace_buffer_to_up_packet.py")
converter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(converter)


def fixture():
    events = up_fixture()
    events[0]["buffer_capture"] = {"requested": True, "encoding": "hex",
                                   "component_limit_bytes": 256 * 1024, "frame_limit_bytes": 8 * 1024 * 1024}
    draw = events[4]
    draw.pop("up_data")
    draw.update(call="DrawIndexedPrimitive", vertex_stride=0, min_vertex_index=1,
                num_vertices=3, start_index=2,
                streams=[{"stream": 0, "hr": 0, "buffer": "0x123", "stride": 20,
                          "description": {"size": 100, "fvf": 0x44}}],
                indices={"hr": 0, "buffer": "0x456", "base_vertex_index": 1,
                         "description": {"format": 101, "size": 10}})
    vertices = b"".join(struct.pack("<4fI", *v) for v in VERTICES)
    draw["buffer_data"] = {"source": "application_lock_shadow",
        "vertex": {"status": "captured", "byte_count": 60, "encoding": "hex", "data": vertices.hex(),
                   "resource": "0x123", "stream": 0, "first_vertex": 2, "vertex_count": 3,
                   "stride": 20, "byte_offset": 40},
        "index": {"status": "captured", "byte_count": 6, "encoding": "hex",
                  "data": struct.pack("<3H", 1, 2, 3).hex(), "resource": "0x456",
                  "index_count": 3, "index_format": 101, "byte_offset": 4}}
    events[5]["buffer_stable"] = True
    return events


class TraceBufferToUpPacketTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-buffer-trace-test-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "input.jsonl"

    def convert(self, events, attested=True):
        self.path.write_text("".join(json.dumps(e) + "\n" for e in events), encoding="utf-8")
        return converter.convert(self.path, synthetic_fixture=attested)

    def invalid(self, events):
        with self.assertRaises((converter.up.PacketError, ValueError)):
            self.convert(events)

    def test_fixture_resolves_observed_indexed_vertices(self):
        result = self.convert(fixture()).splitlines()
        self.assertEqual(result[:3], ["T3R_UP_V1", "256 256", str(0xff000000)])
        for line, original in zip(result[3:], VERTICES):
            values = line.split()
            self.assertEqual(struct.pack("<4fI", *map(float, values[:4]), int(values[4])),
                             struct.pack("<4fI", *original))

    def test_requires_attestation_and_stable_result(self):
        with self.assertRaises(converter.up.PacketError):
            self.convert(fixture(), attested=False)
        for value in (False, None, 1, "true"):
            events = fixture(); events[5]["buffer_stable"] = value
            with self.subTest(value=value): self.invalid(events)
        events = fixture(); del events[5]["buffer_stable"]; self.invalid(events)
        events = fixture(); events[0]["buffer_capture"]["requested"] = False; self.invalid(events)

    def test_invalid_bindings_and_draws_are_rejected(self):
        for edit in ("call", "start", "base", "stride", "size", "format", "identity", "status"):
            events = fixture(); draw = events[4]
            if edit == "call": draw["call"] = "DrawPrimitive"
            if edit == "start": draw["start_index"] = 0
            if edit == "base": draw["indices"]["base_vertex_index"] = 0
            if edit == "stride": draw["streams"][0]["stride"] = 24
            if edit == "size": draw["streams"][0]["description"]["size"] = 99
            if edit == "format": draw["indices"]["description"]["format"] = 102
            if edit == "identity": draw["buffer_data"]["vertex"]["resource"] = "0x999"
            if edit == "status": draw["buffer_data"]["index"]["status"] = "unknown"
            with self.subTest(edit=edit): self.invalid(events)
        for component, binding in (("vertex", "streams"), ("index", "indices")):
            for bad in ("garbage", "0x0", "0X1", "0x-1", "0x100000000"):
                events = fixture(); draw = events[4]
                target = draw[binding][0] if binding == "streams" else draw[binding]
                target["buffer"] = bad
                draw["buffer_data"][component]["resource"] = bad
                with self.subTest(component=component, identity=bad): self.invalid(events)

    def test_invalid_ranges_bytes_and_indices_are_rejected(self):
        for edit in ("offset", "count", "truncated", "nonhex", "outside", "degenerate", "color"):
            events = fixture(); data = events[4]["buffer_data"]
            if edit == "offset": data["vertex"]["byte_offset"] = 20
            if edit == "count": data["index"]["index_count"] = 4
            if edit == "truncated": data["vertex"]["data"] = data["vertex"]["data"][:-2]
            if edit == "nonhex": data["index"]["data"] = "z" * 12
            if edit == "outside": data["index"]["data"] = struct.pack("<3H", 0, 2, 3).hex()
            if edit == "degenerate": data["index"]["data"] = struct.pack("<3H", 1, 1, 1).hex()
            if edit == "color":
                values = list(VERTICES); values[1] = (*values[1][:4], 0xff00ff00)
                data["vertex"]["data"] = b"".join(struct.pack("<4fI", *v) for v in values).hex()
            with self.subTest(edit=edit): self.invalid(events)

    def test_operation_outcomes_and_unsupported_state_are_rejected(self):
        for edit in ("draw_failed", "present_failed", "concurrent", "reset", "state", "extra"):
            events = fixture()
            if edit == "draw_failed": events[5]["hr"] = -1
            if edit == "present_failed": events[6]["present_hr"] = -1
            if edit == "concurrent": events[6]["concurrent_operations"] = True
            if edit == "reset": events[6]["had_reset"] = True
            if edit == "state": events[4]["render_states"]["7"] = 1
            if edit == "extra": events.insert(4, {"event": "unobserved_operation", "frame": 0})
            with self.subTest(edit=edit): self.invalid(events)

    def test_cli_never_overwrites_and_invalid_input_creates_no_output(self):
        self.convert(fixture())
        output = Path(self.temp.name) / "packet.txt"
        output.write_text("keep", encoding="ascii")
        command = [sys.executable, "-B", str(TOOLS / "trace_buffer_to_up_packet.py"),
                   str(self.path), "--synthetic-fixture", "-o", str(output)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_text(), "keep")
        output.unlink()
        broken = fixture(); broken[5]["buffer_stable"] = False
        self.path.write_text("".join(json.dumps(e) + "\n" for e in broken), encoding="utf-8")
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
