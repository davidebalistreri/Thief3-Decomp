"""Exercise the full host orchestration with a fake Wine, never the game."""
import copy
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_trace_to_up_packet import fixture, VERTICES
from test_trace_to_camera_packet import fixture as camera_fixture
from test_trace_to_multi_xyz_packet import fixture as multi_fixture
from test_trace_buffer_to_up_packet import fixture as proxy_buffer_fixture


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tools/run_synthetic_up_replay.py"
FAKE_WINE = r'''#!{python}
import json, os, pathlib, shutil, sys, time
args = sys.argv[1:]
kind = "replay" if pathlib.Path(args[0]).name == "t3render_d3d12_replay_up.exe" else ("capture" if "T3RENDER_CAPTURE_FRAME" in os.environ else "baseline")
print(json.dumps({{"kind": kind, "cwd": os.getcwd(), "args": args,
 "prefix": os.environ.get("WINEPREFIX"), "server": os.environ.get("WINESERVER"),
 "overrides": os.environ.get("WINEDLLOVERRIDES"), "dllpath": os.environ.get("WINEDLLPATH_PREPEND"),
 "inherited": [os.environ.get(x) for x in ("WINEDLLPATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH")],
 "capture_key": os.environ.get("T3RENDER_CAPTURE_KEY"), "frame": os.environ.get("T3RENDER_CAPTURE_FRAME"),
 "buffers": os.environ.get("T3RENDER_CAPTURE_BUFFERS")}}), flush=True)
if os.environ.get("FAKE_TIMEOUT") == kind:
    time.sleep(30)
if os.environ.get("FAKE_FAIL") == kind:
    sys.exit(37)
if "--verify-buffer-readback" in args and os.environ.get("FAKE_READBACK_MISSING") != kind:
    marker = "buffer_readback vertex_offset=20 vertex_bytes=80 index_offset=4 index_bytes=6 result=PASS"
    if os.environ.get("FAKE_READBACK_BAD") == kind:
        marker = marker.replace("index_bytes=6", "index_bytes=5")
    print(marker, flush=True)
    if os.environ.get("FAKE_READBACK_DUPLICATE") == kind:
        print(marker, flush=True)
if "--wrapped-buffers" in args:
    for marker_name, marker in (("writes", "wrapped_buffer_writes vertex_bytes=80 index_bytes=6 result=PASS"),
                                ("lifetime", "wrapped_buffer_lifetime result=PASS")):
        case = kind + ":" + marker_name
        if os.environ.get("FAKE_WRAP_MISSING") == case:
            continue
        if os.environ.get("FAKE_WRAP_BAD") == case:
            marker = marker.replace("result=PASS", "result=FAIL")
        print(marker, flush=True)
        if os.environ.get("FAKE_WRAP_DUPLICATE") == case:
            print(marker, flush=True)
def host(path):
    assert path.startswith("Z:")
    return pathlib.Path(path[2:].replace("\\", "/"))
assert args[-2] == "--output"
out = host(args[-1])
assert not out.exists()
data = b"P6\n256 256\n255\n" + (b"\x00\x00\x00" * (256 * 256 - 1)) + b"\xff\x00\x00"
multi = "--multi-camera" in args or (kind == "replay" and host(args[1]).read_text().startswith("T3R_MULTI_XYZ_V1\n"))
if multi and not os.environ.get("FAKE_SCENE_MISSING"):
    pixels = bytearray(256 * 256 * 3)
    for i in range(101): pixels[3*i:3*i+3] = b"\xff\x00\x00"
    for i in range(200, 1201): pixels[3*i:3*i+3] = b"\x00\xff\x00"
    center = (128 * 256 + 128) * 3
    pixels[center:center+3] = b"\x00\xff\x00"
    data = b"P6\n256 256\n255\n" + pixels
if os.environ.get("FAKE_DIFFER") == kind:
    data = data[:-3] + b"\x00\xff\x00"
out.write_bytes(data)
if kind == "capture":
    trace = host(os.environ["T3RENDER_CAPTURE_PATH"])
    assert not trace.exists()
    name = ("FAKE_TRACE_BUFFERED_PROXY" if "--writeonly-buffers" in args else
            "FAKE_TRACE_BUFFERED_WRAPPED" if "--wrapped-buffers" in args else
            "FAKE_TRACE_BUFFERED" if "--buffered" in args else ("FAKE_TRACE_MULTI" if "--multi-camera" in args else
            "FAKE_TRACE_CAMERA" if "--camera" in args else "FAKE_TRACE"))
    shutil.copyfile(os.environ[name], trace)
    if "--buffer-snapshot" in args and not os.environ.get("FAKE_SNAPSHOT_MISSING"):
        snapshot = host(args[args.index("--buffer-snapshot") + 1])
        assert not snapshot.exists()
        if os.environ.get("FAKE_SNAPSHOT_CORRUPT"):
            snapshot.write_text("invalid snapshot\n")
        else:
            shutil.copyfile(os.environ["FAKE_SNAPSHOT"], snapshot)
if kind == "replay":
    assert host(args[1]).read_text().splitlines()[0] in ("T3R_UP_V1", "T3R_XYZ_V1", "T3R_MULTI_XYZ_V1")
'''


