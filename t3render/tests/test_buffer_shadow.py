"""Host behavior tests and optional x86 Windows cross-compilation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class BufferShadowTests(unittest.TestCase):
    def test_host_and_optional_win32_build(self):
        cxx = shutil.which("clang++") or shutil.which("g++")
        if not cxx:
            self.skipTest("host C++ compiler required")
        root = Path(__file__).resolve().parent.parent
        sources = [str(root / "ir" / "buffer_shadow.cpp"), str(root / "tests" / "buffer_shadow_test.cpp")]
        with tempfile.TemporaryDirectory(prefix="t3render-buffer-shadow-") as tmp:
            executable = Path(tmp) / ("shadow.exe" if os.name == "nt" else "shadow")
            flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror"]
            subprocess.run([cxx, *flags, *sources, "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
            mingw = shutil.which("i686-w64-mingw32-g++")
            if mingw:
                subprocess.run([mingw, *flags, "-static", "-static-libgcc", "-static-libstdc++", *sources,
                                "-o", str(Path(tmp) / "shadow-win32.exe")], check=True)
