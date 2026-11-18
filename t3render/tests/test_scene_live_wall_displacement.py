from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class WallDisplacementTests(unittest.TestCase):
    def test_geometry_cache_and_fallback(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("host C++ compiler required")
        with tempfile.TemporaryDirectory(prefix="t3render-wall-") as directory:
            binary = Path(directory) / "fixture"
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "tests/scene_live_wall_displacement_test.cpp"),
                            str(ROOT / "src/scene_live_wall_displacement.cpp"),
                            str(ROOT / "src/scene_live_materials.cpp"), "-o", str(binary)],
                           check=True, capture_output=True, timeout=60)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
            self.assertIn("wall_displacement=ok", result.stdout)
