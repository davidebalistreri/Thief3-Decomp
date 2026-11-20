import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class LightDiagnosticsTests(unittest.TestCase):
    def test_valid_bounded_json_keeps_identity_and_evidence(self):
        source = Path(__file__).resolve().parent / "scene_live_light_diagnostics_fixture.cpp"
        with tempfile.TemporaryDirectory(prefix="t3scene-light-diagnostics-") as directory:
            output = Path(directory) / "test"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(source), "-o", str(output)], check=True)
            records = [json.loads(line) for line in subprocess.check_output([str(output)], text=True).splitlines()]
        record, raw, no_link, unknown, stale, camera_mismatch, identity = records
        self.assertTrue(record["light_details_truncated"])
        self.assertEqual(len(record["light_details"]), 32)
        self.assertEqual(record["light_details"][0]["id"], 'light:"\\\nè')
        self.assertEqual(record["light_details"][0]["position"], [1, 2, 3])
        self.assertEqual(record["light_details"][0]["position_evidence"], "candidate")
        self.assertIsNone(record["light_details"][1]["position"])
        self.assertEqual(record["light_details"][1]["position_evidence"], "unknown")
        self.assertTrue(raw["light_raw_available"])
        self.assertEqual(raw["light_raw"]["candle_link"], {"actor_number": 6, "status": "candidate", "field_8c_bits": 0x3F000000,
                         "enabled_candidate": True, "enabled_inherited": True,
                         "transform_bits": [0x3F800000 if n % 5 == 0 else 0 for n in range(16)]})
        self.assertEqual(raw["light_raw"]["other_candle_link"], {"actor_number": 14, "status": "no_link", "field_8c_bits": None,
                         "enabled_candidate": False, "enabled_inherited": False,
                         "transform_bits": [0x3F800000 if n % 5 == 0 else 0 for n in range(16)]})
        self.assertEqual(no_link["light_raw"]["candle_link"], {"actor_number": 6, "status": "no_link", "field_8c_bits": None,
                         "enabled_candidate": False, "enabled_inherited": False,
                         "transform_bits": [0x3F800000 if n % 5 == 0 else 0 for n in range(16)]})
        self.assertEqual(unknown["light_raw"]["candle_link"], {"actor_number": 6, "status": "unknown", "field_8c_bits": None,
                                                                  "enabled_candidate": None, "enabled_inherited": None,
                                                                  "transform_bits": None})
        item = raw["light_raw"]["items"][0]
        self.assertEqual(item["field_34_bits"], [0x3F800000, 0x40000000, 0x40400000])
        self.assertEqual(item["field_64"], 5)
        self.assertEqual(item["field_68_bits"][0], 0x3F000000)
        self.assertNotIn("field_80_bits", item)
        self.assertNotIn("field_84_bits", item)
        self.assertNotIn(str(0xDEADBEEF), json.dumps(raw))
        self.assertNotIn(str(0xCAFEBABE), json.dumps(raw))
        self.assertFalse(stale["light_raw_available"])
        self.assertFalse(camera_mismatch["light_raw_available"])
        self.assertEqual((identity["source"], identity["session_id"], identity["frame"]),
                         ("captured_replay", 'test"session', 3))
        self.assertEqual(identity["capture_sha256"], "a" * 64)
        self.assertEqual((identity["map"], identity["map_sha256"]), ("bedroom", "a" * 64))


if __name__ == "__main__":
    unittest.main()
