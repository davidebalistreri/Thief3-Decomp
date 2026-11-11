"""Cross-build the standalone probe and enforce the proxy's x86 boundary."""
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def machine(path):
    with path.open("rb") as binary:
        header = binary.read(64)
        if len(header) != 64 or header[:2] != b"MZ":
            raise ValueError(f"not a PE executable: {path}")
        binary.seek(struct.unpack_from("<I", header, 60)[0])
        pe = binary.read(6)
        if len(pe) != 6 or pe[:4] != b"PE\0\0":
            raise ValueError(f"invalid PE signature: {path}")
        return struct.unpack_from("<H", pe, 4)[0]


class D3D12ProbeBuildTests(unittest.TestCase):
    def configure(self, build, prefix, *options):
        for executable in ("cmake", "ninja", prefix + "gcc", prefix + "g++"):
            if shutil.which(executable) is None:
                self.skipTest(f"{executable} is unavailable")
        return subprocess.run([
            "cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
            "-DCMAKE_SYSTEM_NAME=Windows", "-DCMAKE_C_COMPILER=" + prefix + "gcc",
            "-DCMAKE_CXX_COMPILER=" + prefix + "g++", *options,
        ], text=True, capture_output=True, timeout=60)

    def build(self, build):
        result = subprocess.run(["cmake", "--build", str(build)], text=True, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def probe_only(self, prefix, expected_machine):
        with tempfile.TemporaryDirectory(prefix="t3render-probe-build-") as tmp:
            build = Path(tmp) / "build"
            result = self.configure(build, prefix, "-DT3RENDER_D3D12_PROBE_ONLY=ON")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("T3RENDER_HAS_D3D8_H", (build / "CMakeCache.txt").read_text())
            self.build(build)
            self.assertEqual(machine(build / "t3render_d3d12_probe.exe"), expected_machine)
            self.assertFalse((build / "d3d8.dll").exists())
            self.assertFalse((build / "smoke" / "t3render_smoke.exe").exists())
            self.assertFalse(any(build.glob("*t3render_ir*")))
            proxy_target = subprocess.run(["cmake", "--build", str(build), "--target", "t3render"],
                                          text=True, capture_output=True, timeout=60)
            self.assertNotEqual(proxy_target.returncode, 0, "probe-only must not define a proxy target")

    def test_probe_only_x64(self):
        self.probe_only("x86_64-w64-mingw32-", 0x8664)

    def test_probe_only_x86(self):
        self.probe_only("i686-w64-mingw32-", 0x14C)

    def test_normal_x86_builds_proxy_and_optional_probe(self):
        with tempfile.TemporaryDirectory(prefix="t3render-x86-build-") as tmp:
            build = Path(tmp) / "build"
            result = self.configure(build, "i686-w64-mingw32-", "-DT3RENDER_BUILD_D3D12_PROBE=ON")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.build(build)
            self.assertEqual(machine(build / "d3d8.dll"), 0x14C)
            self.assertEqual(machine(build / "t3render_d3d12_probe.exe"), 0x14C)

    def test_normal_x64_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="t3render-x64-reject-") as tmp:
            result = self.configure(Path(tmp) / "build", "x86_64-w64-mingw32-")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Win32/x86", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
