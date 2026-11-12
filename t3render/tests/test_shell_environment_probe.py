"""Host checks for the bounded ShellExecuteEx environment probe."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import run_shell_environment_probe as probe


class ShellEnvironmentProbeTests(unittest.TestCase):
    def test_snapshot_requires_exact_fields_and_values(self):
        with tempfile.TemporaryDirectory(prefix="t3render-shell-environment-test-") as directory:
            root = Path(directory)
            path = root / "parent.env"
            expected = probe.expected_environment(root)
            valid = "".join(f"{key}={value}\n" for key, value in expected.items()).encode("ascii")
            path.write_bytes(valid)
            probe.validate_environment(path, expected)
            for bad in (valid.replace(b"F11", b"F12"), valid.replace(b"<absent>", b"0"),
                        b"\n".join(valid.splitlines()[:-1]), valid + valid.splitlines()[0] + b"\n",
                        valid + b"x" * 16385, b"\xff" + valid):
                path.write_bytes(bad)
                with self.subTest(sample=bad[:25]), self.assertRaises(probe.RunError):
                    probe.validate_environment(path, expected)

    def test_x86_fixture_compiles(self):
        compiler = shutil.which("i686-w64-mingw32-g++")
        if not compiler:
            self.skipTest("i686 MinGW-w64 is required")
        with tempfile.TemporaryDirectory(prefix="t3render-shell-fixture-build-") as directory:
            result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-static",
                                     "-static-libgcc", "-static-libstdc++",
                                     str(ROOT / "tests/shell_environment_fixture.cpp"), "-lshell32",
                                     "-o", str(Path(directory) / "fixture.exe")], capture_output=True, text=True,
                                    timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
