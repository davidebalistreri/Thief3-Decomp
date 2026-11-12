"""Bounded multi-draw conversion preserves order and rejects uncertain input."""
import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_trace_to_camera_packet as camera
import trace_to_multi_xyz_packet as multi
import trace_summary


def fixture():
    events = camera.fixture()
    second = copy.deepcopy(events[4])
    second["op_id"] = 3
    f32 = lambda value: struct.unpack("<f", struct.pack("<f", value))[0]
    second["transforms"]["world"][0][12] = f32(f32(0.1) + f32(0.3))
    second["transforms"]["view"][12] = f32(f32(-0.05) - f32(0.08))
    second["transforms"]["projection"][0] = second["transforms"]["projection"][5] = f32(f32(1.8) * f32(0.95))
    vertices = [(x, y, z, 0xff00ff00) for x, y, z, _ in camera.VERTICES]
    second["up_data"]["vertex"]["data"] = b"".join(struct.pack("<3fI", *v) for v in vertices).hex()
    events[6:6] = [second, {"event": "draw_result", "frame": 0, "op_id": 3, "hr": 0}]
    events[-2]["draws"] = 2
    return events


class MultiCameraPacketTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-multi-packet-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "capture.jsonl"

    def convert(self, events, attested=True):
        self.path.write_text("".join(json.dumps(e) + "\n" for e in events), encoding="utf-8")
        return multi.convert(self.path, synthetic_fixture=attested)

    def test_preserves_each_draw_in_order(self):
        lines = self.convert(fixture()).splitlines()
        self.assertEqual(lines[:4], ["T3R_MULTI_XYZ_V1", "256 256", str(0xff000000), "2"])
        self.assertEqual(len(lines), 16)
        self.assertAlmostEqual(float(lines[4].split()[12]), 0.1)
        self.assertAlmostEqual(float(lines[10].split()[12]), 0.4)
        self.assertAlmostEqual(float(lines[11].split()[12]), -0.13)
        self.assertAlmostEqual(float(lines[12].split()[0]), 1.71, places=5)
        self.assertEqual(int(lines[7].split()[3]), 0xffff0000)
        self.assertEqual(int(lines[13].split()[3]), 0xff00ff00)

    def test_rejects_unattested_or_unsafe_scenes(self):
        with self.assertRaises(multi.single.PacketError): self.convert(fixture(), False)
        cases = []
        for edit in ("extra_event", "missing_result", "failed_draw", "duplicate_id", "wrong_frame",
                     "wrong_count", "depth", "texture", "shader", "missing_bytes", "reset", "concurrent"):
            events = fixture()
            if edit == "extra_event": events.insert(6, {"event": "copy_rects", "frame": 0})
            if edit == "missing_result": events.pop(7)
            if edit == "failed_draw": events[7]["hr"] = -1
            if edit == "duplicate_id": events[6]["op_id"] = events[7]["op_id"] = 2
            if edit == "wrong_frame": events[6]["frame"] = 1
            if edit == "wrong_count": events[-2]["draws"] = 1
            if edit == "depth": events[6]["render_states"]["7"] = 1
            if edit == "texture": events[6]["stages"][0]["texture"] = {"id": "0x123"}
            if edit == "shader": events[6]["vertex_shader"] = 0x44
            if edit == "missing_bytes": events[6]["up_data"]["vertex"]["data"] = ""
            if edit == "reset": events[-2]["had_reset"] = True
            if edit == "concurrent": events[-2]["concurrent_operations"] = True
            cases.append((edit, events))
        for edit, events in cases:
            with self.subTest(edit=edit), self.assertRaises((ValueError, trace_summary.TraceError)):
                self.convert(events)


if __name__ == "__main__":
    unittest.main()