def write_pe(path, *, machine=0x14c, dll=False, magic=0x10b):
    path.parent.mkdir(parents=True, exist_ok=True)
    dos = bytearray(64)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3c, 64)
    flags = 0x0002 | (0x2000 if dll else 0)
    header = b"PE\0\0" + struct.pack("<HHIIIHH", machine, 0, 0, 0, 0, 96, flags)
    optional = bytearray(96)
    struct.pack_into("<H", optional, 0, magic)
    path.write_bytes(dos + header + optional)


def buffered_fixture(wrapped=False):
    events = fixture()
    draw = events[4]
    draw.update(call="DrawIndexedPrimitive", min_vertex_index=1, num_vertices=3, start_index=2,
                vertex_stride=0, index_format=0,
                streams=[{"stream": 0, "hr": 0, "buffer": "0x1234", "stride": 20,
                          "description": {"size": 100, "fvf": 0x44, "usage": 8 if wrapped else 0, "pool": 1}}],
                indices={"hr": 0, "buffer": "0x5678", "base_vertex_index": 1,
                         "description": {"format": 101, "size": 10, "usage": 8 if wrapped else 0, "pool": 1}})
    draw.pop("up_data")
    return events


class SyntheticReplayRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-synthetic-runner-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.engine, self.bottle, self.build = (self.root / name for name in ("test engine", "test bottle", "build"))
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
        (self.bottle / "bottle.json").write_text('{"keep": true}\n')
        write_pe(self.build / "d3d8_up_fixture/t3render_d3d8_up_fixture.exe")
        write_pe(self.build / "d3d8.dll", dll=True)
        write_pe(self.build / "t3render_d3d12_replay_up.exe")
        self.trace = self.root / "fixture.jsonl"
        self.trace.write_text("".join(json.dumps(event) + "\n" for event in fixture()), encoding="utf-8")
        self.camera_trace = self.root / "camera.jsonl"
        self.camera_trace.write_text("".join(json.dumps(event) + "\n" for event in camera_fixture()), encoding="utf-8")
        self.multi_trace = self.root / "multi.jsonl"
        self.multi_trace.write_text("".join(json.dumps(event) + "\n" for event in multi_fixture()), encoding="utf-8")
        self.buffered_trace = self.root / "buffered.jsonl"
        self.buffered_trace.write_text("".join(json.dumps(event) + "\n" for event in buffered_fixture()), encoding="utf-8")
        self.wrapped_trace = self.root / "wrapped.jsonl"
        self.wrapped_trace.write_text("".join(json.dumps(event) + "\n" for event in buffered_fixture(wrapped=True)), encoding="utf-8")
        self.proxy_trace = self.root / "proxy-buffer.jsonl"
        events = proxy_buffer_fixture()
        events[4]["streams"][0]["description"].update(usage=8, pool=1)
        events[4]["indices"]["description"].update(usage=8, pool=1)
        self.proxy_trace.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
        self.buffer_snapshot = self.root / "buffer-snapshot.txt"
        vertices = [VERTICES[0], *VERTICES]
        vb = b"".join(struct.pack("<4fI", *vertex) for vertex in vertices)
        ib = struct.pack("<3H", 1, 2, 3)
        self.buffer_snapshot.write_text("\n".join(["T3R_BUFFER_V1", "256 256", "4278190080", "1 1 3 2",
                                                     "20 20 80", "4 6", vb.hex(), ib.hex(), ""]))
        self.output = self.root / "output with spaces"

    def run_runner(self, *, output=None, extra_env=None, timeout="5", engine=None, bottle=None, build=None,
                   camera=False, buffered=False, verify_buffer_readback=False, replay_buffer_snapshot=False,
                   wrapped_buffers=False, proxy_buffer_replay=False, multi_camera=False):
        env = os.environ.copy()
        env.update({"FAKE_TRACE": str(self.trace), "FAKE_TRACE_CAMERA": str(self.camera_trace),
                    "FAKE_TRACE_MULTI": str(self.multi_trace),
                    "FAKE_TRACE_BUFFERED": str(self.buffered_trace), "FAKE_TRACE_BUFFERED_WRAPPED": str(self.wrapped_trace),
                    "FAKE_TRACE_BUFFERED_PROXY": str(self.proxy_trace),
                    "FAKE_SNAPSHOT": str(self.buffer_snapshot),
                    "WINEDLLPATH": "poison/dxmt",
                    "WINEDLLPATH_PREPEND": "poison/dxmt", "DYLD_LIBRARY_PATH": "poison",
                    "DYLD_FRAMEWORK_PATH": "poison", "T3RENDER_CAPTURE_KEY": "F11",
                    "T3RENDER_CAPTURE_FRAME": "12", "T3RENDER_CAPTURE_PATH": "poison",
                    "T3RENDER_CAPTURE_BUFFERS": "poison"})
        env.update(extra_env or {})
        args = [sys.executable, "-B", str(RUNNER), "--engine", str(engine or self.engine),
                               "--bottle", str(bottle or self.bottle), "--build-dir", str(build or self.build),
                               "--output-dir", str(output or self.output), "--timeout", timeout]
        if camera:
            args.append("--camera")
        if multi_camera:
            args.append("--multi-camera")
        if buffered:
            args.append("--buffered")
        if verify_buffer_readback:
            args.append("--verify-buffer-readback")
        if wrapped_buffers:
            args.append("--wrapped-buffers")
        if replay_buffer_snapshot:
            args.append("--replay-buffer-snapshot")
        if proxy_buffer_replay:
            args.append("--proxy-buffer-replay")
        return subprocess.run(args, env=env, text=True, capture_output=True, timeout=20)

    def manifest(self):
        return json.loads((self.output / "result.json").read_text())

    def log(self, name):
        lines = (self.output / f"{name}.log").read_text().splitlines()
        return json.loads(lines[1])

    def test_success_and_isolation(self):
        result = self.run_runner()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        manifest = self.manifest()
        self.assertEqual(manifest["status"], "PASS")
        self.assertEqual(manifest["fixture_mode"], "screen_space")
        self.assertEqual(manifest["stages"], ["baseline", "capture", "convert", "replay", "compare"])
        hashes = manifest["sha256"]
        self.assertEqual(hashes["baseline_ppm"], hashes["captured_ppm"])
        self.assertEqual(hashes["baseline_ppm"], hashes["replay_ppm"])
        self.assertEqual((self.output / "baseline.ppm").read_bytes(), (self.output / "replay.ppm").read_bytes())
        self.assertFalse((self.output / "baseline/d3d8.dll").exists())
        self.assertTrue((self.output / "captured/d3d8.dll").is_file())
        self.assertEqual((self.bottle / "bottle.json").read_text(), '{"keep": true}\n')
        for name in ("baseline", "capture", "replay"):
            log = self.log(name)
            self.assertEqual(log["prefix"], str(self.bottle))
            self.assertEqual(log["server"], str(self.engine / "engine/bin/wineserver"))
            self.assertEqual(log["inherited"], [None, None, None])
            self.assertNotIn("dxmt", log["dllpath"].lower())
            self.assertIsNone(log["capture_key"])
            self.assertEqual(log["frame"], "0" if name == "capture" else None)
            self.assertIsNone(log["buffers"])
            self.assertTrue(log["overrides"].startswith("d3d8=n,b;" if name == "capture" else "d3d8=b;"))
            self.assertTrue(log["args"][-1].startswith("Z:\\"))
        self.assertEqual(self.log("baseline")["cwd"], str((self.output / "baseline").resolve()))
        self.assertEqual(self.log("capture")["cwd"], str((self.output / "captured").resolve()))
        self.assertIn("Synthetic fixture only", (self.output / "convert.log").read_text())

    def test_proxy_buffer_replay_uses_capture_trace_and_isolates_opt_in(self):
        result = self.run_runner(buffered=True, proxy_buffer_replay=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        manifest = self.manifest()
        self.assertEqual(manifest["status"], "PASS")
        self.assertEqual(manifest["test"], "synthetic_d3d8_proxy_buffer_dx12_replay")
        self.assertEqual(manifest["dx12_replay"], "completed")
        self.assertEqual(manifest["buffer_capture"],
                         {"producer": "proxy_application_lock_shadow", "conversion": "PASS"})
        self.assertEqual(manifest["stages"],
                         ["baseline", "capture", "trace_validation", "convert", "replay", "compare"])
        self.assertEqual(manifest["sha256"]["baseline_ppm"], manifest["sha256"]["captured_ppm"])
        self.assertEqual(manifest["sha256"]["baseline_ppm"], manifest["sha256"]["replay_ppm"])
        self.assertNotIn("buffer_snapshot", manifest["sha256"])
        self.assertFalse((self.output / "buffer-snapshot.txt").exists())
        for name in ("baseline", "capture"):
            self.assertIn("--writeonly-buffers", self.log(name)["args"])
        self.assertIsNone(self.log("baseline")["buffers"])
        self.assertEqual(self.log("capture")["buffers"], "1")
        self.assertIsNone(self.log("replay")["buffers"])
        self.assertIn("trace_buffer_to_up_packet.py", (self.output / "convert.log").read_text())
        self.assertTrue((self.output / "packet.txt").read_text().startswith("T3R_UP_V1\n"))
        self.assertTrue(any("does not validate TDS" in item for item in manifest["limitations"]))

    def test_proxy_buffer_replay_rejects_unavailable_or_unstable_observations(self):
        cases = ("missing_vertex", "missing_index", "unstable", "missing_capability", "malformed")
        for case in cases:
            with self.subTest(case=case):
                events = proxy_buffer_fixture()
                events[4]["streams"][0]["description"].update(usage=8, pool=1)
                events[4]["indices"]["description"].update(usage=8, pool=1)
                if case == "missing_vertex": events[4]["buffer_data"]["vertex"]["status"] = "unknown"
                if case == "missing_index": events[4]["buffer_data"]["index"].pop("data")
                if case == "unstable": events[5]["buffer_stable"] = False
                if case == "missing_capability": events[0]["buffer_capture"]["requested"] = False
                trace = self.root / (case + ".jsonl")
                trace.write_text("{}\n" if case == "malformed" else
                                 "".join(json.dumps(e) + "\n" for e in events), encoding="utf-8")
                output = self.root / ("output-" + case)
                result = self.run_runner(buffered=True, proxy_buffer_replay=True, output=output,
                                         extra_env={"FAKE_TRACE_BUFFERED_PROXY": str(trace)})
                self.assertNotEqual(result.returncode, 0)
                manifest = json.loads((output / "result.json").read_text())
                self.assertIn(manifest["failed_stage"], ("trace_validation", "convert"))
                self.assertEqual(manifest["dx12_replay"], "not_run")
                self.assertFalse((output / "replay.log").exists())

    def test_proxy_buffer_replay_detects_image_mismatch(self):
        result = self.run_runner(buffered=True, proxy_buffer_replay=True, extra_env={"FAKE_DIFFER": "replay"})
        self.assertNotEqual(result.returncode, 0)
        manifest = self.manifest()
        self.assertEqual(manifest["failed_stage"], "compare")
        self.assertEqual(manifest["dx12_replay"], "completed")
        self.assertNotEqual(manifest["sha256"]["baseline_ppm"], manifest["sha256"]["replay_ppm"])

    def test_proxy_buffer_replay_option_exclusions_precede_output(self):
        cases = ({}, {"buffered": True, "verify_buffer_readback": True},
                 {"buffered": True, "wrapped_buffers": True},
                 {"buffered": True, "verify_buffer_readback": True, "replay_buffer_snapshot": True})
        for options in cases:
            with self.subTest(options=options):
                result = self.run_runner(proxy_buffer_replay=True, **options)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("requires", result.stderr)
                self.assertFalse(self.output.exists())

    def test_camera_mode_passes_fixture_flag_and_replays_xyz_packet(self):
        result = self.run_runner(camera=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.manifest()["fixture_mode"], "camera")
        self.assertTrue((self.output / "packet.txt").read_text().startswith("T3R_XYZ_V1\n"))
        for stage in ("baseline", "capture"):
            self.assertIn("--camera", self.log(stage)["args"])
        self.assertIn("--synthetic-fixture", (self.output / "convert.log").read_text())

    def test_multi_camera_replays_ordered_draws_and_checks_overlap(self):
        result = self.run_runner(multi_camera=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.manifest()["fixture_mode"], "multi_camera")
        self.assertIn("--multi-camera", self.log("baseline")["args"])
        self.assertIn("--multi-camera", self.log("capture")["args"])
        self.assertTrue((self.output / "packet.txt").read_text().startswith("T3R_MULTI_XYZ_V1\n"))
        self.assertEqual(self.manifest()["sha256"]["baseline_ppm"], self.manifest()["sha256"]["replay_ppm"])

    def test_multi_camera_rejects_identical_but_missing_scene(self):
        result = self.run_runner(multi_camera=True, extra_env={"FAKE_SCENE_MISSING": "1"})
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.manifest()["failed_stage"], "compare")
        self.assertIn("lacks both colors", self.manifest()["error"])

    def test_multi_camera_rejects_extra_draw_or_wrong_camera_even_if_pixels_match(self):
        for case in ("duplicate", "camera"):
            events = multi_fixture()
            if case == "duplicate":
                duplicate = copy.deepcopy(events[4])
                duplicate["op_id"] = 3
                events[6]["op_id"] = events[7]["op_id"] = 4
                events[6:6] = [duplicate, {"event": "draw_result", "frame": 0, "op_id": 3, "hr": 0}]
                events[-2]["draws"] = 3
            else:
                events[6]["transforms"]["view"][12] = 0.0
            trace = self.root / f"wrong-{case}.jsonl"
            trace.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
            output = self.root / f"wrong-{case}"
            result = self.run_runner(multi_camera=True, output=output, extra_env={"FAKE_TRACE_MULTI": str(trace)})
            with self.subTest(case=case):
                self.assertNotEqual(result.returncode, 0)
                manifest = json.loads((output / "result.json").read_text())
                self.assertEqual(manifest["failed_stage"], "convert")
                self.assertIn("known fixture", manifest["error"])
                self.assertFalse((output / "replay.log").exists())

    def test_camera_mode_rejects_a_screen_space_capture(self):
        result = self.run_runner(camera=True, extra_env={"FAKE_TRACE_CAMERA": str(self.trace)})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.manifest()["failed_stage"], "convert")
        self.assertIn("does not match the selected fixture mode", result.stderr)

    def test_buffered_mode_compares_images_and_validates_metadata_without_replay(self):
        (self.build / "t3render_d3d12_replay_up.exe").unlink()
        result = self.run_runner(buffered=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("DX12 replay not run", result.stdout)
        manifest = self.manifest()
        self.assertEqual(manifest["status"], "PASS")
        self.assertEqual(manifest["fixture_mode"], "buffered")
        self.assertEqual(manifest["dx12_replay"], "not_run")
        self.assertEqual(manifest["stages"], ["baseline", "capture", "trace_validation", "compare"])
        self.assertEqual(manifest["sha256"]["baseline_ppm"], manifest["sha256"]["captured_ppm"])
        self.assertNotIn("replay_exe", manifest["sha256"])
        self.assertEqual(manifest["buffered_draw"]["base_vertex_index"], 1)
        self.assertEqual(manifest["buffered_draw"]["stride"], 20)
        self.assertTrue(any("buffer bytes and DX12 replay" in line for line in manifest["limitations"]))
        for name in ("baseline", "capture"):
            self.assertIn("--buffered", self.log(name)["args"])
        for name in ("replay.log", "replay.ppm", "convert.log", "packet.txt"):
            self.assertFalse((self.output / name).exists())

    def test_mode_options_are_exclusive(self):
        result = self.run_runner(camera=True, buffered=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

    def test_buffered_readback_checks_both_runs_and_reports_ranges(self):
        result = self.run_runner(buffered=True, verify_buffer_readback=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        manifest = self.manifest()
        self.assertEqual(manifest["buffer_readback"], {"requested": True, "baseline": "PASS", "capture": "PASS",
                         "vertex_offset": 20, "vertex_bytes": 80, "index_offset": 4, "index_bytes": 6})
        self.assertEqual(manifest["dx12_replay"], "not_run")
        self.assertTrue(any("not serialized" in item for item in manifest["limitations"]))
        for stage in ("baseline", "capture"):
            self.assertIn("--verify-buffer-readback", self.log(stage)["args"])
            self.assertIn("buffer_readback vertex_offset=20 vertex_bytes=80 index_offset=4 index_bytes=6 result=PASS",
                          (self.output / f"{stage}.log").read_text())

    def test_buffer_snapshot_replays_fixture_bytes_on_dx12(self):
        result = self.run_runner(buffered=True, verify_buffer_readback=True, replay_buffer_snapshot=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        manifest = self.manifest()
        self.assertEqual(manifest["status"], "PASS")
        self.assertEqual(manifest["dx12_replay"], "completed")
        self.assertEqual(manifest["buffer_snapshot"], {"producer": "fixture_readback_not_proxy_trace", "conversion": "PASS"})
        self.assertEqual(manifest["stages"], ["baseline", "capture", "trace_validation", "convert", "replay", "compare"])
        self.assertEqual(manifest["sha256"]["baseline_ppm"], manifest["sha256"]["replay_ppm"])
        self.assertIn("buffer_snapshot", manifest["sha256"])
        self.assertTrue((self.output / "packet.txt").read_text().startswith("T3R_UP_V1\n"))
        self.assertNotIn("--buffer-snapshot", self.log("baseline")["args"])
        self.assertIn("--buffer-snapshot", self.log("capture")["args"])
        self.assertIn("buffer_snapshot_to_up_packet.py", (self.output / "convert.log").read_text())
        self.assertTrue(any("proxy trace does not contain" in item for item in manifest["limitations"]))

    def test_wrapped_buffer_capture_validates_write_and_lifetime_in_both_runs(self):
        result = self.run_runner(buffered=True, wrapped_buffers=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        manifest = self.manifest()
        self.assertEqual(manifest["status"], "PASS")
        self.assertEqual(manifest["dx12_replay"], "not_run")
        self.assertEqual(manifest["wrapped_buffers"], {"requested": True, "baseline": "PASS", "capture": "PASS"})
        self.assertEqual((manifest["buffered_draw"]["vertex_usage"], manifest["buffered_draw"]["index_usage"]), (8, 8))
        self.assertEqual((manifest["buffered_draw"]["vertex_pool"], manifest["buffered_draw"]["index_pool"]), (1, 1))
        self.assertNotIn("buffer_readback", manifest)
        self.assertNotIn("buffer_snapshot", manifest)
        self.assertTrue(any("not integrated into the proxy" in text for text in manifest["limitations"]))
        for stage in ("baseline", "capture"):
            args = self.log(stage)["args"]
            self.assertIn("--wrapped-buffers", args)
            self.assertNotIn("--verify-buffer-readback", args)
            self.assertNotIn("--buffer-snapshot", args)
            log = (self.output / f"{stage}.log").read_text()
            self.assertIn("wrapped_buffer_writes vertex_bytes=80 index_bytes=6 result=PASS", log)
            self.assertIn("wrapped_buffer_lifetime result=PASS", log)
        self.assertFalse((self.output / "replay.log").exists())

    def test_wrapped_buffer_snapshot_replays_fixture_write_bytes(self):
        result = self.run_runner(buffered=True, wrapped_buffers=True, replay_buffer_snapshot=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        manifest = self.manifest()
        self.assertEqual(manifest["status"], "PASS")
        self.assertEqual(manifest["dx12_replay"], "completed")
        self.assertEqual(manifest["wrapped_buffers"], {"requested": True, "baseline": "PASS", "capture": "PASS"})
        self.assertEqual(manifest["buffer_snapshot"], {"producer": "fixture_wrapper_not_proxy_trace", "conversion": "PASS"})
        self.assertNotIn("buffer_readback", manifest)
        self.assertEqual(manifest["stages"], ["baseline", "capture", "trace_validation", "convert", "replay", "compare"])
        self.assertEqual(manifest["sha256"]["baseline_ppm"], manifest["sha256"]["replay_ppm"])
        self.assertIn("buffer_snapshot", manifest["sha256"])
        self.assertTrue((self.output / "packet.txt").read_text().startswith("T3R_UP_V1\n"))
        self.assertNotIn("--buffer-snapshot", self.log("baseline")["args"])
        self.assertIn("--buffer-snapshot", self.log("capture")["args"])
        self.assertTrue(any("proxy trace does not contain" in item for item in manifest["limitations"]))

    def test_wrapped_marker_missing_wrong_or_duplicate_fails_closed(self):
        for variable in ("FAKE_WRAP_MISSING", "FAKE_WRAP_BAD", "FAKE_WRAP_DUPLICATE"):
            for stage in ("baseline", "capture"):
                for marker in ("writes", "lifetime"):
                    with self.subTest(variable=variable, stage=stage, marker=marker):
                        output = self.root / f"{variable}-{stage}-{marker}"
                        result = self.run_runner(buffered=True, wrapped_buffers=True, replay_buffer_snapshot=True,
                                                 output=output, extra_env={variable: stage + ":" + marker})
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn("wrapped buffer write/lifetime PASS", result.stderr)
                        manifest = json.loads((output / "result.json").read_text())
                        self.assertEqual(manifest["failed_stage"], stage)
                        self.assertEqual(manifest["wrapped_buffers"][stage], "not_verified")
                        if stage == "capture":
                            self.assertEqual(manifest["wrapped_buffers"]["baseline"], "PASS")
                        self.assertFalse((output / "replay.log").exists())

    def test_wrapped_requires_writeonly_usage_on_both_bound_buffers(self):
        for binding in ("vertex", "index"):
            for value in (0, None, True):
                with self.subTest(binding=binding, value=value):
                    events = buffered_fixture(wrapped=True)
                    draw = events[4]
                    desc = draw["streams"][0]["description"] if binding == "vertex" else draw["indices"]["description"]
                    if value is None:
                        del desc["usage"]
                    else:
                        desc["usage"] = value
                    self.wrapped_trace.write_text("".join(json.dumps(event) + "\n" for event in events))
                    output = self.root / f"usage-{binding}-{value}"
                    result = self.run_runner(buffered=True, wrapped_buffers=True, replay_buffer_snapshot=True, output=output)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("WRITEONLY", result.stderr)
                    manifest = json.loads((output / "result.json").read_text())
                    self.assertEqual(manifest["failed_stage"], "trace_validation")
                    self.assertFalse((output / "replay.log").exists())

    def test_wrapped_requires_managed_pool_on_both_bound_buffers(self):
        for binding in ("vertex", "index"):
            for value in (0, None, True):
                with self.subTest(binding=binding, value=value):
                    events = buffered_fixture(wrapped=True)
                    draw = events[4]
                    desc = draw["streams"][0]["description"] if binding == "vertex" else draw["indices"]["description"]
                    if value is None:
                        del desc["pool"]
                    else:
                        desc["pool"] = value
                    self.wrapped_trace.write_text("".join(json.dumps(event) + "\n" for event in events))
                    output = self.root / f"pool-{binding}-{value}"
                    result = self.run_runner(buffered=True, wrapped_buffers=True, replay_buffer_snapshot=True, output=output)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("MANAGED", result.stderr)
                    manifest = json.loads((output / "result.json").read_text())
                    self.assertEqual(manifest["failed_stage"], "trace_validation")
                    self.assertFalse((output / "replay.log").exists())

    def test_wrapped_snapshot_absent_or_corrupt_fails_before_replay(self):
        for name in ("FAKE_SNAPSHOT_MISSING", "FAKE_SNAPSHOT_CORRUPT"):
            with self.subTest(name=name):
                output = self.root / ("wrapped-" + name)
                result = self.run_runner(buffered=True, wrapped_buffers=True, replay_buffer_snapshot=True,
                                         output=output, extra_env={name: "1"})
                self.assertNotEqual(result.returncode, 0)
                manifest = json.loads((output / "result.json").read_text())
                self.assertEqual(manifest["failed_stage"], "convert")
                self.assertEqual(manifest["buffer_snapshot"]["producer"], "fixture_wrapper_not_proxy_trace")
                self.assertEqual(manifest["buffer_snapshot"]["conversion"], "not_verified")
                self.assertEqual(manifest["dx12_replay"], "not_run")
                self.assertFalse((output / "replay.log").exists())

    def test_wrapped_mode_requires_buffered_and_excludes_readback(self):
        for options in ({"wrapped_buffers": True},
                        {"wrapped_buffers": True, "camera": True},
                        {"wrapped_buffers": True, "buffered": True, "verify_buffer_readback": True}):
            with self.subTest(options=options):
                result = self.run_runner(**options)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.output.exists())
        result = self.run_runner(buffered=True, replay_buffer_snapshot=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--verify-buffer-readback or --wrapped-buffers", result.stderr)
        self.assertFalse(self.output.exists())

    def test_buffer_snapshot_absent_or_corrupt_fails_before_replay(self):
        for name in ("FAKE_SNAPSHOT_MISSING", "FAKE_SNAPSHOT_CORRUPT"):
            with self.subTest(name=name):
                output = self.root / name
                result = self.run_runner(buffered=True, verify_buffer_readback=True, replay_buffer_snapshot=True,
                                         output=output, extra_env={name: "1"})
                self.assertNotEqual(result.returncode, 0)
                manifest = json.loads((output / "result.json").read_text())
                self.assertEqual(manifest["failed_stage"], "convert")
                self.assertEqual(manifest["buffer_snapshot"]["conversion"], "not_verified")
                self.assertEqual(manifest["dx12_replay"], "not_run")
                self.assertFalse((output / "replay.log").exists())

    def test_buffer_snapshot_failed_replay_is_recorded_as_attempted(self):
        result = self.run_runner(buffered=True, verify_buffer_readback=True, replay_buffer_snapshot=True,
                                 extra_env={"FAKE_FAIL": "replay"})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.manifest()["failed_stage"], "replay")
        self.assertEqual(self.manifest()["dx12_replay"], "attempted")

    def test_buffer_snapshot_replay_detects_pixel_mismatch(self):
        result = self.run_runner(buffered=True, verify_buffer_readback=True, replay_buffer_snapshot=True,
                                 extra_env={"FAKE_DIFFER": "replay"})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.manifest()["failed_stage"], "compare")
        self.assertEqual(self.manifest()["dx12_replay"], "completed")
        self.assertNotEqual(self.manifest()["sha256"]["baseline_ppm"], self.manifest()["sha256"]["replay_ppm"])

    def test_buffer_snapshot_replay_requires_both_flags_before_output(self):
        for buffered, readback in ((False, False), (True, False), (False, True)):
            with self.subTest(buffered=buffered, readback=readback):
                result = self.run_runner(buffered=buffered, verify_buffer_readback=readback, replay_buffer_snapshot=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("requires", result.stderr)
                self.assertFalse(self.output.exists())

    def test_buffer_snapshot_replay_requires_x86_executable(self):
        (self.build / "t3render_d3d12_replay_up.exe").unlink()
        result = self.run_runner(buffered=True, verify_buffer_readback=True, replay_buffer_snapshot=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("PE32", result.stderr)
        self.assertFalse(self.output.exists())

    def test_readback_missing_wrong_or_duplicate_result_fails_closed(self):
        cases = (("FAKE_READBACK_MISSING", "baseline"), ("FAKE_READBACK_MISSING", "capture"),
                 ("FAKE_READBACK_BAD", "baseline"), ("FAKE_READBACK_BAD", "capture"),
                 ("FAKE_READBACK_DUPLICATE", "capture"))
        for variable, stage in cases:
            with self.subTest(variable=variable, stage=stage):
                output = self.root / f"{variable}-{stage}"
                result = self.run_runner(buffered=True, verify_buffer_readback=True, output=output,
                                         extra_env={variable: stage})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("buffer readback PASS", result.stderr)
                manifest = json.loads((output / "result.json").read_text())
                self.assertEqual(manifest["failed_stage"], stage)
                self.assertEqual(manifest["buffer_readback"][stage], "not_verified")
                if stage == "capture":
                    self.assertEqual(manifest["buffer_readback"]["baseline"], "PASS")
                self.assertFalse((output / "replay.log").exists())

    def test_readback_requires_buffered_mode_before_any_output(self):
        for camera in (False, True):
            with self.subTest(camera=camera):
                result = self.run_runner(camera=camera, verify_buffer_readback=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("requires --buffered", result.stderr)
                self.assertFalse(self.output.exists())

    def test_buffered_trace_malformed_or_wrong_bindings_fail(self):
        def remove_stream(draw): draw.pop("streams")
        def duplicate_stream(draw): draw["streams"].append(dict(draw["streams"][0]))
        def invalid_base(draw): draw["indices"]["base_vertex_index"] = 0
        def invalid_stride(draw): draw["streams"][0]["stride"] = 24
        def index_format(draw): draw["indices"]["description"]["format"] = 102
        def absent_ib(draw): draw["indices"]["buffer"] = None
        def failed_getter(draw): draw["streams"][0]["hr"] = 0x80004005
        def wrong_start(draw): draw["start_index"] = 0
        def wrong_vertices(draw): draw["num_vertices"] = True
        def wrong_call(draw): draw["call"] = "DrawPrimitiveUP"
        for mutate in (remove_stream, duplicate_stream, invalid_base, invalid_stride, index_format,
                       absent_ib, failed_getter, wrong_start, wrong_vertices, wrong_call):
            with self.subTest(mutation=mutate.__name__):
                events = buffered_fixture()
                mutate(events[4])
                self.buffered_trace.write_text("".join(json.dumps(event) + "\n" for event in events))
                output = self.root / mutate.__name__
                result = self.run_runner(output=output, buffered=True)
                self.assertNotEqual(result.returncode, 0)
                manifest = json.loads((output / "result.json").read_text())
                self.assertEqual(manifest["failed_stage"], "trace_validation")
                self.assertFalse((output / "replay.log").exists())
                self.assertFalse((output / "packet.txt").exists())

    def test_buffered_truncated_trace_and_image_difference_fail(self):
        self.buffered_trace.write_text("{}\n")
        result = self.run_runner(buffered=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.manifest()["failed_stage"], "trace_validation")
        self.buffered_trace.write_text("".join(json.dumps(event) + "\n" for event in buffered_fixture()))
        output = self.root / "mismatched"
        result = self.run_runner(buffered=True, output=output, extra_env={"FAKE_DIFFER": "capture"})
        self.assertNotEqual(result.returncode, 0)
        manifest = json.loads((output / "result.json").read_text())
        self.assertEqual(manifest["failed_stage"], "compare")
        self.assertNotEqual(manifest["sha256"]["baseline_ppm"], manifest["sha256"]["captured_ppm"])

    def test_failure_and_partial_output_are_reported(self):
        result = self.run_runner(extra_env={"FAKE_FAIL": "capture"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("capture", result.stderr)
        self.assertIn("status 37", result.stderr)
        self.assertTrue((self.output / "baseline.ppm").exists())
        self.assertFalse((self.output / "replay.log").exists())
        self.assertEqual(self.manifest()["failed_stage"], "capture")
        self.assertEqual(self.manifest()["status"], "FAIL")
        self.assertIn("exit_status=37", (self.output / "capture.log").read_text())

    def test_timeout_records_partial_output(self):
        result = self.run_runner(extra_env={"FAKE_TIMEOUT": "capture"}, timeout="0.5")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("timed out", result.stderr)
        self.assertEqual(self.manifest()["failed_stage"], "capture")
        self.assertIn("timeout_seconds=0.5", (self.output / "capture.log").read_text())

    def test_mismatch_never_reports_pass(self):
        result = self.run_runner(extra_env={"FAKE_DIFFER": "replay"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("differ byte for byte", result.stderr)
        manifest = self.manifest()
        self.assertEqual(manifest["failed_stage"], "compare")
        self.assertNotEqual(manifest["sha256"]["baseline_ppm"], manifest["sha256"]["replay_ppm"])

    def test_conversion_failure_is_reported(self):
        self.trace.write_text("{}\n")
        result = self.run_runner()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.manifest()["failed_stage"], "convert")
        self.assertTrue((self.output / "capture.jsonl").exists())
        self.assertFalse((self.output / "replay.log").exists())

    def test_invalid_paths_and_bottle_are_rejected(self):
        for kwargs in ({"engine": "relative"}, {"output": "relative"}, {"timeout": "nan"},
                       {"output": self.root / "bad:name"}, {"output": self.bottle / "new"},
                       {"output": self.engine / "new"}, {"output": self.build / "new"}):
            with self.subTest(kwargs=kwargs):
                result = self.run_runner(**kwargs)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.output.exists())
        drive = self.bottle / "dosdevices/z:"
        drive.unlink()
        drive.symlink_to(self.root)
        result = self.run_runner()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("map Z:", result.stderr)
        self.assertFalse(self.output.exists())

    def test_existing_and_dangling_output_never_overwritten(self):
        self.output.mkdir()
        marker = self.output / "keep"
        marker.write_text("unchanged")
        result = self.run_runner()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(marker.read_text(), "unchanged")
        self.assertEqual(list(self.output.iterdir()), [marker])
        marker.unlink()
        self.output.rmdir()
        self.output.symlink_to(self.root / "missing")
        result = self.run_runner()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "missing").exists())

    def test_rejects_wrong_architecture_kind_and_truncated_binary(self):
        binaries = [("d3d8_up_fixture/t3render_d3d8_up_fixture.exe", {"machine": 0x8664}),
                    ("t3render_d3d12_replay_up.exe", {"magic": 0x20b}), ("d3d8.dll", {"dll": False})]
        for name, kwargs in binaries:
            with self.subTest(name=name):
                binary = self.build / name
                good = binary.read_bytes()
                write_pe(binary, **kwargs)
                result = self.run_runner()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("PE32", result.stderr)
                self.assertFalse(self.output.exists())
                binary.write_bytes(good)
        (self.build / "d3d8.dll").write_bytes(b"MZ")
        result = self.run_runner()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
