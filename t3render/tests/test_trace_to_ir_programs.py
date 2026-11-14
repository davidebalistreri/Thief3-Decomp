"""Fail-closed extraction of paired vertex-program observations."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import trace_to_ir
from test_trace_to_ir_declarations import events, history_events

WORDS = [0xfffe0101, 0x0002fffe, 0x0000ffff, 0xfffe0101, 81, 0xa00f0000,
         0x0000ffff, 0, 0xffffffff, 0x3f800000, 1, 0xc00f0000, 0x90e40000, 0xffff]


def program_events():
    records = events()
    records[0]["vertex_program_capture"] = {"requested": True, "words_per_observation": 1024,
                                            "words_per_frame": 262144}
    records[2]["vertex_program"] = {"status": "candidate", "version": "vs_1_1", "generation": 5,
                                     "words": WORDS[:]}
    return records


def program_history_events():
    records = history_events()
    records[0]["vertex_program_capture"] = {"requested": True, "words_per_observation": 1024,
                                            "words_per_frame": 262144}
    records[2]["vertex_program_pre_reset"] = {"status": "hypothesis", "source": "application_memory_before_reset",
        "generation": 5, "reset_epoch": 1, "reset_hr": 0, "rebind_epoch": 2, "words": WORDS[:]}
    return records


class ProgramTraceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="t3render-program-ir-")
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "trace.jsonl"

    def commands(self, records):
        self.path.write_text("".join(json.dumps(item) + "\n" for item in records), encoding="utf-8")
        return trace_to_ir.convert(self.path)["frames"][0]["commands"]

    def test_paired_candidate_skips_comment_and_immediates(self):
        program = self.commands(program_events())[0]["vertex_program_observed"]
        self.assertEqual(program, {"status": "candidate", "version": "vs_1_1", "generation": 5,
                                   "words": WORDS, "shader_value": 0x44, "shader_stable": True, "draw_hresult": 0})

    def test_vertex_10_requires_matching_version_label(self):
        records = program_events()
        records[2]["vertex_program"].update(version="vs_1_0", words=[0xfffe0100, *WORDS[1:]])
        program = self.commands(records)[0]["vertex_program_observed"]
        self.assertEqual((program["version"], program["words"][0]), ("vs_1_0", 0xfffe0100))
        records[2]["vertex_program"]["version"] = "vs_1_1"
        self.assertNotIn("vertex_program_observed", self.commands(records)[0])

    def test_capability_and_result_are_required(self):
        for mode in ("missing", "disabled", "decl_disabled", "too_large", "too_small", "bad_frame_limit",
                     "failed", "unstable", "missing_stability", "generation_mismatch", "version"):
            records = program_events()
            cap = records[0]["vertex_program_capture"]
            if mode == "missing": del records[0]["vertex_program_capture"]
            if mode == "disabled": cap["requested"] = False
            if mode == "decl_disabled": records[0]["declaration_capture"]["requested"] = False
            if mode == "too_large": cap["words_per_observation"] = 1025
            if mode == "too_small": cap["words_per_observation"] = len(WORDS) - 1
            if mode == "bad_frame_limit": cap["words_per_frame"] = 262145
            if mode == "failed": records[3]["hr"] = -1
            if mode == "unstable": records[3]["shader_stable"] = False
            if mode == "missing_stability": del records[3]["shader_stable"]
            if mode == "generation_mismatch": records[2]["vertex_program"]["generation"] = 6
            if mode == "version": records[2]["vertex_program"]["version"] = "vs_2_0"
            with self.subTest(mode=mode):
                self.assertNotIn("vertex_program_observed", self.commands(records)[0])

    def test_malformed_token_framing_is_never_published(self):
        invalid = ([], [0xfffe0200, 0xffff], [0xfffe0101, 0xffff, 0],
                   [0xfffe0101, 1, 0xffff], [0xfffe0101, 0x0002fffe, 0xffff],
                   [0xfffe0101, 81, 0xa00f0000, 0xffff], [0xfffe0101, 0x10000001, 0xffff],
                   [0xfffe0101, True, 0xffff], [0xfffe0101] + [0] * 1024 + [0xffff])
        for words in invalid:
            records = program_events(); records[2]["vertex_program"]["words"] = words
            with self.subTest(words=str(words)[:70]):
                self.assertNotIn("vertex_program_observed", self.commands(records)[0])

    def test_frame_budget_includes_unstable_and_malformed_later_draws(self):
        for mode in ("exact", "over", "failed", "unstable", "malformed"):
            records = program_events()
            records[0]["vertex_program_capture"].update(words_per_observation=len(WORDS),
                                                        words_per_frame=2 * len(WORDS) if mode == "exact" else 2 * len(WORDS) - 1)
            second = copy.deepcopy(records[2]); second["op_id"] = 2
            result = copy.deepcopy(records[3]); result["op_id"] = 2
            if mode == "failed": result["hr"] = -1
            if mode == "unstable": result["shader_stable"] = False
            if mode == "malformed": second["vertex_program"]["words"] = "unmeasurable"
            records[2:4] = [records[2], second, result, records[3]]
            records[-2]["concurrent_operations"] = True
            with self.subTest(mode=mode):
                commands = self.commands(records)
                self.assertEqual(["vertex_program_observed" in c for c in commands],
                                 [True, True] if mode == "exact" else [False, False])

    def test_unavailable_reason_is_paired_and_does_not_invalidate_other_programs(self):
        records = program_events()
        second = copy.deepcopy(records[2]); second["op_id"] = 2
        second["vertex_program"] = {"status": "unavailable", "generation": 5,
                                    "reason": "unsupported_version", "words": None}
        result = copy.deepcopy(records[3]); result["op_id"] = 2
        records[2:4] = [records[2], second, result, records[3]]
        records[-2]["concurrent_operations"] = True
        commands = self.commands(records)
        self.assertEqual(commands[0]["vertex_program_observed"]["status"], "candidate")
        self.assertEqual(commands[1]["vertex_program_observed"]["reason"], "unsupported_version")
        self.assertIsNone(commands[1]["vertex_program_observed"]["words"])
        for bad in ([], {}, None, "unknown"):
            changed = copy.deepcopy(records); changed[3]["vertex_program"]["reason"] = bad
            with self.subTest(reason=bad):
                self.assertTrue(all("vertex_program_observed" not in c for c in self.commands(changed)))

    def test_pre_reset_program_is_only_a_matching_hypothesis(self):
        command = self.commands(program_history_events())[0]
        self.assertNotIn("vertex_program_observed", command)
        self.assertEqual(command["vertex_program_pre_reset"]["status"], "hypothesis")
        self.assertEqual(command["vertex_program_pre_reset"]["words"], WORDS)
        for mode in ("no_decl_history", "decl_mismatch", "generation", "reset_epoch", "reset_hr", "rebind_epoch",
                     "source", "status", "bad_words", "unstable", "failed", "no_program_capability"):
            records = program_history_events()
            raw = records[2]["vertex_program_pre_reset"]
            if mode == "no_decl_history": del records[2]["vertex_declaration_pre_reset"]
            if mode == "decl_mismatch": records[2]["vertex_declaration_pre_reset"]["generation"] = 6
            if mode == "generation": raw["generation"] = 6
            if mode == "reset_epoch": raw["reset_epoch"] = 2
            if mode == "reset_hr": raw["reset_hr"] = 1
            if mode == "rebind_epoch": raw["rebind_epoch"] = 3
            if mode == "source": raw["source"] = "driver"
            if mode == "status": raw["status"] = "candidate"
            if mode == "bad_words": raw["words"] = [0xfffe0101, 1, 0xffff]
            if mode == "unstable": records[3]["shader_stable"] = False
            if mode == "failed": records[3]["hr"] = -1
            if mode == "no_program_capability": del records[0]["vertex_program_capture"]
            with self.subTest(mode=mode):
                command = self.commands(records)[0]
                self.assertNotIn("vertex_program_pre_reset", command)
                self.assertNotIn("vertex_program_observed", command)

    def test_pre_reset_vertex_10_remains_a_hypothesis(self):
        records = program_history_events()
        records[2]["vertex_program_pre_reset"]["words"][0] = 0xfffe0100
        command = self.commands(records)[0]
        self.assertEqual(command["vertex_program_pre_reset"]["status"], "hypothesis")
        self.assertEqual(command["vertex_program_pre_reset"]["words"][0], 0xfffe0100)
        self.assertNotIn("vertex_program_observed", command)

    def test_pre_reset_program_words_share_frame_budget(self):
        records = program_history_events()
        records[0]["vertex_program_capture"].update(words_per_observation=len(WORDS), words_per_frame=2 * len(WORDS) - 1)
        second = copy.deepcopy(records[2]); second["op_id"] = 2
        result = copy.deepcopy(records[3]); result.update(op_id=2, shader_stable=False)
        records[2:4] = [records[2], second, result, records[3]]
        records[-2]["concurrent_operations"] = True
        self.assertTrue(all("vertex_program_pre_reset" not in command for command in self.commands(records)))


if __name__ == "__main__":
    unittest.main()
