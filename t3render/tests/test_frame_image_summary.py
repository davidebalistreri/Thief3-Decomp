import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import analyze_tds_capture
import trace_summary


class FrameImageSummaryTests(unittest.TestCase):
    def test_requires_complete_file_and_matching_frame_before_present(self):
        with tempfile.TemporaryDirectory(prefix="t3render-image-test-") as directory:
            trace = Path(directory) / "trace.jsonl"
            records = [
                {"event": "header", "format": "t3render-d3d8-diagnostic", "version": 1,
                 "requested_frames": 1, "operation_results": 1, "clear_rect_limit": 256,
                 "frame_images": {"requested": True, "timing": "before_present"}},
                {"event": "frame_begin", "frame": 7},
                {"event": "clear", "frame": 7, "op_id": 1, "rect_count": 0, "rects_status": "none",
                 "rects": [], "flags": 1, "color": 0, "depth": 1, "depth_bits": 1065353216, "stencil": 0},
                {"event": "clear_result", "frame": 7, "op_id": 1, "hr": 0},
                {"event": "frame_image", "frame": 7, "status": "captured", "stage": "complete", "hr": 0,
                 "width": 2, "height": 1, "format": 21, "present_source_rect": False,
                 "present_dest_rect": False},
                {"event": "frame_end", "frame": 7, "present_hr": 0, "draws": 0,
                 "had_reset": False, "concurrent_operations": False},
                {"event": "capture_end", "frames": 1}]
            def write():
                trace.write_text("".join(json.dumps(record) + "\n" for record in records))
            write()
            image = Path(str(trace) + ".frame-7.ppm")
            image.write_bytes(b"P6\n2 1\n255\n\xff\x00\x00\x00\xff\x00")
            result = analyze_tds_capture.analyze(trace)["frames"][0]["frame_image"]
            self.assertEqual(result["status"], "captured")
            self.assertEqual(result["file"], image.name)
            self.assertEqual(len(result["sha256"]), 64)
            image.write_bytes(b"P6\n2 1\n255\n\xff")
            self.assertEqual(analyze_tds_capture.analyze(trace)["frames"][0]["frame_image"]["status"],
                             "file_missing_or_invalid")
            records[4]["frame"] = 8
            write()
            with self.assertRaises(trace_summary.TraceError):
                trace_summary.summarize(trace)
            records[4]["frame"] = 7
            records.insert(5, {"event": "clear", "frame": 7})
            write()
            with self.assertRaises(trace_summary.TraceError):
                trace_summary.summarize(trace)
