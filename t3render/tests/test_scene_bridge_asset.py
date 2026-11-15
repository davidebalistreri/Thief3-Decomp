import shutil
import subprocess
import tempfile
from pathlib import Path
import unittest


class SceneBridgeAssetTests(unittest.TestCase):
    def test_bounded_read_only_candidate(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++ compiler unavailable")
        source = Path(__file__).with_name("scene_bridge_asset_test.cpp")
        with tempfile.TemporaryDirectory(prefix="t3render-asset-test-") as directory:
            binary = Path(directory) / "test"
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
