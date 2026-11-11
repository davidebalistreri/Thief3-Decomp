"""Host-side checks for DX12 readback row pitch and bounds handling."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class D3D12ReadbackTests(unittest.TestCase):
    def test_pixels(self):
        cxx = shutil.which("clang++") or shutil.which("g++")
        if not cxx:
            self.skipTest("host C++ compiler required")
        source = Path(__file__).resolve().parent / "d3d12_probe_pixels_mock.cpp"
        with tempfile.TemporaryDirectory(prefix="t3render-d3d12-") as tmp:
            binary = Path(tmp) / ("pixels.exe" if os.name == "nt" else "pixels")
            subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
