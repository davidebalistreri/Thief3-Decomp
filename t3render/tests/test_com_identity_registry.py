"""Exercise the isolated identity table with real host threads."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ComIdentityRegistryTests(unittest.TestCase):
    def test_registry(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("a host C++ compiler is required")
        with tempfile.TemporaryDirectory(prefix="t3render-com-identity-test-") as temporary:
            binary = Path(temporary) / "com_identity_registry_test"
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                       str(ROOT / "tests" / "com_identity_registry_test.cpp"), "-o", str(binary)]
            result = subprocess.run(command, text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
