from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class CandleEnabledTests(unittest.TestCase):
    def test_local_and_inherited_property_reads(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("host C++ compiler required")
        source = Path(__file__).resolve().parent / "scene_bridge_candle_enabled_test.cpp"
        with tempfile.TemporaryDirectory(prefix="t3render-candle-enabled-") as directory:
            output = Path(directory) / "fixture"
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(source), "-o", str(output)], check=True, timeout=60)
            subprocess.run([str(output)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
