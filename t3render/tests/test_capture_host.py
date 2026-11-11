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
                         ["header", "frame_begin", "draw", "draw", "draw", "draw", "reset", "frame_end", "capture_end"])
        self.assertFalse(events[0]["replayable"])
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

    def test_f11_arms_the_next_frame(self):
        output = self.run_capture("key", key="F11")
        self.assertIn("configured=1", output)
        summary = trace_summary.summarize(self.path)
        self.assertEqual(summary["frames"][0]["frame"], 2)
        self.assertEqual(summary["calls"], {"DrawIndexedPrimitive": 1})
        self.assertEqual(self.records()[0]["trigger"], "F11")

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
        self.assertEqual([event["event"] for event in self.records()], ["header", "frame_begin", "draw"])
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
