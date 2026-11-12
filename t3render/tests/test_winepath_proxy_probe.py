"""Host tests for isolated DLL-search orchestration; no Wine or game needed."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_run_synthetic_up_replay import write_pe
from test_trace_to_up_packet import fixture
import run_winepath_proxy_probe as probe


FAKE_WINE = r'''#!{python}
import os, pathlib, shutil, sys
args = sys.argv[1:]
exe = pathlib.Path(args[0])
assert pathlib.Path.cwd().resolve() == exe.parent.resolve() and not (exe.parent / "d3d8.dll").exists()
assert args[-2] == "--output"
def host(p):
    assert p.startswith("Z:")
    return pathlib.Path(p[2:].replace("\\", "/"))
mode = "capture" if "T3RENDER_CAPTURE_PATH" in os.environ else "baseline"
kind = os.environ.get("FAKE_CASE", "")
if mode == "capture":
    assert os.environ["WINEDLLOVERRIDES"].startswith("d3d8=n,b;")
    assert os.environ["T3RENDER_CAPTURE_FRAME"] == "0"
    assert "T3RENDER_CAPTURE_BUFFERS" not in os.environ
    proxy_dir = host(os.environ["WINEPATH"])
    assert (proxy_dir / "d3d8.dll").is_file() and proxy_dir != exe.parent
    path = os.environ["WINEPATH"] + "\\d3d8.dll"
    if kind == "wrong_path": path = "C:\\wrong\\d3d8.dll"
    load_kind = "builtin" if kind == "wrong_kind" else "native"
    if kind != "missing_log":
        print('0024:trace:loaddll:build_module Loaded L"' + path.replace("\\", "\\\\") + '" at 12345678: ' + load_kind)
    if kind == "duplicate_log":
        print('0024:trace:loaddll:build_module Loaded L"' + path.replace("\\", "\\\\") + '" at 87654321: native')
    if kind != "missing_trace":
        trace = host(os.environ["T3RENDER_CAPTURE_PATH"])
        if kind == "bad_trace": trace.write_text("broken\n")
        else: shutil.copyfile(os.environ["FAKE_TRACE"], trace)
else:
    assert os.environ["WINEDLLOVERRIDES"].startswith("d3d8=b;") and "WINEPATH" not in os.environ
    assert not any(k.startswith("T3RENDER_CAPTURE_") for k in os.environ)
    load_kind = "native" if kind == "baseline_native" else "builtin"
    print('0024:trace:loaddll:build_module Loaded L"C:\\\\windows\\\\system32\\\\d3d8.dll" at 12345678: ' + load_kind)
image = bytearray(b"P6\n256 256\n255\n" + bytes(256*256*3))
offset = len(b"P6\n256 256\n255\n") + (128*256+128)*3
image[offset:offset+3] = b"\xff\x00\x00"
if kind == "different_image" and mode == "capture": image[-1] = 1
if kind == "no_triangle": image[offset:offset+3] = b"\x00\x00\x00"
out = host(args[-1]); assert not out.exists(); out.write_bytes(image)
'''


class WinepathProxyProbeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-winepath-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.engine, self.bottle, self.build = (self.root / n for n in ("engine", "bottle", "build"))
        binary_dir = self.engine / "engine/bin"
        binary_dir.mkdir(parents=True)
        (self.engine / "frameworks").mkdir()
        wine = binary_dir / "wine"
        wine.write_text(FAKE_WINE.format(python=sys.executable), encoding="utf-8")
        wine.chmod(0o755)
        server = binary_dir / "wineserver"
        server.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        server.chmod(0o755)
        (self.bottle / "dosdevices").mkdir(parents=True)
        (self.bottle / "dosdevices/z:").symlink_to("/")
        self.exe, self.dll = self.build / "fixture/fixture.exe", self.build / "d3d8.dll"
        write_pe(self.exe)
        write_pe(self.dll, dll=True)
        self.trace = self.root / "fixture.jsonl"
        self.trace.write_text("".join(json.dumps(e) + "\n" for e in fixture()), encoding="utf-8")

    def execute(self, name="output", case=""):
        output = self.root / name
        with mock.patch.dict(os.environ, {"FAKE_TRACE": str(self.trace), "FAKE_CASE": case,
                                         "WINEPATH": "Z:\\poison", "T3RENDER_CAPTURE_BUFFERS": "1",
                                         "T3RENDER_CAPTURE_FRAMES": "3"}):
            result = probe.execute(str(self.engine), str(self.bottle), str(self.exe), str(self.dll), str(output), 5)
        return output, result

    def test_exact_native_load_complete_trace_and_cleanup(self):
        output, result = self.execute()
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["runs"]["baseline"]["d3d8_loads"][0]["kind"], "builtin")
        self.assertEqual(result["runs"]["capture"]["d3d8_loads"][0]["kind"], "native")
        self.assertEqual(result["runs"]["baseline"]["ppm_sha256"], result["runs"]["capture"]["ppm_sha256"])
        self.assertTrue((output / "capture.jsonl").exists())
        self.assertEqual(list(output.glob("t3render-winepath-*")), [])
        self.assertTrue((self.exe).exists())
        self.assertTrue((self.dll).exists())

    def test_wrong_loader_or_incomplete_capture_fails_and_cleans_scratch(self):
        for case in ("wrong_path", "wrong_kind", "missing_log", "duplicate_log", "missing_trace",
                     "bad_trace", "different_image", "no_triangle", "baseline_native"):
            with self.subTest(case=case):
                with self.assertRaises(probe.RunError): self.execute("output-" + case, case)
                output = self.root / ("output-" + case)
                self.assertEqual(json.loads((output / "result.json").read_text())["status"], "FAIL")
                self.assertEqual(list(output.glob("t3render-winepath-*")), [])

    def test_existing_output_is_not_modified(self):
        output = self.root / "existing"
        output.mkdir()
        keep = output / "keep.txt"
        keep.write_text("keep", encoding="ascii")
        with self.assertRaises(probe.RunError): self.execute("existing")
        self.assertEqual(keep.read_text(), "keep")
        self.assertEqual(list(output.iterdir()), [keep])


if __name__ == "__main__":
    unittest.main()
