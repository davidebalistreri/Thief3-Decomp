"""Run the real capture writer against small POSIX/D3D test doubles.

This validates host-side behavior, not the Windows ABI, DLL hook, or TDS.
"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("trace_summary", ROOT / "tools" / "trace_summary.py")
trace_summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace_summary)


class CaptureHostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++")
        if not compiler or os.name != "posix":
            raise unittest.SkipTest("POSIX and clang++ are required for the capture host test")
        cls.build = tempfile.TemporaryDirectory(prefix="t3render-host-test-")
        cls.binary = Path(cls.build.name) / "capture_fake"
        result = subprocess.run([
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I", str(ROOT / "tests" / "fakes"), "-I", str(ROOT / "src"),
            str(ROOT / "src" / "capture.cpp"), str(ROOT / "tests" / "capture_fake.cpp"),
            "-o", str(cls.binary),
        ], text=True, capture_output=True)
        if result.returncode:
            cls.build.cleanup()
            raise RuntimeError("host capture compilation failed:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-trace-test-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "trace.jsonl"

    def run_capture(self, scenario="frame", *, frame=None, key=None, extra=None, path=True):
        env = {k: v for k, v in os.environ.items() if not k.startswith(("T3RENDER_", "T3_TEST_"))}
        if path:
            env["T3RENDER_CAPTURE_PATH"] = str(self.path)
        if frame is not None:
            env["T3RENDER_CAPTURE_FRAME"] = frame
        if key is not None:
            env["T3RENDER_CAPTURE_KEY"] = key
        if extra:
            env.update(extra)
        result = subprocess.run([str(self.binary), scenario], env=env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("state=unchanged", result.stdout)
        return result.stdout

    def records(self):
        return [json.loads(line) for line in self.path.read_text(encoding="utf-8").splitlines()]

    def test_frame_trigger_records_actual_snapshots_and_balances_references(self):
        output = self.run_capture(frame="1")
        self.assertIn("configured=1", output)
        summary = trace_summary.summarize(self.path)
        self.assertEqual(summary["draw_count"], 4)
        self.assertEqual(summary["calls"], {
            "DrawIndexedPrimitive": 1, "DrawPrimitive": 1,
            "DrawIndexedPrimitiveUP": 1, "DrawPrimitiveUP": 1,
        })
        self.assertEqual(summary["frames"][0]["frame"], 1)
        self.assertEqual(summary["frames"][0]["present_hr"], -7)
        events = self.records()
        self.assertEqual([e["event"] for e in events],
                         ["header", "frame_begin", "draw", "draw_result", "draw", "draw_result",
                          "draw", "draw_result", "draw", "draw_result", "reset", "frame_end", "capture_end"])
        self.assertFalse(events[0]["replayable"])
        self.assertEqual(events[0]["operation_results"], 1)
        self.assertEqual(events[1]["presentation"]["width"], 1280)
        draw = events[2]
        self.assertEqual(draw["viewport"]["height"], 720)
        self.assertEqual(draw["transforms"]["view"][12], 12)
        self.assertIsNone(draw["transforms"]["world"][0])
        self.assertEqual(draw["render_states"], {"7": 42})
        self.assertEqual(draw["material"]["power"], 16)
        self.assertEqual(draw["lights"][0]["position"], [1, 2, 3])
        self.assertEqual(draw["stages"][0]["texture"]["width"], 64)
        self.assertEqual(draw["streams"][0]["stride"], 32)
        self.assertEqual(draw["indices"]["base_vertex_index"], 3)
        self.assertEqual(draw["vertex_shader"], 444)
        self.assertEqual(draw["pixel_shader"], 555)
        self.assertTrue(events[-2]["had_reset"])
        self.assertEqual(events[-2]["draws"], 4)

    def test_only_sdk_defined_observable_state_ids_are_queried(self):
        # The fake deliberately accepts *every* numeric ID, like a permissive
        # driver, so this verifies the writer's filtering rather than the fake's.
        self.run_capture(frame="1", extra={"T3_TEST_ALL_STATES": "1"})
        draw = next(event for event in self.records() if event["event"] == "draw")
        render_ids = {
            7, 8, 9, 10, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 30,
            34, 35, 36, 37, 38, 40, 47, 48, 52, 53, 54, 55, 56, 57, 58, 59, 60,
            *range(128, 138), *range(139, 144), *range(145, 149), *range(151, 165),
            *range(166, 169), *range(170, 174),
        }
        self.assertEqual({int(key) for key in draw["render_states"]}, render_ids)
        for key, value in draw["render_states"].items():
            self.assertEqual(value, int(key))
        stage_ids = {*range(1, 12), *range(13, 29)}
        for stage in draw["stages"]:
            self.assertEqual({int(key) for key in stage["states"]}, stage_ids)
            for key, value in stage["states"].items():
                self.assertEqual(value, stage["stage"] * 1000 + int(key))

    def test_clear_before_draw_and_operation_results_are_ordered(self):
        self.run_capture("clear", frame="1")
        events = self.records()
        self.assertEqual(events[0]["operation_results"], 1)
        self.assertEqual(events[0]["clear_rect_limit"], 256)
        self.assertEqual([e["event"] for e in events[:6]],
                         ["header", "frame_begin", "clear", "clear_result", "draw", "draw_result"])
        self.assertEqual([e["op_id"] for e in events if e["event"] in ("clear", "draw")], list(range(1, 9)))
        self.assertEqual(events[2]["rects_status"], "none")
        self.assertEqual(events[2]["rects"], [])
        self.assertEqual((events[2]["flags"], events[2]["color"], events[2]["depth"], events[2]["stencil"]),
                         (7, 0x12345678, 0.75, 9))
        self.assertEqual(events[2]["depth_bits"], 0x3f400000)
        self.assertEqual(events[3]["op_id"], events[2]["op_id"])
        self.assertEqual(events[5]["op_id"], events[4]["op_id"])
        clears = [e for e in events if e["event"] == "clear"]
        results = [e for e in events if e["event"] == "clear_result"]
        self.assertEqual(clears[1]["rects"], [{"x1": -5, "y1": 2, "x2": 31, "y2": 42},
                                              {"x1": 50, "y1": 60, "x2": 70, "y2": 80}])
        self.assertEqual(results[1]["hr"], -1)
        self.assertEqual([e["rects_status"] for e in clears[2:]],
                         ["null_pointer", "inconsistent", "unreadable", "count_limit", "address_overflow"])
        self.assertTrue(all(e["rects"] is None for e in clears[2:]))
        self.assertFalse(events[-2]["concurrent_operations"])
        self.assertEqual(trace_summary.summarize(self.path)["draw_count"], 1)

    def test_clear_partial_copy_is_unknown(self):
        self.run_capture("clear", frame="1", extra={"T3_TEST_PARTIAL_READ": "success"})
        clears = [e for e in self.records() if e["event"] == "clear"]
        self.assertEqual(clears[1]["rects_status"], "unreadable")
        self.assertIsNone(clears[1]["rects"])

    def test_clear_only_frame_is_valid(self):
        self.run_capture("clear_only", frame="1")
        self.assertEqual([e["event"] for e in self.records()],
                         ["header", "frame_begin", "clear", "clear_result", "frame_end", "capture_end"])
        summary = trace_summary.summarize(self.path)
        self.assertEqual(summary["draw_count"], 0)
        self.assertEqual(summary["events"]["clear"], 1)

    def test_clear_nonfinite_depth_preserves_bits_without_invalid_json(self):
        self.run_capture("clear_nan", frame="1")
        clear = [e for e in self.records() if e["event"] == "clear"][1]
        self.assertIsNone(clear["depth"])
        self.assertEqual(clear["depth_bits"] & 0x7f800000, 0x7f800000)
        self.assertNotEqual(clear["depth_bits"] & 0x007fffff, 0)
        trace_summary.summarize(self.path)

    def test_pending_clear_or_draw_leaves_incomplete_trace(self):
        for scenario in ("clear_pending", "draw_pending"):
            with self.subTest(scenario=scenario):
                if self.path.exists(): self.path.unlink()
                self.run_capture(scenario, frame="1")
                events = self.records()
                self.assertEqual(events[-1]["event"], "capture_incomplete")
                self.assertEqual(events[-1]["reason"], "pending_operations")
                self.assertFalse(any(e["event"] in ("frame_end", "capture_end") for e in events))
                with self.assertRaisesRegex(trace_summary.TraceError, "no frame_end"):
                    trace_summary.summarize(self.path)

    def test_overlapping_operations_are_flagged(self):
        self.run_capture("overlap", frame="1")
        events = self.records()
        self.assertEqual([e["event"] for e in events[2:-2]],
                         ["clear", "clear_result", "clear", "draw", "clear_result", "draw_result"])
        self.assertTrue(events[-2]["concurrent_operations"])
        self.assertEqual(trace_summary.summarize(self.path)["draw_count"], 1)

    def test_f11_arms_the_next_frame(self):
        output = self.run_capture("key", key="F11")
        self.assertIn("configured=1", output)
        summary = trace_summary.summarize(self.path)
        self.assertEqual(summary["frames"][0]["frame"], 2)
        self.assertEqual(summary["calls"], {"DrawIndexedPrimitive": 1})
        self.assertEqual(self.records()[0]["trigger"], "F11")

    def test_up_geometry_snapshots_and_failure_states(self):
        self.run_capture("up", frame="1")
        events = self.records()
        draws = [event for event in events if event["event"] == "draw"]
        self.assertEqual(len(draws), 26)
        self.assertEqual(events[0]["up_capture"]["component_limit_bytes"], 256 * 1024)
        expected_counts = [2, 4, 3, 6, 4, 4]
        for draw, count in zip(draws[:6], expected_counts):
            vertex = draw["up_data"]["vertex"]
            self.assertEqual(vertex["status"], "captured")
            self.assertEqual(vertex["first_vertex"], 0)
            self.assertEqual(vertex["vertex_count"], count)
            self.assertEqual(vertex["byte_offset"], 0)
            self.assertEqual(bytes.fromhex(vertex["data"]), bytes(range(count * 4)))
            self.assertIsNone(draw["up_data"]["index"])
        v16 = draws[6]["up_data"]["vertex"]
        self.assertEqual((v16["first_vertex"], v16["vertex_count"], v16["byte_offset"]), (2, 3, 8))
        self.assertEqual(bytes.fromhex(v16["data"]), bytes(range(8, 20)))
        i16 = draws[6]["up_data"]["index"]
        self.assertEqual((i16["index_count"], i16["index_format"]), (3, 101))
        self.assertEqual(bytes.fromhex(i16["data"]), b"\x02\x00\x03\x00\x04\x00")
        v32 = draws[7]["up_data"]["vertex"]
        self.assertEqual((v32["first_vertex"], v32["vertex_count"], v32["byte_offset"]), (3, 3, 12))
        self.assertEqual(bytes.fromhex(v32["data"]), bytes(range(12, 24)))
        i32 = draws[7]["up_data"]["index"]
        self.assertEqual((i32["index_count"], i32["index_format"]), (3, 102))
        self.assertEqual(bytes.fromhex(i32["data"]), b"\x03\x00\x00\x00\x04\x00\x00\x00\x05\x00\x00\x00")
        self.assertEqual([d["up_data"]["vertex"]["status"] for d in draws[8:17]], [
            "null_pointer", "unreadable", "unreadable", "unreadable", "invalid_stride",
            "unsupported_topology", "overflow", "overflow", "per_draw_limit",
        ])
        for draw in draws[8:17]:
            self.assertIsNone(draw["up_data"]["vertex"]["data"])
        self.assertEqual(draws[17]["up_data"]["index"]["status"], "unsupported_index_format")
        self.assertEqual(draws[18]["up_data"]["vertex"]["status"], "empty")
        self.assertEqual(draws[18]["up_data"]["index"]["status"], "empty")
        self.assertEqual(draws[18]["up_data"]["vertex"]["data"], "")
        for draw, count in zip(draws[19:25], expected_counts):
            index = draw["up_data"]["index"]
            self.assertEqual(index["status"], "captured")
            self.assertEqual(index["index_count"], count)
            self.assertEqual(index["byte_count"], count * 2)
            self.assertEqual(len(bytes.fromhex(index["data"])), count * 2)
        self.assertEqual(bytes.fromhex(draws[25]["up_data"]["vertex"]["data"]),
                         bytes([254, 255, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9]))
        self.assertFalse(events[-2]["replayable"])

    def test_up_partial_kernel_copy_is_never_emitted(self):
        self.run_capture("up", frame="1", extra={"T3_TEST_PARTIAL_READ": "1"})
        draws = [event for event in self.records() if event["event"] == "draw"]
        for draw in draws[:8]:
            self.assertEqual(draw["up_data"]["vertex"]["status"], "unreadable")
            self.assertIsNone(draw["up_data"]["vertex"]["data"])
        for draw in draws[6:8]:
            self.assertEqual(draw["up_data"]["index"]["status"], "unreadable")
            self.assertIsNone(draw["up_data"]["index"]["data"])

    def test_up_short_successful_kernel_copy_is_also_rejected(self):
        self.run_capture("up", frame="1", extra={"T3_TEST_PARTIAL_READ": "success"})
        draws = [event for event in self.records() if event["event"] == "draw"]
        self.assertTrue(all(d["up_data"]["vertex"]["status"] == "unreadable" for d in draws[:8]))
        self.assertTrue(all(d["up_data"]["vertex"]["data"] is None for d in draws[:8]))

    def test_up_frame_budget_counts_only_captured_raw_bytes(self):
        self.run_capture("up_budget", frame="1")
        draws = [event for event in self.records() if event["event"] == "draw"]
        self.assertEqual(len(draws), 33)
        self.assertTrue(all(d["up_data"]["vertex"]["status"] == "captured" for d in draws[:32]))
        self.assertEqual(draws[32]["up_data"]["vertex"]["status"], "frame_limit")
        self.assertIsNone(draws[32]["up_data"]["vertex"]["data"])

    def test_failed_up_reads_do_not_consume_frame_budget(self):
        self.run_capture("up_budget", frame="1", extra={"T3_TEST_PARTIAL_READ": "1"})
        draws = [event for event in self.records() if event["event"] == "draw"]
        self.assertEqual(len(draws), 33)
        self.assertTrue(all(d["up_data"]["vertex"]["status"] == "unreadable" for d in draws))
        self.assertTrue(all(d["up_data"]["vertex"]["data"] is None for d in draws))

    def test_up_frame_budget_includes_indices(self):
        self.run_capture("up_mixed_budget", frame="1")
        draws = [event for event in self.records() if event["event"] == "draw"]
        self.assertEqual(len(draws), 33)
        self.assertEqual(draws[31]["up_data"]["vertex"]["byte_count"], 262136)
        self.assertEqual(draws[31]["up_data"]["index"]["status"], "captured")
        self.assertEqual(draws[32]["up_data"]["vertex"]["status"], "empty")
        self.assertEqual(draws[32]["up_data"]["index"]["status"], "frame_limit")
        self.assertIsNone(draws[32]["up_data"]["index"]["data"])

    def test_release_before_capture_restarts_with_second_device(self):
        self.run_capture("release_before", frame="1")
        summary = trace_summary.summarize(self.path)
        self.assertEqual(summary["frames"][0]["frame"], 1)
        self.assertEqual(summary["calls"], {"DrawIndexedPrimitive": 1})
        self.assertEqual(self.records()[1]["presentation"]["width"], 1600)

    def test_release_clears_armed_f11_for_second_device(self):
        self.run_capture("release_key", key="F11")
        summary = trace_summary.summarize(self.path)
        self.assertEqual(summary["frames"][0]["frame"], 2)
        self.assertEqual(summary["calls"], {"DrawIndexedPrimitive": 1})
        self.assertEqual(self.records()[1]["presentation"]["width"], 1600)

    def test_held_f11_does_not_retrigger_after_device_release(self):
        self.run_capture("release_key_held", key="F11")
        summary = trace_summary.summarize(self.path)
        self.assertEqual(summary["frames"][0]["frame"], 4)
        self.assertEqual(summary["calls"], {"DrawIndexedPrimitive": 1})

    def test_release_during_frame_leaves_detectably_incomplete_trace(self):
        self.run_capture("release_open", frame="1")
        self.assertEqual([event["event"] for event in self.records()], ["header", "frame_begin", "draw", "draw_result"])
        with self.assertRaisesRegex(trace_summary.TraceError, "frame 1 has no frame_end"):
            trace_summary.summarize(self.path)

    def test_short_writes_are_completed(self):
        self.run_capture(frame="1", extra={"T3_TEST_MAX_WRITE": "5"})
        self.assertEqual(trace_summary.summarize(self.path)["draw_count"], 4)

    def test_successful_reset_before_capture_updates_presentation(self):
        self.run_capture("reset_before", frame="1")
        self.assertEqual(self.records()[1]["presentation"]["width"], 1600)
        self.assertEqual(trace_summary.summarize(self.path)["draw_count"], 4)

    def test_failed_reset_before_capture_preserves_presentation(self):
        self.run_capture("reset_before_failure", frame="1")
        self.assertEqual(self.records()[1]["presentation"]["width"], 1280)
        self.assertEqual(trace_summary.summarize(self.path)["draw_count"], 4)

    def test_write_failure_stops_with_detectably_incomplete_trace(self):
        self.run_capture(frame="1", extra={"T3_TEST_WRITE_FAIL_AFTER": "1"})
        with self.assertRaisesRegex(trace_summary.TraceError, "missing capture_end"):
            trace_summary.summarize(self.path)

    def test_existing_trace_is_not_overwritten(self):
        self.path.write_text("preserve me", encoding="utf-8")
        self.run_capture(frame="1")
        self.assertEqual(self.path.read_text(encoding="utf-8"), "preserve me")

    def test_invalid_or_disabled_configuration_stays_passive(self):
        cases = [
            {"frame": None}, {"frame": "-1"}, {"frame": "+1"}, {"frame": "abc"},
            {"frame": "18446744073709551615"}, {"key": "F10"}, {"frame": "1", "key": "F11"},
            {"frame": "1", "path": False},
        ]
        for case in cases:
            with self.subTest(case=case):
                output = self.run_capture(**case)
                self.assertIn("configured=0 getters=0", output)
                self.assertFalse(self.path.exists())


if __name__ == "__main__":
    unittest.main()
