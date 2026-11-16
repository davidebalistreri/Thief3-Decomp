import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

from test_analyze_scene_bridge import fixture

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
import compare_live_present


class LivePresentComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-live-present-")
        self.addCleanup(self.temp.cleanup)
        self.trace = Path(self.temp.name) / "trace.jsonl"
        self.viewer = Path(self.temp.name) / "viewer.jsonl"
        self.session = "live:" + "a" * 32
        self.events = fixture()
        self.events[0]["live_session_id"] = self.session
        self.events[2]["pass"] = 0
        self.events[-2]["scene_live_frame"] = 88
        matrix = self.events[3]["transforms"]["view"]
        self.records = [{"event": "viewer_started", "session_id": self.session},
                        {"event": "frame", "source": "live_process", "frame": 88,
                         "render_pass": 0, "stage": 1, "view": matrix, "projection": matrix,
                         "viewport": {"x": 0, "y": 0, "width": 640, "height": 480,
                                      "min_depth": 0, "max_depth": 1}}]

    def compare(self):
        self.trace.write_text("".join(json.dumps(row) + "\n" for row in self.events))
        self.viewer.write_text("".join(json.dumps(row) + "\n" for row in self.records))
        return compare_live_present.compare(self.trace, self.viewer)

    def test_same_present_link_and_camera(self):
        result = self.compare()
        self.assertEqual(result["status"], "matched_present_and_camera")
        self.assertEqual(result["matches"][0]["trace_frame"], 3)
        self.assertEqual(result["matches"][0]["live_frame"], 88)
        self.assertNotIn("StaticMeshActor", json.dumps(result))

    def test_equal_frame_number_without_link_is_not_evidence(self):
        del self.events[-2]["scene_live_frame"]
        self.records[1]["frame"] = 3
        self.assertEqual(self.compare()["status"], "no_verified_match")

    def test_session_mismatch_is_rejected(self):
        self.records[0]["session_id"] = "live:" + "b" * 32
        with self.assertRaisesRegex(ValueError, "session identities"):
            self.compare()

    def test_camera_pass_and_synchronization_are_required(self):
        for variant in ("camera", "viewport", "failed_draw", "pass", "stage", "boolean_pass", "unsynchronized", "source"):
            with self.subTest(variant=variant):
                original_events, original_records = copy.deepcopy(self.events), copy.deepcopy(self.records)
                if variant == "camera":
                    self.records[1]["view"][0] = 0.5
                elif variant == "viewport":
                    self.records[1]["viewport"]["max_depth"] = 0.5
                elif variant == "failed_draw":
                    self.events[4]["hr"] = -1
                elif variant == "pass":
                    self.records[1]["render_pass"] = 1
                elif variant == "stage":
                    self.records[1]["stage"] = 0
                elif variant == "boolean_pass":
                    self.records[1]["render_pass"] = False
                elif variant == "source":
                    self.records[1]["source"] = "synthetic"
                else:
                    self.events[5]["status"] = "unknown"
                self.assertEqual(self.compare()["status"], "no_verified_match")
                self.events, self.records = original_events, original_records

    def test_duplicate_viewer_frames_are_ambiguous(self):
        self.records.append(copy.deepcopy(self.records[1]))
        with self.assertRaisesRegex(ValueError, "duplicate viewer frame"):
            self.compare()


if __name__ == "__main__":
    unittest.main()
