"""Host-only tests of the fixture build's experimental state-block windows.

These are fake COM calls, not Wine or evidence that TDS is safe. The production
proxy does not compile with T3RENDER_STATEBLOCK_TEST_FIXTURE.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class StateBlockExperimentalProxyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("clang") or shutil.which("gcc")
        cxx = shutil.which("clang++") or shutil.which("g++")
        if not cc or not cxx:
            raise unittest.SkipTest("host C and C++ compilers required")
        root = Path(__file__).resolve().parents[1]
        cls.temp = tempfile.TemporaryDirectory(prefix="t3render-stateblock-proxy-")
        layout = Path(cls.temp.name) / "layout.o"
        cls.binary = Path(cls.temp.name) / ("proxy-test.exe" if os.name == "nt" else "proxy-test")
        includes = ["-I", str(root / "tests/proxy_fakes"), "-I", str(root / "src")]
        try:
            subprocess.run([cc, "-std=c11", *includes, "-c", str(root / "src/vtable_layout.c"),
                            "-o", str(layout)], check=True, timeout=60)
            subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-pthread", "-DT3RENDER_PROXY_TEST",
                            "-DT3RENDER_STATEBLOCK_TEST_FIXTURE", *includes,
                            str(root / "src/proxy.cpp"), str(root / "src/buffer_proxy.cpp"),
                            str(root / "ir/buffer_shadow.cpp"), str(root / "ir/buffer_registry.cpp"),
                            str(root / "tests/proxy_mock.cpp"), str(layout), "-o", str(cls.binary)],
                           check=True, timeout=60)
        except Exception:
            cls.temp.cleanup()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_mode(self, mode):
        result = subprocess.run([str(self.binary), "state-exp-" + mode], text=True,
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_sequential_success_failure_and_wrapper_lifetime(self):
        # Preservation after a failed state call is an observation in this
        # fixture, not proof of safe failure semantics on a real runtime.
        for mode in ("preserve", "failure", "lifetime"):
            with self.subTest(mode=mode): self.run_mode(mode)

    def test_draw_overlap_invalidates_in_both_orders_even_if_state_call_fails(self):
        for mode in ("state-draw", "state-fail-draw", "draw-state", "draw-state-fail"):
            with self.subTest(mode=mode): self.run_mode(mode)

    def test_lock_overlap_invalidates_in_both_orders(self):
        for mode in ("state-lock", "lock-state"):
            with self.subTest(mode=mode): self.run_mode(mode)

    def test_experimental_define_requires_an_explicit_test_build(self):
        cxx = shutil.which("clang++") or shutil.which("g++")
        if not cxx:
            self.skipTest("host C++ compiler required")
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="t3render-stateblock-guard-") as directory:
            result = subprocess.run([cxx, "-std=c++17", "-DT3RENDER_STATEBLOCK_TEST_FIXTURE",
                                     "-I", str(root / "tests/proxy_fakes"), "-I", str(root / "src"),
                                     "-c", str(root / "src/proxy.cpp"), "-o", str(Path(directory) / "proxy.o")],
                                    text=True, capture_output=True, timeout=60)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("state-block experiment is restricted to explicitly designated test builds", result.stderr)


if __name__ == "__main__":
    unittest.main()
