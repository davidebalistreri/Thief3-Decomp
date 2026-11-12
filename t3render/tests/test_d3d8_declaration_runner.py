"""Validate the isolated declaration fixture's evidence checks without Wine."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import run_d3d8_declaration_fixture as runner


def trace(enabled):
    header = {"event": "header", "format": "t3render-d3d8-diagnostic", "version": 1,
              "operation_results": 1, "clear_rect_limit": 256, "declaration_capture": {"requested": enabled}}
    clear = {"event": "clear", "frame": 0, "op_id": 1, "rect_count": 0, "rects_status": "none",
             "rects": [], "flags": 1, "color": 0xff000000, "depth": 1.0, "depth_bits": 0x3f800000, "stencil": 0}
    result = [header, {"event": "frame_begin", "frame": 0}, clear,
              {"event": "clear_result", "frame": 0, "op_id": 1, "hr": 0}]
    for n in range(2):
        draw = {"event": "draw", "frame": 0, "op_id": n + 2, "call": "DrawPrimitiveUP", "primitive_type": 4,
                "primitive_count": 1, "vertex_stride": 16, "vertex_shader": 0x100 + n}
        finish = {"event": "draw_result", "frame": 0, "op_id": n + 2, "hr": 0}
        if enabled:
            draw["vertex_declaration"] = {"status": "candidate", "generation": n + 1, "words": runner.WORDS[:]}
            finish["shader_stable"] = True
        result += [draw, finish]
    return result + [{"event": "frame_end", "frame": 0, "present_hr": 0, "draws": 2,
                      "had_reset": False, "concurrent_operations": False},
                     {"event": "capture_end", "frames": 1}]


class DeclarationRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3-declaration-runner-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "trace.jsonl"

    def check(self, events, enabled):
        self.path.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
        runner.inspect_trace(self.path, [0x100, 0x101], enabled)

    def test_accepts_each_mode(self):
        self.check(trace(False), False)
        self.check(trace(True), True)

    def test_rejects_wrong_association_and_unstable_or_modified_declaration(self):
        for case in ("handle", "op_id", "generation", "boolean_generation", "boolean_count", "words", "stability", "request", "capability", "result"):
            events = copy.deepcopy(trace(True))
            if case == "handle": events[4]["vertex_shader"] += 1
            if case == "op_id": events[5]["op_id"] += 1
            if case == "generation": events[4]["vertex_declaration"]["generation"] = 2
            if case == "boolean_generation": events[4]["vertex_declaration"]["generation"] = True
            if case == "boolean_count": events[4]["primitive_count"] = True
            if case == "words": events[4]["vertex_declaration"]["words"][1] = 0
            if case == "stability": events[5]["shader_stable"] = False
            if case == "request": events[0]["declaration_capture"]["requested"] = False
            if case == "capability": events[0]["declaration_capture"] = None
            if case == "result": events[5]["hr"] = -1
            with self.subTest(case=case), self.assertRaises((runner.RunError, runner.trace_summary.TraceError)):
                self.check(events, True)

    def test_rejects_unexpected_capture_when_disabled(self):
        events = trace(False)
        events[4]["vertex_declaration"] = {"status": "unknown", "generation": None, "words": None}
        with self.assertRaises(runner.RunError):
            self.check(events, False)

    def test_requires_complete_successful_ordered_hresult_sequence(self):
        lines = [f"parity {name} hr=00000000" for name in runner.EXPECTED_CALLS]
        lines += ["handle 0=00000100", "handle 1=00000101", "fixture result=PASS"]
        self.path.write_text("\n".join(lines), encoding="utf-8")
        self.assertEqual(runner.inspect_log(self.path)[1], [0x100, 0x101])
        for pos, replacement in ((0, "parity GetAdapterDisplayMode hr=80004005"),
                                 (16, "parity WRONG hr=00000000"), (-1, "fixture result=FAIL")):
            altered = lines[:]
            altered[pos] = replacement
            self.path.write_text("\n".join(altered), encoding="utf-8")
            with self.subTest(pos=pos), self.assertRaises(runner.RunError):
                runner.inspect_log(self.path)

    def test_requires_two_distinct_visible_triangles(self):
        rgb = bytearray(256 * 256 * 3)
        for y in range(100, 140):
            for x in range(40, 80):
                offset = (y * 256 + x) * 3
                rgb[offset:offset + 3] = b"\xff\0\0"
            for x in range(170, 210):
                offset = (y * 256 + x) * 3
                rgb[offset:offset + 3] = b"\0\xff\0"
        self.path.write_bytes(runner.HEADER + rgb)
        runner.inspect_image(self.path)
        rgb[(128 * 256 + 192) * 3:(128 * 256 + 192) * 3 + 3] = b"\0\0\0"
        self.path.write_bytes(runner.HEADER + rgb)
        with self.assertRaises(runner.RunError):
            runner.inspect_image(self.path)


if __name__ == "__main__":
    unittest.main()
