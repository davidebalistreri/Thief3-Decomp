"""Check private-data provenance, reentrancy and concurrent refusal."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TESTS = Path(__file__).resolve().parent


class PrivateBlobTests(unittest.TestCase):
    def test_provenance_and_overlap(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("a host C++ compiler is required")
        with tempfile.TemporaryDirectory(prefix="t3render-private-blob-") as directory:
            binary = Path(directory) / "private-blob-test"
            build = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                                    "-I", str(TESTS / "buffer_proxy_fakes"), str(TESTS / "private_blob_test.cpp"),
                                    "-o", str(binary)], text=True, capture_output=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
