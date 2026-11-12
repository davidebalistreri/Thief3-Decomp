"""Exercise fixture readback failures without a graphics runtime."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class FixtureReadbackTests(unittest.TestCase):
    def test_lock_compare_and_unlock(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("a host C++ compiler is required")
        source = Path(__file__).with_name("buffer_readback_test.cpp")
        with tempfile.TemporaryDirectory(prefix="t3render-readback-test-") as temporary:
            binary = Path(temporary) / "test"
            compile_result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                             str(source), "-o", str(binary)],
                                            text=True, capture_output=True, timeout=60)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout + compile_result.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
