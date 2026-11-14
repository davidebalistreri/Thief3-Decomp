"""A bounded diagnostic quad must retain its source camera and index order."""
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
import trace_to_diagnostic_quad as converter


def fixture():
    draw = ir_test.buffered_fixture()
    matrix = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    view = matrix[:]
    view[12] = 0.25
    vertices = [(-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (-0.5, 0.5, 0.5), (0.5, 0.5, 0.5)]
    data = b"".join(struct.pack("<3fI4f", *point, 0xffa06523, 0, 0, 0, 0) for point in vertices)
    draw.update(primitive_count=2, num_vertices=4, vertex_shader=0x242, pixel_shader=0,
                render_states={"168": 7},
                viewport={"x": 0, "y": 0, "width": 1024, "height": 768, "min_z": 0, "max_z": 0.5},
                transforms={"world": [matrix], "view": view, "projection": matrix})
    draw["streams"][0].update(stride=32, description={"size": 512})
    draw["buffer_data"]["vertex"].update(first_vertex=5, vertex_count=4, stride=32,
                                        byte_offset=160, byte_count=len(data), data=data.hex())
    draw["buffer_data"]["index"].update(index_count=6, byte_count=12,
                                       data=struct.pack("<6H", 2, 3, 4, 4, 3, 5).hex())
    events = ir_test.buffered_events(draw)
    events[-2].update(had_reset=False, draws=1)
    return events


class DiagnosticQuadTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-quad-test-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "input.jsonl"

    def convert(self, events, frame=0, draw=0):
        self.path.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
        return converter.convert(self.path, frame, draw)

    def test_preserves_source_camera_viewport_and_indexed_order(self):
        lines = self.convert(fixture()).splitlines()
        self.assertEqual(lines[:4], ["T3R_DIAGNOSTIC_XYZ_V1", "1024 768", "0 0.5", "4278190080"])
        self.assertEqual(float(lines[5].split()[12]), 0.25)
        vertices = [line.split() for line in lines[7:]]
        self.assertEqual([tuple(map(float, item[:3])) for item in vertices],
                         [(-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (-0.5, 0.5, 0.5),
                          (-0.5, 0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5)])
        self.assertTrue(all(int(item[3]) == 0xffa06523 for item in vertices))

    def test_missing_or_unsupported_data_is_rejected(self):
        for change in ("wrong_frame", "wrong_draw", "unstable", "shader", "pixel_shader", "layout", "viewport",
                       "depth", "matrix", "index", "topology", "color", "color_write", "write_failure"):
            events = copy.deepcopy(fixture())
            draw, result = events[2:4]
            frame = number = 0
            if change == "wrong_frame": frame = 1
            if change == "wrong_draw": number = 1
            if change == "unstable": result["buffer_stable"] = False
            if change == "shader": draw["vertex_shader"] = 0x42
            if change == "pixel_shader": draw["pixel_shader"] = 1
            if change == "layout": draw["streams"][0]["stride"] = 28
            if change == "viewport": draw["viewport"]["x"] = 1
            if change == "depth": draw["viewport"]["max_z"] = 2
            if change == "matrix": draw["transforms"]["view"] = None
            if change == "index": draw["buffer_data"]["index"]["data"] = struct.pack("<6H", 2, 3, 4, 4, 3, 6).hex()
            if change == "topology": draw["buffer_data"]["index"]["data"] = struct.pack("<6H", 2, 3, 4, 4, 3, 3).hex()
            if change == "color_write": draw["render_states"]["168"] = 0
            if change == "color":
                data = bytearray.fromhex(draw["buffer_data"]["vertex"]["data"])
                struct.pack_into("<I", data, 32 + 12, 0xff00ff00)
                draw["buffer_data"]["vertex"]["data"] = data.hex()
            if change == "write_failure": result["hr"] = -1
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.convert(events, frame, number)

    def test_cli_does_not_overwrite_or_create_output_for_rejected_input(self):
        self.convert(fixture())
        output = Path(self.temp.name) / "packet.txt"
        output.write_text("keep", encoding="ascii")
        command = [sys.executable, "-B", str(HERE.parent / "tools" / "trace_to_diagnostic_quad.py"),
                   str(self.path), "--frame", "0", "--draw", "0", "-o", str(output)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_text(), "keep")
        output.unlink()
        events = fixture()
        events[3]["buffer_stable"] = False
        self.path.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
