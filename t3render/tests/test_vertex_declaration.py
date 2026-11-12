"""Portable D3D8 declaration framing and lifecycle observations."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [ROOT / "ir" / "vertex_declaration.cpp", ROOT / "ir" / "vertex_declaration_registry.cpp"]


class VertexDeclarationTests(unittest.TestCase):
    def test_host_lifecycle_and_bounded_reads(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("host C++ compiler required")
        with tempfile.TemporaryDirectory(prefix="t3render-declaration-") as directory:
            binary = Path(directory) / "declaration-test"
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                       *(str(path) for path in SOURCES), str(ROOT / "tests" / "vertex_declaration_test.cpp"),
                       "-o", str(binary)]
            compiled = subprocess.run(command, text=True, capture_output=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_i686_cross_compile(self):
        compiler = shutil.which("i686-w64-mingw32-g++")
        if not compiler:
            self.skipTest("i686 MinGW-w64 compiler required")
        with tempfile.TemporaryDirectory(prefix="t3render-declaration-x86-") as directory:
            for source in SOURCES:
                with self.subTest(source=source.name):
                    result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                             "-c", str(source), "-o", str(Path(directory) / (source.stem + ".o"))],
                                            text=True, capture_output=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
