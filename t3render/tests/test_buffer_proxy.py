"""Exercise COM buffer proxy identity, lifetime and conservative tracking."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BufferProxyTests(unittest.TestCase):
    def test_registry_and_wrappers(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("a host C++ compiler is required")
        with tempfile.TemporaryDirectory(prefix="t3render-buffer-proxy-test-") as directory:
            binary = Path(directory) / "buffer_proxy_test"
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                       "-I", str(ROOT / "tests/buffer_proxy_fakes"),
                       str(ROOT / "ir/buffer_shadow.cpp"), str(ROOT / "ir/buffer_registry.cpp"),
                       str(ROOT / "src/buffer_proxy.cpp"), str(ROOT / "tests/buffer_proxy_test.cpp"),
                       "-o", str(binary)]
            built = subprocess.run(command, text=True, capture_output=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
