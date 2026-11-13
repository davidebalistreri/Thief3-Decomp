"""Bounded, fail-closed framing of synthetic VS 1.1 shader programs."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ir" / "shader_program.cpp"


class ShaderProgramTests(unittest.TestCase):
    def test_host_reader_and_token_framing(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("host C++ compiler required")
        with tempfile.TemporaryDirectory(prefix="t3render-shader-program-") as directory:
            binary = Path(directory) / "shader-program-test"
            build = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                    str(SOURCE), str(ROOT / "tests" / "shader_program_test.cpp"),
                                    "-o", str(binary)], text=True, capture_output=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_i686_cross_compile(self):
        compiler = shutil.which("i686-w64-mingw32-g++")
        if not compiler:
            self.skipTest("i686 MinGW-w64 compiler required")
        with tempfile.TemporaryDirectory(prefix="t3render-shader-program-x86-") as directory:
            result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-c",
                                     str(SOURCE), "-o", str(Path(directory) / "shader_program.o")],
                                    text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
