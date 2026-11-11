"""Host-side COM mock for proxy forwarding, not a Windows ABI/runtime test."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class ProxyTest(unittest.TestCase):
    def test_forwarding_and_lifecycle(self):
        cc = shutil.which("clang") or shutil.which("gcc")
        cxx = shutil.which("clang++") or shutil.which("g++")
        if not cc or not cxx:
            self.skipTest("host C and C++ compilers are required")

        root = Path(__file__).resolve().parents[1]
        fake = root / "tests" / "proxy_fakes"
        with tempfile.TemporaryDirectory(prefix="t3render-proxy-") as directory:
            obj = Path(directory) / "layout.o"
            binary = Path(directory) / ("proxy-test.exe" if os.name == "nt" else "proxy-test")
            include = ["-I", str(fake), "-I", str(root / "src")]
            subprocess.run([cc, "-std=c11", *include, "-c", str(root / "src" / "vtable_layout.c"),
                            "-o", str(obj)], check=True)
            subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-pthread", *include,
                            str(root / "src" / "proxy.cpp"), str(root / "tests" / "proxy_mock.cpp"),
                            str(obj), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
