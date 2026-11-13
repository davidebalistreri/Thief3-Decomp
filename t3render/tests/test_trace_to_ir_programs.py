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
from test_trace_to_ir_declarations import events

WORDS = [0xfffe0101, 0x0002fffe, 0x0000ffff, 0xfffe0101, 81, 0xa00f0000,
         0x0000ffff, 0, 0xffffffff, 0x3f800000, 1, 0xc00f0000, 0x90e40000, 0xffff]


def program_events():
    records = events()
    records[0]["vertex_program_capture"] = {"requested": True, "words_per_observation": 1024,
                                            "words_per_frame": 262144}
    records[2]["vertex_program"] = {"status": "candidate", "version": "vs_1_1", "generation": 5,
                                     "words": WORDS[:]}
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


if __name__ == "__main__":
    unittest.main()
