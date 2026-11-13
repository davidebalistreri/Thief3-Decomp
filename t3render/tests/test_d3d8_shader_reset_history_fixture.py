"""Host-only tests for the isolated pre-Reset hypothesis comparison."""
import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools")); sys.path.insert(0, str(ROOT / "tests"))
import run_d3d8_shader_reset_history_fixture as runner
import test_d3d8_shader_reset_runner as fixture
from test_run_synthetic_up_replay import write_pe


def history_trace(scenario="valid-only", *, fallback=False, bad_reset_succeeded=False):
    records = fixture.trace_records(scenario, fallback=fallback, bad_hr=0 if bad_reset_succeeded else -2005530516)
    records[0]["declaration_history"] = {"requested": True, "words_per_observation": 1024,
                                         "words_per_frame": 262144, "words_per_device": 8192}
    if scenario == "valid-only" or (scenario == "recovery" and bad_reset_succeeded):
        for record in records:
            if record.get("event") == "draw" and record["frame"] > 0 and not (fallback and record["frame"] == 1):
                record["vertex_declaration_pre_reset"] = {"status": "hypothesis",
                    "source": "application_memory_before_reset", "generation": 1,
                    "reset_epoch": 100 * record["frame"], "reset_hr": 0,
                    "rebind_epoch": 100 * record["frame"] + 1, "words": runner.base.WORDS[:]}
    return records


def program_history_trace(scenario="valid-only"):
    records = history_trace(scenario)
    records[0]["vertex_program_capture"] = {"requested": True, "words_per_observation": 1024, "words_per_frame": 262144}
    for record in records:
        if record.get("event") != "draw":
            continue
        if record["frame"] == 0:
            record["vertex_program"] = {"status": "candidate", "version": "vs_1_1", "generation": 1,
                                        "words": runner.base.PROGRAM[:]}
        if "vertex_declaration_pre_reset" in record:
            record["vertex_program_pre_reset"] = {**record["vertex_declaration_pre_reset"],
                                                   "words": runner.base.PROGRAM[:]}
    return records


FAKE_WINE = r'''import os, pathlib, shutil, sys
args=sys.argv[1:]
def host(value):
    assert value.startswith('Z:')
    return pathlib.Path(value[2:].replace('\\','/'))
def wine(path): return 'Z:' + str(path.resolve()).replace('/','\\')
fixture_root=pathlib.Path(os.environ['FAKE_RESET_ROOT'])
scenario=args[args.index('--scenario')+1]
proxy='T3RENDER_CAPTURE_PATH' in os.environ
history=os.environ.get('T3RENDER_CAPTURE_DECLARATION_HISTORY')=='1'
declarations=os.environ.get('T3RENDER_CAPTURE_DECLARATIONS')=='1'
mode='history' if history else 'on' if declarations else 'off'
if proxy:
    loaded=wine(pathlib.Path.cwd()/'d3d8.dll'); kind='native'
else:
    loaded=r'C:\windows\system32\d3d8.dll'; kind='builtin'
print('0024:trace:loaddll:build_module Loaded L"'+loaded.replace('\\','\\\\')+'" at 12345678: '+kind)
print((fixture_root/(scenario+'.log')).read_text())
for phase in range(3):
    out=host(args[args.index('--out'+str(phase))+1]); assert not out.exists()
    shutil.copyfile(fixture_root/'image.ppm',out)
if proxy:
    out=host(os.environ['T3RENDER_CAPTURE_PATH']); assert not out.exists()
    shutil.copyfile(fixture_root/(scenario+'_'+mode+'.jsonl'),out)
'''


class ShaderResetHistoryFixtureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3-history-fixture-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "trace.jsonl"

    def observation(self, scenario="valid-only", fallback=False, bad_hr="0x8876086c"):
        self.path.write_text(fixture.log_text(fixture.events(scenario, fallback=fallback, bad_hr=bad_hr)), encoding="utf-8")
        return runner.base.inspect_log(self.path, scenario)

    def validate(self, records, observation, scenario, program_capture=False):
        self.path.write_text("".join(json.dumps(record) + "\n" for record in records), encoding="utf-8")
        return runner.inspect_history(self.path, observation, scenario, program_capture)

    def test_hypotheses_after_success_remain_separate_from_standard_observation(self):
        observation = self.observation()
        result = self.validate(history_trace(), observation, "valid-only")
        self.assertEqual(result, {"hypotheses": 2, "epochs": [(100, 101), (200, 201)]})

    def test_no_hypothesis_after_failed_reset_and_recovery(self):
        observation = self.observation("recovery")
        records = history_trace("recovery")
        self.assertEqual(self.validate(records, observation, "recovery")["hypotheses"], 0)
        draw = next(item for item in history_trace() if item.get("event") == "draw" and item.get("frame") == 1)
        target = next(item for item in records if item.get("event") == "draw" and item.get("frame") == 1)
        target["vertex_declaration_pre_reset"] = draw["vertex_declaration_pre_reset"]
        with self.assertRaises(runner.RunError): self.validate(records, observation, "recovery")

    def test_program_history_is_paired_and_does_not_become_a_candidate(self):
        observation = self.observation()
        records = program_history_trace()
        self.assertEqual(self.validate(records, observation, "valid-only", True)["hypotheses"], 2)
        for mode in ("missing", "bad_words", "bad_epoch", "candidate"):
            changed = copy.deepcopy(records)
            draw = next(x for x in changed if x.get("event") == "draw" and x["frame"] == 1)
            if mode == "missing": del draw["vertex_program_pre_reset"]
            if mode == "bad_words": draw["vertex_program_pre_reset"]["words"][1] = 0
            if mode == "bad_epoch": draw["vertex_program_pre_reset"]["reset_epoch"] += 1
            if mode == "candidate": draw["vertex_program"] = copy.deepcopy(records[4]["vertex_program"])
            with self.subTest(mode=mode), self.assertRaises(runner.RunError):
                self.validate(changed, observation, "valid-only", True)
        recovery = program_history_trace("recovery")
        self.assertEqual(self.validate(recovery, self.observation("recovery"), "recovery", True)["hypotheses"], 0)

    def test_no_hypothesis_when_original_shader_not_rebound(self):
        observation = self.observation(fallback=True)
        self.assertTrue(observation["partial"])
        self.assertEqual(self.validate(history_trace(fallback=True), observation, "valid-only")["hypotheses"], 1)

    def test_unexpected_success_is_partial_and_can_retain_hypothesis(self):
        observation = self.observation("recovery", bad_hr="0x00000000")
        self.assertTrue(observation["partial"])
        records = history_trace("recovery", bad_reset_succeeded=True)
        self.assertEqual(self.validate(records, observation, "recovery")["hypotheses"], 2)

    def test_rejects_forged_epochs_capabilities_and_words(self):
        observation = self.observation()
        for case in ("missing", "source", "generation", "boolean_generation", "reset_failure", "rebind_order",
                     "word_change", "epoch_reuse", "capability", "stable", "before_reset"):
            records = history_trace()
            draws = [item for item in records if item.get("event") == "draw"]
            history = draws[1]["vertex_declaration_pre_reset"]
            if case == "missing": del draws[1]["vertex_declaration_pre_reset"]
            if case == "source": history["source"] = "driver_state"
            if case == "generation": history["generation"] = 2
            if case == "boolean_generation": history["generation"] = True
            if case == "reset_failure": history["reset_hr"] = 0x80004005
            if case == "rebind_order": history["rebind_epoch"] = history["reset_epoch"]
            if case == "word_change": history["words"][1] = 0
            if case == "epoch_reuse": draws[2]["vertex_declaration_pre_reset"]["reset_epoch"] = 101
            if case == "capability": records[0]["declaration_history"]["words_per_device"] = 1
            if case == "stable": next(item for item in records if item.get("event") == "draw_result" and item["frame"] == 1)["shader_stable"] = False
            if case == "before_reset": draws[0]["vertex_declaration_pre_reset"] = copy.deepcopy(history)
            with self.subTest(case=case), self.assertRaises((runner.RunError, runner.trace_to_ir.trace_summary.TraceError)):
                self.validate(records, observation, "valid-only")

    def test_end_to_end_uses_distinct_modes_and_retains_hypotheses(self):
        root = Path(self.temp.name)
        engine, bottle, binaries, templates = (root / name for name in ("engine", "bottle", "binaries", "templates"))
        wine_dir = engine / "engine/bin"; wine_dir.mkdir(parents=True); (engine / "frameworks").mkdir()
        wine = wine_dir / "wine"; wine.write_text("#!" + sys.executable + "\n" + FAKE_WINE); wine.chmod(0o755)
        server = wine_dir / "wineserver"; server.write_text("#!/bin/sh\nexit 0\n"); server.chmod(0o755)
        (bottle / "dosdevices").mkdir(parents=True); (bottle / "dosdevices/z:").symlink_to("/")
        fixture_exe, proxy = binaries / "exe/fixture.exe", binaries / "dll/d3d8.dll"
        write_pe(fixture_exe); write_pe(proxy, dll=True)
        templates.mkdir()
        for scenario in runner.base.SCENARIOS:
            (templates / f"{scenario}.log").write_text(fixture.log_text(fixture.events(scenario)))
            for mode, records in (("off", fixture.trace_records(scenario, enabled=False)),
                                  ("on", fixture.trace_records(scenario, enabled=True)),
                                  ("history", history_trace(scenario))):
                (templates / f"{scenario}_{mode}.jsonl").write_text("".join(json.dumps(item) + "\n" for item in records))
        pixels = bytearray(256 * 256 * 3)
        for y in range(80, 180):
            for x in range(80, 180): pixels[(y * 256 + x) * 3:(y * 256 + x) * 3 + 3] = b"\0\xff\0"
        (templates / "image.ppm").write_bytes(b"P6\n256 256\n255\n" + pixels)
        with mock.patch.dict(os.environ, {"FAKE_RESET_ROOT": str(templates)}, clear=False):
            result = runner.execute(str(engine), str(bottle), str(fixture_exe), str(proxy), str(root / "output"), 10)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["scenarios"]["valid-only"]["history"]["hypotheses"], 2)
        self.assertEqual(result["scenarios"]["recovery"]["history"]["hypotheses"], 0)


if __name__ == "__main__": unittest.main()
