"""Texture type observations are bounded to validated successful draws."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import trace_texture_inventory as inventory


class TextureInventoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-texture-inventory-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "trace.jsonl"

    def write(self, stages_and_results):
        events = [{"event": "header", "format": "t3render-d3d8-diagnostic", "version": 1,
                   "operation_results": 1, "clear_rect_limit": 256},
                  {"event": "frame_begin", "frame": 7}]
        for op, (stages, hr) in enumerate(stages_and_results, 1):
            events.extend(({"event": "draw", "frame": 7, "op_id": op, "call": "DrawIndexedPrimitive",
                            "stages": stages},
                           {"event": "draw_result", "frame": 7, "op_id": op, "hr": hr}))
        events.extend(({"event": "frame_end", "frame": 7, "present_hr": 0, "concurrent_operations": False},
                       {"event": "capture_end", "frames": 1}))
        self.path.write_text("\n".join(map(json.dumps, events)) + "\n", encoding="utf-8")

    def test_counts_successful_types_and_process_local_addresses(self):
        tex = {"id": "0x123", "type": 3}
        cube = {"id": "0xabc", "type": 5}
        self.write([
            ([{"stage": 0, "texture": tex}, {"stage": 1, "texture": cube},
              {"stage": 2, "texture": cube}, {"stage": 3, "texture": None}], 0),
            ([{"stage": 0, "texture": tex}, {"stage": 1, "texture_hr": -1, "texture": cube},
              {"stage": 2, "texture": {"type": 3, "id": "0x0"}}], 0),
            ([{"stage": 0, "texture": {"id": "0xff", "type": 4}}], -1),
            ([{"stage": 0, "texture": tex}, {"stage": 0, "texture": cube}], 0),
            (None, 0),
        ])
        result = inventory.inventory(self.path)
        self.assertEqual(result["source_sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())
        frame = result["frames"][0]
        self.assertEqual(frame["frame_outcome"], {"present_succeeded": True, "concurrent_operations": False,
                                                  "had_reset": None})
        self.assertEqual(frame["draw_outcomes"], {"failed": 1, "succeeded": 4})
        self.assertEqual(frame["draws_with_missing_or_invalid_stages"], 2)
        self.assertEqual(frame["stage_observations"], {"bound": 4, "getter_failed_or_invalid": 1,
                                                       "unbound": 1, "unknown": 1})
        self.assertEqual(frame["texture_types"]["texture_2d"], {"bound_stage_observations": 2,
                         "draws_with_type": 2, "distinct_observed_addresses": 1, "by_stage": {"0": 2}})
        self.assertEqual(frame["texture_types"]["cube_texture"], {"bound_stage_observations": 2,
                         "draws_with_type": 1, "distinct_observed_addresses": 1, "by_stage": {"1": 1, "2": 1}})
        self.assertNotIn("volume_texture", frame["texture_types"])

    def test_cli_requires_complete_trace_and_never_overwrites_output(self):
        self.write([([{"stage": 0, "texture": {"id": "0x1", "type": 5}}], 0)])
        out = Path(self.temp.name) / "inventory.json"
        command = [sys.executable, "-B", str(ROOT / "tools/trace_texture_inventory.py"), str(self.path), "-o", str(out)]
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
        first = out.read_bytes()
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)
        self.assertEqual(out.read_bytes(), first)
        self.path.write_text(self.path.read_text().rsplit("\n", 2)[0] + "\n", encoding="utf-8")
        out.unlink()
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)
        self.assertFalse(out.exists())

    def test_missing_draw_result_is_unknown(self):
        events = [{"event": "header", "format": "t3render-d3d8-diagnostic", "version": 1},
                  {"event": "frame_begin", "frame": 0},
                  {"event": "draw", "frame": 0, "call": "DrawPrimitive",
                   "stages": [{"stage": 0, "texture": {"id": "0x1", "type": 5}}]},
                  {"event": "frame_end", "frame": 0, "present_hr": 0},
                  {"event": "capture_end", "frames": 1}]
        self.path.write_text("\n".join(map(json.dumps, events)) + "\n", encoding="utf-8")
        frame = inventory.inventory(self.path)["frames"][0]
        self.assertEqual(frame["draw_outcomes"], {"unknown": 1})
        self.assertEqual(frame["texture_types"], {})


if __name__ == "__main__":
    unittest.main()
