import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "trace_summary.py"
spec = importlib.util.spec_from_file_location("trace_summary", SCRIPT)
trace_summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace_summary)

HEADER = {"event": "header", "format": "t3render-d3d8-diagnostic", "version": 1}
BEGIN = {"event": "frame_begin", "frame": 7}
DRAW = {"event": "draw", "frame": 7, "call": "DrawIndexedPrimitive"}
END = {"event": "frame_end", "frame": 7, "present_hr": 0}
CAPTURE_END = {"event": "capture_end", "frames": 1}
HEADER_OPS = dict(HEADER, operation_results=1, clear_rect_limit=256)
DRAW_OP = dict(DRAW, op_id=2)
DRAW_RESULT = {"event": "draw_result", "frame": 7, "op_id": 2, "hr": 0}
CLEAR = {"event": "clear", "frame": 7, "op_id": 1, "rect_count": 0, "rects_status": "none",
         "rects": [], "flags": 1, "color": 0xff000000, "depth": 1.0, "depth_bits": 0x3f800000, "stencil": 0}
CLEAR_RESULT = {"event": "clear_result", "frame": 7, "op_id": 1, "hr": 0}
END_OPS = dict(END, concurrent_operations=False)


class TraceSummaryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.trace = Path(self.temp.name) / "trace.jsonl"

    def write_events(self, *events):
        self.trace.write_text("\n".join(json.dumps(event) for event in events) + "\n", encoding="utf-8")

    def assert_invalid(self, events, message):
        self.write_events(*events)
        with self.assertRaisesRegex(trace_summary.TraceError, message):
            trace_summary.summarize(self.trace)

    def test_summary_counts_per_call_and_frame_without_interpreting_other_events(self):
        self.write_events(
            HEADER,
            {"event": "device_info", "frame": "opaque"},
            BEGIN,
            DRAW,
            DRAW,
            {"event": "draw", "frame": 7, "call": "DrawPrimitive", "details": {"count": 3}},
            END,
            {"event": "frame_begin", "frame": 8},
            {"event": "frame_end", "frame": 8, "present_hr": -1},
            {"event": "capture_end", "frames": 2},
        )
        result = trace_summary.summarize(self.trace)
        self.assertEqual(result["frame_count"], 2)
        self.assertEqual(result["draw_count"], 3)
        self.assertEqual(result["calls"], {"DrawIndexedPrimitive": 2, "DrawPrimitive": 1})
        self.assertEqual(result["events"]["device_info"], 1)
        self.assertEqual(result["frames"][0]["calls"], result["calls"])
        self.assertEqual(result["frames"][1]["present_hr"], -1)
        self.assertEqual(result["frames"][1]["draws"], 0)

    def test_json_cli_and_invalid_cli(self):
        self.write_events(HEADER, BEGIN, DRAW, END, CAPTURE_END)
        result = subprocess.run(
            [sys.executable, "-B", str(SCRIPT), str(self.trace), "--json"],
            text=True, capture_output=True, check=True,
        )
        self.assertEqual(json.loads(result.stdout)["draw_count"], 1)
        self.write_events(HEADER, BEGIN, DRAW)
        failed = subprocess.run(
            [sys.executable, "-B", str(SCRIPT), str(self.trace)], text=True, capture_output=True,
        )
        self.assertEqual(failed.returncode, 1)
        self.assertIn("unexpected EOF", failed.stderr)
        self.assertNotIn("Traceback", failed.stderr)

    def test_header_and_required_types(self):
        cases = [
            ([BEGIN, DRAW, END], "first event must be header"),
            ([dict(HEADER, format="wrong")], "unsupported trace format"),
            ([dict(HEADER, version=True)], "unsupported trace version"),
            ([dict(HEADER, version=1.0)], "unsupported trace version"),
            ([HEADER, HEADER], "duplicate header"),
            ([HEADER, {"event": "frame_begin", "frame": True}], "frame must be an integer"),
            ([HEADER, BEGIN, dict(DRAW, call=3)], "draw call must be a non-empty string"),
            ([HEADER, BEGIN, dict(DRAW, call="")], "draw call must be a non-empty string"),
            ([HEADER, BEGIN, DRAW, dict(END, present_hr=True)], "present_hr must be an integer"),
            ([HEADER, BEGIN, DRAW, {"event": "frame_end", "frame": 7}], "present_hr must be an integer"),
            ([HEADER, {"event": None}], "event must be a non-empty string"),
            ([HEADER, []], "expected a JSON object"),
        ]
        for events, message in cases:
            with self.subTest(events=events):
                self.assert_invalid(events, message)

    def test_frame_order_and_ids(self):
        cases = [
            ([HEADER, DRAW], "draw outside a frame"),
            ([HEADER, END], "frame_end outside a frame"),
            ([HEADER, BEGIN, BEGIN], "frame_begin before previous frame_end"),
            ([HEADER, BEGIN, dict(DRAW, frame=8)], "does not match active frame"),
            ([HEADER, BEGIN, DRAW, dict(END, frame=8)], "does not match active frame"),
            ([HEADER, BEGIN, dict(DRAW, frame=False)], "frame must be an integer"),
            ([HEADER, BEGIN, DRAW, dict(END, frame=None)], "frame must be an integer"),
        ]
        for events, message in cases:
            with self.subTest(events=events):
                self.assert_invalid(events, message)

    def test_truncation_and_missing_draws(self):
        self.trace.write_text("", encoding="utf-8")
        with self.assertRaisesRegex(trace_summary.TraceError, "missing header"):
            trace_summary.summarize(self.trace)
        self.assert_invalid([HEADER], "missing capture_end")
        self.assert_invalid([HEADER, {"event": "capture_end", "frames": 0}], "no complete frames")
        self.assert_invalid([HEADER, BEGIN, END, CAPTURE_END], "no draw events")
        self.assert_invalid([HEADER, BEGIN, DRAW], "unexpected EOF")
        self.assert_invalid([HEADER, BEGIN, DRAW, END], "missing capture_end")
        self.trace.write_text(json.dumps(HEADER) + "\n{\"event\":", encoding="utf-8")
        with self.assertRaisesRegex(trace_summary.TraceError, "line 2: invalid JSON"):
            trace_summary.summarize(self.trace)

    def test_rejects_nonstandard_json_and_invalid_utf8(self):
        for payload in [
            '{"event":"header","event":"header"}',
            '{"event":"header","format":"t3render-d3d8-diagnostic","version":NaN}',
        ]:
            with self.subTest(payload=payload):
                self.trace.write_text(payload, encoding="utf-8")
                with self.assertRaisesRegex(trace_summary.TraceError, "invalid JSON"):
                    trace_summary.summarize(self.trace)
        self.trace.write_bytes(b"\xff")
        with self.assertRaisesRegex(trace_summary.TraceError, "cannot read trace"):
            trace_summary.summarize(self.trace)

    def test_final_newline_is_optional(self):
        self.write_events(HEADER, BEGIN, DRAW, END, CAPTURE_END)
        self.trace.write_bytes(self.trace.read_bytes().rstrip(b"\n"))
        self.assertEqual(trace_summary.summarize(self.trace)["draw_count"], 1)

    def test_capture_end_is_final_and_counts_complete_frames(self):
        cases = [
            ([HEADER, BEGIN, DRAW, CAPTURE_END], "capture_end inside a frame"),
            ([HEADER, BEGIN, DRAW, END, {"event": "capture_end"}], "capture_end frames must be an integer"),
            ([HEADER, BEGIN, DRAW, END, dict(CAPTURE_END, frames=True)], "capture_end frames must be an integer"),
            ([HEADER, BEGIN, DRAW, END, dict(CAPTURE_END, frames=2)], "does not match 1 complete frames"),
            ([HEADER, BEGIN, DRAW, END, CAPTURE_END, CAPTURE_END], "event after capture_end"),
            ([HEADER, BEGIN, DRAW, END, CAPTURE_END, BEGIN], "event after capture_end"),
            ([HEADER, BEGIN, DRAW, END, CAPTURE_END, {"event": "state"}], "event after capture_end"),
        ]
        for events, message in cases:
            with self.subTest(events=events):
                self.assert_invalid(events, message)

    def test_requested_window_must_be_complete_and_consecutive(self):
        self.write_events(dict(HEADER, requested_frames=2), BEGIN, DRAW, END,
                          {"event": "frame_begin", "frame": 8},
                          {"event": "frame_end", "frame": 8, "present_hr": 0},
                          dict(CAPTURE_END, frames=2))
        self.assertEqual(trace_summary.summarize(self.trace)["frame_count"], 2)
        for invalid in (None, True, False, 0, -1, 4, 1.0, "2"):
            with self.subTest(invalid=invalid):
                self.assert_invalid([dict(HEADER, requested_frames=invalid)], "requested_frames")
        self.assert_invalid([dict(HEADER, requested_frames=2), BEGIN, DRAW, END, CAPTURE_END],
                            "does not match requested_frames")
        for next_frame in (7, 9, -1, (1 << 64) - 1):
            with self.subTest(frame=next_frame):
                self.assert_invalid([dict(HEADER, requested_frames=2), BEGIN, DRAW, END,
                                     {"event": "frame_begin", "frame": next_frame}],
                                    "not consecutive|outside requested window range")

    def test_legacy_trace_remains_valid_without_requested_window(self):
        self.write_events(HEADER, BEGIN, DRAW, END, dict(BEGIN), dict(DRAW), dict(END),
                          dict(CAPTURE_END, frames=2))
        self.assertEqual(trace_summary.summarize(self.trace)["frame_count"], 2)

    def test_operation_results_are_required_when_advertised(self):
        self.write_events(HEADER_OPS, BEGIN, CLEAR, CLEAR_RESULT, DRAW_OP, DRAW_RESULT, END_OPS, CAPTURE_END)
        result = trace_summary.summarize(self.trace)
        self.assertEqual((result["events"]["clear"], result["events"]["draw_result"]), (1, 1))
        self.assertFalse(result["frames"][0]["concurrent_operations"])
        cases = [
            ([dict(HEADER_OPS, operation_results=True)], "unsupported operation result capability"),
            ([dict(HEADER_OPS, clear_rect_limit=0)], "clear_rect_limit"),
            ([HEADER_OPS, BEGIN, DRAW], "operation id must increase"),
            ([HEADER_OPS, BEGIN, DRAW_OP, END_OPS], "pending operations"),
            ([HEADER_OPS, BEGIN, CLEAR, DRAW_RESULT], "no matching operation"),
            ([HEADER_OPS, BEGIN, CLEAR, dict(CLEAR_RESULT, frame=8)], "does not match active frame"),
            ([HEADER_OPS, BEGIN, CLEAR, dict(CLEAR_RESULT, hr=True)], "operation hr"),
            ([HEADER_OPS, BEGIN, CLEAR, CLEAR_RESULT, dict(DRAW_OP, op_id=1)], "operation id must increase"),
            ([HEADER_OPS, BEGIN, CLEAR, CLEAR_RESULT, DRAW_OP, DRAW_RESULT, END], "concurrent_operations"),
            ([HEADER, BEGIN, CLEAR], "clear requires operation result capability"),
        ]
        for events, message in cases:
            with self.subTest(events=events):
                self.assert_invalid(events, message)

    def test_overlapping_operations_must_be_flagged(self):
        events = [HEADER_OPS, BEGIN, CLEAR, DRAW_OP, DRAW_RESULT, CLEAR_RESULT]
        self.assert_invalid(events + [END_OPS], "overlapping operations were not flagged")
        self.write_events(*events, dict(END_OPS, concurrent_operations=True), CAPTURE_END)
        self.assertTrue(trace_summary.summarize(self.trace)["frames"][0]["concurrent_operations"])

    def test_buffered_draw_result_is_paired_by_frame_and_operation_id(self):
        buffered = dict(DRAW_OP, buffer_data={"source": "application_lock_shadow"})
        for stable in (True, False):
            self.write_events(HEADER_OPS, BEGIN, buffered, dict(DRAW_RESULT, buffer_stable=stable), END_OPS, CAPTURE_END)
            self.assertEqual(trace_summary.summarize(self.trace)["draw_count"], 1)

        # Overlapping draw results can arrive in a different order: the flag
        # must follow its own op_id, not the last draw or event order.
        other = dict(DRAW_OP, op_id=3, call="DrawPrimitive")
        self.write_events(HEADER_OPS, BEGIN, buffered, other, dict(DRAW_RESULT, op_id=3),
                          dict(DRAW_RESULT, buffer_stable=False), dict(END_OPS, concurrent_operations=True), CAPTURE_END)
        self.assertEqual(trace_summary.summarize(self.trace)["draw_count"], 2)
        self.assert_invalid([HEADER_OPS, BEGIN, buffered, other, dict(DRAW_RESULT, op_id=3, buffer_stable=True)],
                            "no matching buffer_data")
        self.assert_invalid([HEADER_OPS, BEGIN, buffered, other, dict(DRAW_RESULT, op_id=3), DRAW_RESULT],
                            "requires boolean buffer_stable")

        cases = [
            ([HEADER_OPS, BEGIN, buffered, DRAW_RESULT], "requires boolean buffer_stable"),
            ([HEADER_OPS, BEGIN, buffered, dict(DRAW_RESULT, buffer_stable=0)], "requires boolean buffer_stable"),
            ([HEADER_OPS, BEGIN, buffered, dict(DRAW_RESULT, buffer_stable=None)], "requires boolean buffer_stable"),
            ([HEADER_OPS, BEGIN, buffered, dict(DRAW_RESULT, buffer_stable="false")], "requires boolean buffer_stable"),
            ([HEADER_OPS, BEGIN, DRAW_OP, dict(DRAW_RESULT, buffer_stable=True)], "no matching buffer_data"),
            ([HEADER_OPS, BEGIN, CLEAR, dict(CLEAR_RESULT, buffer_stable=True)], "no matching buffer_data"),
            ([HEADER_OPS, BEGIN, dict(DRAW_OP, buffer_data=None)], "buffer_data must be an object"),
            ([HEADER, BEGIN, dict(DRAW, buffer_data={})], "requires operation result capability"),
            ([HEADER_OPS, BEGIN, buffered, dict(DRAW_RESULT, frame=8, buffer_stable=True)], "does not match active frame"),
            ([HEADER_OPS, BEGIN, buffered, dict(DRAW_RESULT, op_id=3, buffer_stable=True)], "no matching operation"),
            ([HEADER_OPS, BEGIN, buffered, END_OPS], "pending operations"),
        ]
        for events, message in cases:
            with self.subTest(events=events): self.assert_invalid(events, message)

        # Original traces without buffered observations remain valid.
        self.write_events(HEADER_OPS, BEGIN, DRAW_OP, DRAW_RESULT, END_OPS, CAPTURE_END)
        self.assertEqual(trace_summary.summarize(self.trace)["draw_count"], 1)

    def test_declaration_and_shader_stability_are_paired_without_requiring_optional_fields(self):
        draw = dict(DRAW_OP, vertex_declaration={"status": "candidate"})
        for result in (DRAW_RESULT, dict(DRAW_RESULT, shader_stable=True), dict(DRAW_RESULT, shader_stable=False)):
            self.write_events(HEADER_OPS, BEGIN, draw, result, END_OPS, CAPTURE_END)
            self.assertEqual(trace_summary.summarize(self.trace)["draw_count"], 1)
        # A getter interrupted before publishing the snapshot can still emit
        # shader_stable, but there is no declaration to expose.
        self.write_events(HEADER_OPS, BEGIN, DRAW_OP, dict(DRAW_RESULT, shader_stable=True), END_OPS, CAPTURE_END)
        self.assertEqual(trace_summary.summarize(self.trace)["draw_count"], 1)
        for invalid in (None, 0, 1, "true"):
            with self.subTest(invalid=invalid):
                self.assert_invalid([HEADER_OPS, BEGIN, draw, dict(DRAW_RESULT, shader_stable=invalid)],
                                    "shader_stable requires a boolean draw result")
        self.assert_invalid([HEADER_OPS, BEGIN, CLEAR, dict(CLEAR_RESULT, shader_stable=True)],
                            "shader_stable requires a boolean draw result")
        self.assert_invalid([HEADER_OPS, BEGIN, dict(DRAW_OP, vertex_declaration=None)],
                            "vertex_declaration must be an object")
        self.assert_invalid([HEADER, BEGIN, dict(DRAW, vertex_declaration={})],
                            "vertex_declaration requires operation result capability")
        self.assert_invalid([HEADER_OPS, BEGIN, draw, dict(DRAW_RESULT, op_id=3, shader_stable=True)],
                            "no matching operation")

    def test_clear_rectangles_are_never_fabricated(self):
        cases = [
            (dict(CLEAR, rects_status="captured", rect_count=2, rects=[]), "invalid captured"),
            (dict(CLEAR, rects_status="captured", rect_count=1,
                  rects=[{"x1": False, "y1": 0, "x2": 1, "y2": 2}]), "invalid clear rectangle"),
            (dict(CLEAR, rects_status="null_pointer", rect_count=1, rects=[]), "must be null"),
            (dict(CLEAR, rects_status="null_pointer", rect_count=0, rects=None), "status does not match count"),
            (dict(CLEAR, rects_status="count_limit", rect_count=2, rects=None), "status does not match count"),
            (dict(CLEAR, flags=True), "invalid clear flags"),
            (dict(CLEAR, depth="1"), "invalid clear depth"),
        ]
        for event, message in cases:
            with self.subTest(event=event):
                self.assert_invalid([HEADER_OPS, BEGIN, event], message)
        raw = json.dumps(CLEAR).replace('"depth": 1.0', '"depth": 1e999')
        self.trace.write_text(json.dumps(HEADER_OPS) + "\n" + json.dumps(BEGIN) + "\n" + raw + "\n", encoding="utf-8")
        with self.assertRaisesRegex(trace_summary.TraceError, "invalid clear depth"):
            trace_summary.summarize(self.trace)


if __name__ == "__main__":
    unittest.main()
