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
            record = json.loads(subprocess.check_output([str(output)], text=True))
        self.assertTrue(record["light_details_truncated"])
        self.assertEqual(len(record["light_details"]), 32)
        self.assertEqual(record["light_details"][0]["id"], 'light:"\\\nè')
        self.assertEqual(record["light_details"][0]["position"], [1, 2, 3])
        self.assertEqual(record["light_details"][0]["position_evidence"], "candidate")
        self.assertIsNone(record["light_details"][1]["position"])
        self.assertEqual(record["light_details"][1]["position_evidence"], "unknown")


if __name__ == "__main__":
    unittest.main()
