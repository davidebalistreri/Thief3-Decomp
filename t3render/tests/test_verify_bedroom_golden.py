import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import verify_bedroom_golden as golden


class GoldenFrameTest(unittest.TestCase):
    def setUp(self):
        camera = {"view": [1] * 16, "projection": [2] * 16}
        viewport = {"width": 1024, "height": 768}
        self.records = [{"event": "frame_image", "frame": golden.FRAME, "status": "captured",
                         "stage": "complete", "hr": 0, "width": 1024, "height": 768},
                        {"event": "frame_end", "frame": golden.FRAME, "present_hr": 0,
                         "had_reset": False, "concurrent_operations": False}]
        for op in golden.MATCHED_DRAWS:
            self.records += [{"event": "draw", "frame": golden.FRAME, "op_id": op,
                              "transforms": copy.deepcopy(camera), "viewport": copy.deepcopy(viewport),
                              "buffer_data": {"source": "application_lock_shadow",
                                              "vertex": {"status": "captured"}, "index": {"status": "captured"}}},
                             {"event": "draw_result", "frame": golden.FRAME, "op_id": op,
                              "hr": 0, "buffer_stable": True}]

    def test_preserves_synchronized_camera_and_draws(self):
        result = golden.frame_evidence(self.records)
        self.assertEqual(result["frame"], golden.FRAME)
        self.assertEqual(result["correlated_draws"], list(golden.MATCHED_DRAWS))

    def test_rejects_mismatched_camera_or_missing_shadow(self):
        for field in ("camera", "buffer", "present"):
            records = copy.deepcopy(self.records)
            if field == "camera": records[2]["transforms"]["view"][0] = 42
            if field == "buffer": records[2]["buffer_data"]["index"]["status"] = "unknown"
            if field == "present": records[1]["present_hr"] = -1
            with self.subTest(field=field), self.assertRaises(ValueError):
                golden.frame_evidence(records)


if __name__ == "__main__":
    unittest.main()
