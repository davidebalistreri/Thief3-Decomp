"""Verify reports use deltas and disclose incomplete or invalid measurements."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOL = Path(__file__).resolve().parent.parent / "tools/report_live_performance.py"
spec = importlib.util.spec_from_file_location("report_live_performance", TOOL)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

BOUNDS = [1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 25, 33, 50, 75, 100, 250, 500, 1000]


def snapshot(elapsed, successful, intervals, total, histogram, **changes):
    timing = {"clock_hz": 1000, "successful": successful, "failed": 0, "overlaps": 0,
              "resets": 0, "interruptions": 0, "clock_errors": 0,
              "intervals": intervals, "total_ticks": total, "max_ticks": 0,
              "bounds_ms": BOUNDS, "histogram": histogram}
    timing.update(changes)
    return {"event": "driver", "elapsed_ms": elapsed, "present_timing": timing}


class LivePerformanceReportTests(unittest.TestCase):
    def events(self):
        zero = [0] * 20
        histogram = zero.copy()
        histogram[8], histogram[10], histogram[12] = 1, 2, 1
        return [{"event": "viewer_started", "session_id": "live:test"},
                snapshot(1000, 1, 0, 0, zero),
                snapshot(2000, 5, 4, 86, histogram, max_ticks=30)]

    def test_report_computes_present_cadence_and_histogram_bounds(self):
        result = module.report(self.events())
        self.assertAlmostEqual(result["fps"], 4000 / 86)
        self.assertEqual(result["mean_frametime_ms"], 21.5)
        self.assertEqual(result["median_frametime_ms_bucket"], [18, 20])
        self.assertEqual(result["p95_frametime_ms_bucket"], [25, 33])
        self.assertEqual(result["measured_interval_seconds"], 0.086)
        self.assertEqual(result["telemetry_window_seconds"], 1.0)

    def test_report_rejects_old_or_nonmonotonic_logs(self):
        events = self.events()
        del events[1]["present_timing"]
        with self.assertRaisesRegex(ValueError, "unavailable"):
            module.report(events)
        events = self.events()
        reversed_snapshot = copy.deepcopy(events[-1])
        reversed_snapshot["elapsed_ms"] = 3000
        reversed_snapshot["present_timing"]["successful"] = 1
        events.append(reversed_snapshot)
        with self.assertRaisesRegex(ValueError, "backwards"):
            module.report(events)

    def test_zero_intervals_and_window(self):
        events = self.events()[:2]
        events.append(snapshot(2000, 1, 0, 0, [0] * 20, failed=1, interruptions=1))
        result = module.report(events, 1000, 2000)
        self.assertIsNone(result["fps"])
        self.assertEqual(result["failed"], 1)
        with self.assertRaisesRegex(ValueError, "two driver snapshots"):
            module.report(events, 1500, 2000)

    def test_cli_reads_viewer_log(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "viewer.jsonl"
            log.write_text("\n".join(json.dumps(e) for e in self.events()) + "\n", encoding="utf-8")
            output = subprocess.run([sys.executable, "-B", str(TOOL), str(log), "--start-ms", "1000"],
                                    check=True, capture_output=True, text=True)
            result = json.loads(output.stdout)
            self.assertEqual(result["window_start_ms"], 1000)
            self.assertEqual(result["p95_frametime_ms_bucket"], [25, 33])


if __name__ == "__main__":
    unittest.main()
