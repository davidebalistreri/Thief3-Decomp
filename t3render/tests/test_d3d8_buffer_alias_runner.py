"""Host-only alias-fixture validator and orchestration tests; no Wine or TDS."""
import copy
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import run_d3d8_buffer_alias_fixture as runner
from test_d3d8_stateblock_runner import trace_fixture
from test_run_synthetic_up_replay import write_pe


def log_records(status="exercised", process_hr="0x00000000"):
    acquire = {"event": "acquire", "stage": "texture_get_device", "hr": "0x00000000", "nonnull": True}
    if status == "acquisition_failed":
        acquire.update(hr="0x80004002", nonnull=False)
        return [acquire, {"event": "end", "status": status}]
    alias = {"event": "alias", "same_interface": True, "same_iunknown": True, "same_vtable": True, "vtable_unchanged": True,
             "roundtrip_hr": "0x00000000", "roundtrip_same": True}
    if status == "unsafe_alias":
        alias["same_interface"] = False
        return [acquire, alias, {"event": "end", "status": status}]
    calls = [{"event": "call", "name": name, "hr": process_hr if name == "process_vertices" else "0x00000000"}
             for name in runner.CALLS]
    calls.insert(4, {"event": "binding_identity", "vertex_same": True, "index_same": True})
    return [acquire, alias, *calls, {"event": "end", "status": status}]


def frame_records(exercised=True):
    records = trace_fixture()
    draw, result = records[4], records[5]
    vertices = b"".join(struct.pack("<4fI", *v) for v in (
        (25.6, 217.6, 0, 1, 0xffff0000), (128, 25.6, 0, 1, 0xffff0000),
        (230.4, 217.6, 0, 1, 0xffff0000)))
    draw["buffer_data"]["vertex"].update(status="captured", data=vertices.hex())
    draw["buffer_data"]["index"].update(status="captured", data=struct.pack("<3H", 0, 1, 2).hex())
    result["buffer_stable"] = True
    records[-2].update(had_reset=False, concurrent_operations=False)
    if exercised:
        clear = copy.deepcopy(records[2]); clear["op_id"] = 3
        clear_result = copy.deepcopy(records[3]); clear_result["op_id"] = 3
        again = copy.deepcopy(draw); again["op_id"] = 4
        again_result = copy.deepcopy(result); again_result["op_id"] = 4
        records[6:6] = [clear, clear_result, again, again_result]
        records[-2]["draws"] = 2
    return records


FAKE_WINE = r'''import json, os, pathlib, shutil, sys
args=sys.argv[1:]
assert args[1]=='--case' and args[3]=='--output'
def host(value):
    assert value.startswith('Z:')
    return pathlib.Path(value[2:].replace('\\','/'))
def wine(value): return 'Z:' + str(value.resolve()).replace('/','\\')
case=args[2]
mode='proxy' if 'T3RENDER_CAPTURE_PATH' in os.environ else 'baseline'
status=os.environ.get('FAKE_STATUS','exercised')
hr='0x80004002' if status=='acquisition_failed' else '0x00000000'
acq={'event':'acquire','stage':'texture_get_device','hr':hr,'nonnull':status!='acquisition_failed'}
def emit(value): print('alias-json '+json.dumps(value,separators=(',',':')))
if mode=='baseline':
    load=r'C:\windows\system32\d3d8.dll'; kind='builtin'
else:
    load=wine(pathlib.Path.cwd()/'d3d8.dll'); kind='native'
quoted=load.replace('\\','\\\\')
print('0024:trace:loaddll:build_module Loaded L"'+quoted+'" at 12345678: '+kind)
emit(acq)
if status!='acquisition_failed':
    emit({'event':'alias','same_interface':status=='exercised','same_iunknown':True,'same_vtable':True,'vtable_unchanged':True,
          'roundtrip_hr':'0x00000000','roundtrip_same':True})
if status=='exercised':
    names=('set_stream','set_indices','get_stream','get_indices','process_input','process_target','process_fvf',
           'process_stream','process_vertices','restore_fvf','restore_stream','clear_alias','alias_draw')
    for i,name in enumerate(names):
        if i==4: emit({'event':'binding_identity','vertex_same':True,'index_same':True})
        value='0x80004005' if name=='process_vertices' and os.environ.get('FAKE_PROCESS_FAIL') else '0x00000000'
        emit({'event':'call','name':name,'hr':value})
emit({'event':'end','status':status})
ppm=bytearray(b'P6\n256 256\n255\n'+bytes(256*256*3)); offset=len(b'P6\n256 256\n255\n')
for y in range(80,180):
    for x in range(80,180): ppm[offset+(y*256+x)*3:offset+(y*256+x)*3+3]=b'\xff\x00\x00'
destination=host(args[-1]); assert not destination.exists(); destination.write_bytes(ppm)
if mode=='proxy':
    source=os.environ['FAKE_TRACE_EXERCISED' if status=='exercised' else 'FAKE_TRACE_PARTIAL']
    shutil.copyfile(source,host(os.environ['T3RENDER_CAPTURE_PATH']))
'''


class BufferAliasRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-buffer-alias-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root / "fixture.log"

    def log(self, records):
        self.path.write_text("noise\n" + "".join("alias-json " + json.dumps(r) + "\n" for r in records))
        return runner.inspect_log(self.path)

    def trace(self, records, exercised=True):
        self.path.write_text("".join(json.dumps(r) + "\n" for r in records))
        return runner.inspect_trace(self.path, exercised)

    def test_log_statuses_are_explicit_and_process_failure_is_not_success(self):
        for status in ("exercised", "acquisition_failed", "unsafe_alias"):
            with self.subTest(status=status): self.assertEqual(self.log(log_records(status))["status"], status)
        result = self.log(log_records(process_hr="0x80004005"))
        self.assertEqual(next(v["hr"] for v in result["calls"] if v["name"] == "process_vertices"), "0x80004005")

    def test_all_runner_cases_have_fixture_paths(self):
        source = (ROOT / "tests/d3d8_buffer_alias_fixture.cpp").read_text()
        self.assertEqual(len(runner.CASES), len(set(runner.CASES)))
        for case in runner.CASES:
            with self.subTest(case=case): self.assertIn(f'L"{case}"', source)

    def test_rejects_forged_or_incomplete_alias_evidence(self):
        mutations = [
            lambda r: r.pop(), lambda r: r[0].update(nonnull=False),
            lambda r: r[1].update(same_interface=False), lambda r: r[1].update(vtable_unchanged=False),
            lambda r: r[1].update(roundtrip_hr="0x80004002"),
            lambda r: r[2].update(name="set_indices"), lambda r: r[2].update(hr="0x80004005"),
            lambda r: r[6].update(vertex_same=1), lambda r: r.pop(8),
        ]
        for mutate in mutations:
            records = log_records(); mutate(records)
            with self.subTest(mutate=mutate), self.assertRaises(runner.RunError): self.log(records)
        bad = log_records("unsafe_alias"); bad.insert(2, {"event": "call", "name": "set_stream", "hr": "0x00000000"})
        with self.assertRaises(runner.RunError): self.log(bad)
        self.path.write_text('alias-json {"event":"acquire","event":"acquire"}\n')
        with self.assertRaises(runner.RunError): runner.inspect_log(self.path)

    def test_control_trace_requires_real_stable_buffer_observation(self):
        self.assertEqual(self.trace(frame_records())["draws"], 2)
        self.assertEqual(self.trace(frame_records(False), False)["draws"], 1)
        exact_budget = frame_records()
        exact_budget[0]["buffer_capture"].update(component_limit_bytes=60, frame_limit_bytes=132)
        self.assertTrue(self.trace(exact_budget)["alias_buffer_stable"])
        for mutate in (
            lambda r: r[0]["buffer_capture"].update(requested=False),
            lambda r: r[0]["buffer_capture"].update(encoding="base64"),
            lambda r: r[0]["buffer_capture"].update(component_limit_bytes=59),
            lambda r: r[0]["buffer_capture"].update(component_limit_bytes=True),
            lambda r: r[0]["buffer_capture"].update(component_limit_bytes=60, frame_limit_bytes=131),
            lambda r: r[0]["buffer_capture"].update(frame_limit_bytes=8 * 1024 * 1024 + 1),
            lambda r: r[5].update(buffer_stable=False),
            lambda r: r[4]["buffer_data"]["vertex"].update(data="00" * 60),
            lambda r: r[4]["buffer_data"]["index"].update(index_format=102),
            lambda r: r[4]["streams"][0]["description"].update(usage=0),
            lambda r: r[8]["streams"][0].update(buffer="0x999"),
            lambda r: r[9].update(buffer_stable=None),
            lambda r: r[9].update(buffer_stable=1),
            lambda r: r[8]["buffer_data"]["vertex"].update(data="00" * 60),
            lambda r: r[8]["buffer_data"]["index"].update(status="window_unavailable", data=None),
            lambda r: r[6].update(color=0), lambda r: r[7].update(hr=-1),
            lambda r: r.pop(6),
            lambda r: r[-2].update(draws=True),
        ):
            records = frame_records(); mutate(records)
            with self.subTest(mutate=mutate), self.assertRaises(runner.RunError): self.trace(records)

    def execute(self, status="exercised", process_fail=False, alias_stable=True):
        engine, bottle, build = (self.root / name for name in ("engine", "bottle", "build"))
        binary_dir = engine / "engine/bin"; binary_dir.mkdir(parents=True)
        (engine / "frameworks").mkdir()
        wine = binary_dir / "wine"; wine.write_text("#!" + sys.executable + "\n" + FAKE_WINE); wine.chmod(0o755)
        server = binary_dir / "wineserver"; server.write_text("#!/bin/sh\nexit 0\n"); server.chmod(0o755)
        (bottle / "dosdevices").mkdir(parents=True); (bottle / "dosdevices/z:").symlink_to("/")
        fixture, proxy = build / "exe/fixture.exe", build / "dll/d3d8.dll"
        write_pe(fixture); write_pe(proxy, dll=True)
        exercised = self.root / "exercised.jsonl"; partial = self.root / "partial.jsonl"
        exercised_records = frame_records()
        exercised_records[9]["buffer_stable"] = alias_stable
        exercised.write_text("".join(json.dumps(r) + "\n" for r in exercised_records))
        partial.write_text("".join(json.dumps(r) + "\n" for r in frame_records(False)))
        env = {"FAKE_STATUS": status, "FAKE_TRACE_EXERCISED": str(exercised), "FAKE_TRACE_PARTIAL": str(partial)}
        if process_fail: env["FAKE_PROCESS_FAIL"] = "1"
        with mock.patch.dict(os.environ, env, clear=False), mock.patch.object(runner, "CASES", ("texture_device",)):
            return runner.execute(str(engine), str(bottle), str(fixture), str(proxy), str(self.root / "output"), 10)

    def test_host_orchestration_compares_loader_trace_and_pixels(self):
        self.assertEqual(self.execute()["status"], "PASS")

    def test_unsupported_or_failed_process_is_partial(self):
        self.assertEqual(self.execute("unsafe_alias")["status"], "PARTIAL")

    def test_unsupported_interface_is_partial(self):
        self.assertEqual(self.execute("acquisition_failed")["status"], "PARTIAL")

    def test_unsupported_process_vertices_is_partial(self):
        self.assertEqual(self.execute(process_fail=True)["status"], "PARTIAL")

    def test_unstable_alias_draw_is_partial(self):
        self.assertEqual(self.execute(alias_stable=False)["status"], "PARTIAL")


class BufferAliasCompileTests(unittest.TestCase):
    def test_i686_fixture_cross_compiles_without_touching_game(self):
        import shutil
        import subprocess
        compiler = shutil.which("i686-w64-mingw32-g++")
        if not compiler: self.skipTest("i686 MinGW-w64 compiler required")
        with tempfile.TemporaryDirectory(prefix="t3render-alias-x86-") as directory:
            result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                     "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX",
                                     str(ROOT / "tests/d3d8_buffer_alias_fixture.cpp"), "-municode", "-static",
                                     "-static-libgcc", "-static-libstdc++", "-ld3d8", "-ldxguid", "-luser32",
                                     "-o", str(Path(directory) / "fixture.exe")],
                                    text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__": unittest.main()
