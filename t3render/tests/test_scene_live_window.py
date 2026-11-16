"""A synthetic Present interval cannot publish incomplete semantic evidence."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class SceneLiveWindowTests(unittest.TestCase):
    def test_present_windows_reject_ambiguity_and_recover(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("host C++ compiler required")
        with tempfile.TemporaryDirectory(prefix="t3render-scene-window-") as directory:
            binary = Path(directory) / "fixture"
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                            str(ROOT / "tests/scene_live_window_fixture.cpp"), str(ROOT / "ir/scene_live.cpp"),
                            str(ROOT / "ir/scene_live_window.cpp"), "-o", str(binary)],
                           check=True, capture_output=True, timeout=60)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
            self.assertIn("scene_live_window_fixture=ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
