"""Resolve only scoped, independently sampled semantic candidates."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class SceneLiveBindingTests(unittest.TestCase):
    def test_binding_to_resident_window_rejects_ambiguous_metadata(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("host C++ compiler required")
        with tempfile.TemporaryDirectory(prefix="t3render-scene-binding-") as directory:
            binary = Path(directory) / "fixture"
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                            str(ROOT / "tests/scene_live_binding_fixture.cpp"), str(ROOT / "ir/scene_live.cpp"),
                            str(ROOT / "ir/scene_live_window.cpp"), str(ROOT / "src/scene_live_binding.cpp"),
                            "-o", str(binary)], check=True, capture_output=True, timeout=60)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
            self.assertIn("scene_live_binding_fixture=ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
