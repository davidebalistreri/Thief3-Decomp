"""Camera fixture conversion: captured matrices and XYZ bytes, fail closed."""
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_trace_to_up_packet as base

packet = base.packet
VERTICES = [(-0.7, 0.8, 0.0, 0xffff0000), (0.8, 0.0, 0.2, 0xffff0000),
            (-0.7, -0.8, 0.1, 0xffff0000)]
WORLD = [0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0.1, -0.05, 2, 1]
VIEW = [0.9, 0, 0, 0, 0, 1.1, 0, 0, 0, 0, 1, 0, -0.05, 0.05, 0.5, 1]
PROJECTION = [1.8, 0, 0, 0, 0, 1.8, 0, 0, 0, 0, 8/7.5, 1, 0, 0, -4/7.5, 0]


def fixture():
    events = base.fixture()
    draw = events[4]
    draw.update(vertex_stride=16, vertex_shader=0x42,
                transforms={"world": [list(WORLD)], "view": list(VIEW), "projection": list(PROJECTION)})
    vertex = draw["up_data"]["vertex"]
    vertex.update(stride=16, byte_count=48,
                  data=b"".join(struct.pack("<3fI", *v) for v in VERTICES).hex())
    return events


class CameraPacketTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-camera-packet-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "trace.jsonl"

    def convert(self, events, **kwargs):
        self.path.write_text("".join(json.dumps(e) + "\n" for e in events), encoding="utf-8")
        return packet.convert(self.path, **kwargs)

    def valid(self, events):
        return self.convert(events, synthetic_fixture=True)

    def test_captured_matrices_and_vertices_roundtrip_float32(self):
        lines = self.valid(fixture()).splitlines()
        self.assertEqual(lines[:3], ["T3R_XYZ_V1", "256 256", str(0xff000000)])
        self.assertEqual(len(lines), 9)
        for line, matrix in zip(lines[3:6], (WORLD, VIEW, PROJECTION)):
            self.assertEqual(struct.pack("<16f", *map(float, line.split())), struct.pack("<16f", *matrix))
        for line, vertex in zip(lines[6:], VERTICES):
            values = line.split()
            self.assertEqual(struct.pack("<3fI", *map(float, values[:3]), int(values[3])),
                             struct.pack("<3fI", *vertex))

    def test_requires_fixture_attestation(self):
        with self.assertRaises(packet.PacketError):
            self.convert(fixture())

    def test_missing_malformed_or_nonfinite_matrix_is_rejected(self):
        for replacement in (None, [], [0]*15, [0]*17, [False]*16, [10001]*16, [float('nan')]*16):
            events = fixture()
            events[4]["transforms"]["view"] = replacement
            with self.subTest(replacement=replacement), self.assertRaises((packet.PacketError, ValueError)):
                self.valid(events)
        events = fixture(); events[4]["transforms"]["world"] = []
        with self.assertRaises(packet.PacketError): self.valid(events)
        events = fixture(); del events[4]["transforms"]
        with self.assertRaises(packet.PacketError): self.valid(events)

    def test_outside_clip_degenerate_and_unsupported_vertex_are_rejected(self):
        for edit in ("outside", "behind", "degenerate", "nan", "stride", "shader", "varying"):
            events = fixture()
            draw = events[4]
            vertices = list(VERTICES)
            if edit == "outside": vertices[0] = (100, 100, 0, 0xffff0000)
            if edit == "behind": draw["transforms"]["projection"][11] = -1
            if edit == "degenerate": vertices = [vertices[0]] * 3
            if edit == "nan": vertices[0] = (math.nan, 0, 0, 0xffff0000)
            if edit == "stride": draw["vertex_stride"] = 20
            if edit == "shader": draw["vertex_shader"] = 0x44
            if edit == "varying": vertices[2] = (-0.7, -0.8, 0.1, 0xff00ff00)
            draw["up_data"]["vertex"]["data"] = b"".join(struct.pack("<3fI", *v) for v in vertices).hex()
            with self.subTest(edit=edit), self.assertRaises(packet.PacketError): self.valid(events)


if __name__ == "__main__":
    unittest.main()
