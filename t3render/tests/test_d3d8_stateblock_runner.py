"""Host validation of the isolated state-block fixture's evidence."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_trace_buffer_to_up_packet import fixture as buffered_fixture
import run_d3d8_stateblock_fixture as runner


def trace_fixture():
    records = buffered_fixture()
    draw = records[4]
    draw.update(min_vertex_index=0, num_vertices=3, start_index=0)
    stream = draw["streams"][0]
    stream["description"].update(size=60, usage=8, pool=1)
    index = draw["indices"]
    index["base_vertex_index"] = 0
    index["description"].update(size=6, usage=8, pool=1)
    vertex = draw["buffer_data"]["vertex"]
    vertex.update(status="window_unavailable", data=None, first_vertex=0, byte_offset=0)
    index_data = draw["buffer_data"]["index"]
    index_data.update(status="window_unavailable", data=None, byte_offset=0)
    records[5]["buffer_stable"] = False
    return records


class StateBlockRunnerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="t3render-stateblock-test-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def inspect_trace(self, records):
        path = self.root / "trace.jsonl"
        path.write_text("".join(json.dumps(event) + "\n" for event in records), encoding="utf-8")
        runner.inspect_trace(path)

    def test_accepts_ordered_fixture_with_unavailable_shadows(self):
        self.inspect_trace(trace_fixture())

    def test_rejects_uncertain_or_inconsistent_capture(self):
        for edit in ("request", "capability", "draw", "stable", "bytes", "alias", "usage", "pool", "failure", "order"):
            records = trace_fixture()
            draw = records[4]
            if edit == "request": records[0]["buffer_capture"]["requested"] = False
            if edit == "capability": records[0]["buffer_capture"] = True
            if edit == "draw": draw["call"] = "DrawPrimitive"
            if edit == "stable": records[5]["buffer_stable"] = True
            if edit == "bytes": draw["buffer_data"]["vertex"]["data"] = "00" * 60
            if edit == "alias": draw["buffer_data"]["index"]["resource"] = "0x999"
            if edit == "usage": draw["streams"][0]["description"]["usage"] = 0
            if edit == "pool": draw["indices"]["description"]["pool"] = 0
            if edit == "failure": records[5]["hr"] = -1
            if edit == "order": records.insert(4, {"event": "other", "frame": 0})
            with self.subTest(edit=edit), self.assertRaises(runner.RunError):
                self.inspect_trace(records)

    def test_markers_require_complete_order_and_valid_outcomes(self):
        path = self.root / "fixture.log"
        path.write_text("\n".join(runner.MARKERS) + "\n", encoding="utf-8")
        runner.inspect_log(path)
        for lines in (runner.MARKERS[:-1], runner.MARKERS + [runner.MARKERS[0]],
                      [x.replace("=PASS", "=FAIL", 1) for x in runner.MARKERS],
                      list(reversed(runner.MARKERS))):
            path.write_text("\n".join(lines), encoding="utf-8")
            with self.assertRaises(runner.RunError): runner.inspect_log(path)

    def test_image_requires_complete_red_triangle(self):
        pixels = bytearray(256 * 256 * 3)
        for i in range(1, 1002): pixels[i * 3:i * 3 + 3] = b"\xff\x00\x00"
        center = (128 * 256 + 128) * 3
        pixels[center:center + 3] = b"\xff\x00\x00"
        valid = runner.HEADER + pixels
        path = self.root / "image.ppm"
        path.write_bytes(valid)
        self.assertEqual(runner.inspect_image(path), valid)
        for invalid in (valid[:-1], valid + b"x", b"X" + valid[1:], runner.HEADER + bytes(len(pixels))):
            path.write_bytes(invalid)
            with self.assertRaises(runner.RunError): runner.inspect_image(path)


if __name__ == "__main__":
    unittest.main()
