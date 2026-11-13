"""Fail-closed extraction of paired vertex shader constant samples."""
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

BITS = [0x3f800000, 0x80000000, 0x7fc00000, 0xffffffff, 0, 1, 2, 3]


def constant_events():
    records = events()
    records[0]["vertex_constant_capture"] = {"requested": True, "registers_per_observation": 2, "words_per_frame": 16}
    records[1]["caps"] = {"vertex_shader_constants": 2}
    records[2]["vertex_constants"] = {"status": "sampled", "start_register": 0, "count": 2,
                                       "getter_hr": 0, "verify_hr": 0, "bits": BITS[:]}
    return records


class ConstantTraceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="t3render-constants-ir-")
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "trace.jsonl"

    def commands(self, records):
        self.path.write_text("".join(json.dumps(item) + "\n" for item in records), encoding="utf-8")
        return trace_to_ir.convert(self.path)["frames"][0]["commands"]

    def test_preserves_raw_bits_only_for_a_paired_successful_draw(self):
        output = self.commands(constant_events())[0]["vertex_constants_observed"]
        self.assertEqual(output, {"status": "sampled", "shader_value": 0x44, "shader_stable": True,
                                  "draw_hresult": 0, "start_register": 0, "count": 2, "bits": BITS})

    def test_rejects_missing_capability_caps_failed_getters_or_draw(self):
        for mode in ("missing_header", "disabled", "decl_disabled", "bad_limit", "bad_frame_limit",
                     "missing_caps", "mismatched_caps", "failed_getter", "failed_verify", "failed_draw",
                     "unstable", "missing_stability", "bad_shader", "bad_count", "bad_start", "bad_bits"):
            r = constant_events(); cap = r[0]["vertex_constant_capture"]; raw = r[2]["vertex_constants"]
            if mode == "missing_header": del r[0]["vertex_constant_capture"]
            if mode == "disabled": cap["requested"] = False
            if mode == "decl_disabled": r[0]["declaration_capture"]["requested"] = False
            if mode == "bad_limit": cap["registers_per_observation"] = 97
            if mode == "bad_frame_limit": cap["words_per_frame"] = 262145
            if mode == "missing_caps": del r[1]["caps"]
            if mode == "mismatched_caps": r[1]["caps"]["vertex_shader_constants"] = 1
            if mode == "failed_getter": raw["getter_hr"] = -1
            if mode == "failed_verify": raw["verify_hr"] = 0x80004005
            if mode == "failed_draw": r[3]["hr"] = -1
            if mode == "unstable": r[3]["shader_stable"] = False
            if mode == "missing_stability": del r[3]["shader_stable"]
            if mode == "bad_shader": r[2]["vertex_shader"] = True
            if mode == "bad_count": raw["count"] = True
            if mode == "bad_start": raw["start_register"] = 1
            if mode == "bad_bits": raw["bits"][2] = True
            with self.subTest(mode=mode):
                self.assertNotIn("vertex_constants_observed", self.commands(r)[0])

    def test_frame_budget_counts_failed_unstable_and_malformed_later_draws(self):
        for mode in ("exact", "over", "failed", "unstable", "malformed"):
            r = constant_events(); r[0]["vertex_constant_capture"]["words_per_frame"] = 16 if mode == "exact" else 15
            second = copy.deepcopy(r[2]); second["op_id"] = 2
            result = copy.deepcopy(r[3]); result["op_id"] = 2
            if mode == "failed": result["hr"] = -1
            if mode == "unstable": result["shader_stable"] = False
            if mode == "malformed": second["vertex_constants"]["bits"] = "not measurable"
            r[2:4] = [r[2], second, result, r[3]]
            r[-2]["concurrent_operations"] = True
            with self.subTest(mode=mode):
                self.assertEqual(["vertex_constants_observed" in c for c in self.commands(r)],
                                 [True, True] if mode == "exact" else [False, False])

    def test_unavailable_reason_does_not_suppress_other_samples(self):
        r = constant_events(); second = copy.deepcopy(r[2]); second["op_id"] = 2
        second["vertex_constants"] = {"status": "unavailable", "reason": "getter_failed", "count": 2,
                                       "getter_hr": -1, "verify_hr": -1, "bits": None}
        result = copy.deepcopy(r[3]); result["op_id"] = 2
        r[2:4] = [r[2], second, result, r[3]]; r[-2]["concurrent_operations"] = True
        commands = self.commands(r)
        self.assertEqual(commands[0]["vertex_constants_observed"]["status"], "sampled")
        self.assertEqual(commands[1]["vertex_constants_observed"]["reason"], "getter_failed")
        inconsistent = copy.deepcopy(r)
        inconsistent[3]["vertex_constants"]["getter_hr"] = 0
        commands = self.commands(inconsistent)
        self.assertEqual(commands[0]["vertex_constants_observed"]["status"], "sampled")
        self.assertNotIn("vertex_constants_observed", commands[1])
        for bad in ([], {}, None, "invented"):
            changed = copy.deepcopy(r); changed[3]["vertex_constants"]["reason"] = bad
            with self.subTest(reason=bad):
                self.assertTrue(all("vertex_constants_observed" not in c for c in self.commands(changed)))

    def test_unavailable_reason_must_match_caps_and_getter_results(self):
        cases = (
            ("caps_unavailable", 0, -1, -1, 0),
            ("getter_failed", 2, -1, -1, 2),
            ("verification_failed", 2, 0, -1, 2),
            ("inconsistent_read", 2, 0, 0, 2),
            ("frame_word_limit", 2, 0, 0, 2),
            ("invalid_count", 3, -1, -1, 2),
        )
        for reason, count, first, second, caps in cases:
            r = constant_events(); r[1]["caps"]["vertex_shader_constants"] = caps
            r[2]["vertex_constants"] = {"status": "unavailable", "reason": reason, "count": count,
                                         "getter_hr": first, "verify_hr": second, "bits": None}
            with self.subTest(reason=reason):
                self.assertEqual(self.commands(r)[0]["vertex_constants_observed"]["reason"], reason)


if __name__ == "__main__":
    unittest.main()
