"""Run isolated D3D8 resource-wrapper lifecycle and forwarding tests on the host."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BufferWrappersTest(unittest.TestCase):
    def test_resource_wrappers(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("a host C++ compiler is required")
        with tempfile.TemporaryDirectory(prefix="t3render-wrapper-test-") as temporary:
            binary = Path(temporary) / "buffer_wrappers_test"
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                       "-I", str(ROOT / "tests" / "wrapper_fakes"),
                       str(ROOT / "ir" / "buffer_shadow.cpp"), str(ROOT / "ir" / "buffer_registry.cpp"),
                       str(ROOT / "tests" / "buffer_wrappers_test.cpp"), "-o", str(binary)]
            built = subprocess.run(command, text=True, capture_output=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
