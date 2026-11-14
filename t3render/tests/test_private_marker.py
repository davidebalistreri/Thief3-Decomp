"""Exercise isolated marker lifetime, collisions and concurrent initialization."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TESTS = Path(__file__).resolve().parent


class PrivateMarkerTests(unittest.TestCase):
    def test_marker_contract(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("a host C++ compiler is required")
        with tempfile.TemporaryDirectory(prefix="t3render-private-marker-") as directory:
            binary = Path(directory) / "marker-test"
            built = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                                    "-I", str(TESTS / "buffer_proxy_fakes"), str(TESTS / "private_marker_test.cpp"),
                                    "-o", str(binary)], text=True, capture_output=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
