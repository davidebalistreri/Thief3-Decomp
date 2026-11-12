"""Host-side COM mock for proxy forwarding, not a Windows ABI/runtime test."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class ProxyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("clang") or shutil.which("gcc")
        cxx = shutil.which("clang++") or shutil.which("g++")
        if not cc or not cxx:
            raise unittest.SkipTest("host C and C++ compilers are required")

        root = Path(__file__).resolve().parents[1]
        fake = root / "tests" / "proxy_fakes"
        cls.temp = tempfile.TemporaryDirectory(prefix="t3render-proxy-")
        obj = Path(cls.temp.name) / "layout.o"
        cls.binary = Path(cls.temp.name) / ("proxy-test.exe" if os.name == "nt" else "proxy-test")
        include = ["-I", str(fake), "-I", str(root / "src")]
        try:
            subprocess.run([cc, "-std=c11", *include, "-c", str(root / "src" / "vtable_layout.c"),
                            "-o", str(obj)], check=True, timeout=60)
            subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-pthread", "-DT3RENDER_PROXY_TEST", *include,
                            str(root / "src" / "proxy.cpp"), str(root / "src" / "buffer_proxy.cpp"),
                            str(root / "ir" / "buffer_shadow.cpp"), str(root / "ir" / "buffer_registry.cpp"),
                            str(root / "tests" / "proxy_mock.cpp"), str(obj), "-o", str(cls.binary)],
                           check=True, timeout=60)
        except Exception:
            cls.temp.cleanup()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_mode(self, mode=None, timeout=10):
        command = [str(self.binary)] + ([mode] if mode else [])
        result = subprocess.run(command, text=True, capture_output=True, timeout=timeout)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_forwarding_and_lifecycle(self):
        self.run_mode()

    def test_buffer_opt_out_and_missing_capture_remain_passive(self):
        for mode in ("opt-out", "invalid-opt-in", "no-capture"):
            with self.subTest(mode=mode): self.run_mode(mode)

    def test_buffer_forwarding_identity_and_lifetime(self):
        self.run_mode("opt-in")

    def test_alias_stateblock_and_reset_invalidate_device(self):
        for mode in ("alias", "device-alias", "device-query-alias", "stateblock", "reset", "raw-vertex", "raw-index"):
            with self.subTest(mode=mode): self.run_mode(mode)

    def test_capture_getter_last_release_does_not_reenter_capture_mutex(self):
        self.run_mode("reentrant", timeout=5)

    def test_overlapping_binding_and_draw_never_leave_known_bytes(self):
        for mode in ("set-draw", "draw-set"):
            with self.subTest(mode=mode): self.run_mode(mode, timeout=5)

    def test_immediate_draw_stream_unbinding_is_observed(self):
        self.run_mode("up-clears-stream")

    def test_capture_completion_or_error_drops_evidence_but_forwards_wrappers(self):
        for mode in ("stop-present", "stop-error"):
            with self.subTest(mode=mode): self.run_mode(mode)


if __name__ == "__main__":
    unittest.main()
