"""Fail-closed, operation-paired declaration candidates in the offline IR."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import trace_summary
import trace_to_ir
import test_trace_to_ir as buffered

HEADER = {"event": "header", "format": "t3render-d3d8-diagnostic", "version": 1,
          "operation_results": 1, "clear_rect_limit": 256, "declaration_capture": {"requested": True}}
DECLARATION = {"status": "candidate", "generation": 5, "words": [0x20000000, 0x40020000, 0xffffffff]}
DRAW = {"event": "draw", "frame": 0, "op_id": 1, "call": "DrawPrimitiveUP", "vertex_shader": 0x44,
        "vertex_declaration": DECLARATION}
RESULT = {"event": "draw_result", "frame": 0, "op_id": 1, "hr": 0, "shader_stable": True}
END = {"event": "frame_end", "frame": 0, "present_hr": 0, "concurrent_operations": False}


def events():
    return copy.deepcopy([HEADER, {"event": "frame_begin", "frame": 0}, DRAW, RESULT, END,
                          {"event": "capture_end", "frames": 1}])


class DeclarationTraceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="t3render-declaration-ir-")
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "trace.jsonl"

    def convert(self, records):
        self.path.write_text("".join(json.dumps(item) + "\n" for item in records), encoding="utf-8")
        return trace_to_ir.convert(self.path)

    def observation(self, records):
        return self.convert(records)["frames"][0]["commands"][0]["vertex_declaration_observed"]

    def test_stable_candidate_retains_shader_value_and_words(self):
        records = events()
        output = self.convert(records)
        command = output["frames"][0]["commands"][0]
        self.assertEqual(command["vertex_declaration_observed"], {
            "status": "candidate", "shader_value": 0x44, "generation": 5,
            "words": DECLARATION["words"], "shader_stable": True, "draw_hresult": 0})
        self.assertEqual(command["compatibility"]["d3d8"]["draw"], records[2])
        self.assertFalse(output["replayable"])

    def test_framing_skips_constant_and_extension_payload_end_words(self):
        words = [0x20000000, (4 << 29) | (2 << 25) | 3,
                 0xffffffff, 2, 3, 4, 5, 6, 0xffffffff, 8,
                 (5 << 29) | (3 << 24) | 7, 0xffffffff, 0x20000000, 0, 0xffffffff]
        records = events()
        records[2]["vertex_declaration"]["words"] = words
        self.assertEqual(self.observation(records)["words"], words)
        records[2]["vertex_declaration"]["words"] = [0] * 1023 + [0xffffffff]
        self.assertEqual(len(self.observation(records)["words"]), 1024)

    def test_requires_opt_in_success_stability_and_valid_shader_value(self):
        edits = ("disabled", "missing_capability", "invalid_capability", "unstable", "missing_stability",
                 "failed_draw_signed", "failed_draw_unsigned", "missing_shader", "null_shader",
                 "invalid_shader_bool", "invalid_shader_range")
        for edit in edits:
            records = events()
            if edit == "disabled": records[0]["declaration_capture"]["requested"] = False
            if edit == "missing_capability": del records[0]["declaration_capture"]
            if edit == "invalid_capability": records[0]["declaration_capture"]["requested"] = "true"
            if edit == "unstable": records[3]["shader_stable"] = False
            if edit == "missing_stability": del records[3]["shader_stable"]
            if edit == "failed_draw_signed": records[3]["hr"] = -2147467259
            if edit == "failed_draw_unsigned": records[3]["hr"] = 0x80004005
            if edit == "missing_shader": del records[2]["vertex_shader"]
            if edit == "null_shader": records[2]["vertex_shader"] = None
            if edit == "invalid_shader_bool": records[2]["vertex_shader"] = True
            if edit == "invalid_shader_range": records[2]["vertex_shader"] = 1 << 32
            with self.subTest(edit=edit):
                observed = self.observation(records)
                self.assertEqual(observed["status"], "unknown")
                self.assertIsNone(observed["words"])
                self.assertIsNone(observed["generation"])
                self.assertEqual(self.convert(records)["frames"][0]["commands"][0]["compatibility"]["d3d8"]["draw"], records[2])

    def test_rejects_malformed_candidates_without_fabricating_words(self):
        invalid_generations = (None, 0, -1, 1 << 64, True, "5")
        for value in invalid_generations:
            records = events(); records[2]["vertex_declaration"]["generation"] = value
            with self.subTest(generation=value): self.assertIsNone(self.observation(records)["words"])
        invalid_words = (None, [], [0] * 1024 + [0xffffffff], [0x20000000], [0xffffffff, 0],
                         [(4 << 29) | (1 << 25), 0xffffffff], [(5 << 29) | (2 << 24), 1, 0xffffffff],
                         [1, 0xffffffff], [6 << 29, 0xffffffff], [True, 0xffffffff], [-1, 0xffffffff],
                         [1 << 32, 0xffffffff], ["0", 0xffffffff])
        for value in invalid_words:
            records = events(); records[2]["vertex_declaration"]["words"] = value
            with self.subTest(words=value):
                observed = self.observation(records)
                self.assertEqual(observed["status"], "unknown")
                self.assertIsNone(observed["words"])
        for field in ("status", "generation", "words"):
            records = events(); del records[2]["vertex_declaration"][field]
            with self.subTest(field=field): self.assertIsNone(self.observation(records)["words"])

    def test_non_candidate_status_and_orphan_stability_remain_unknown_or_unavailable(self):
        for status in ("unknown", "unavailable", "unsafe"):
            records = events()
            records[2]["vertex_declaration"] = {"status": status, "generation": None, "words": None}
            with self.subTest(status=status):
                observed = self.observation(records)
                self.assertEqual(observed["status"], status)
                self.assertIsNone(observed["words"])
        records = events(); del records[2]["vertex_declaration"]
        observed = self.observation(records)
        self.assertEqual(observed["status"], "unknown")
        self.assertTrue(observed["shader_stable"])
        self.assertIsNone(observed["words"])

    def test_reordered_results_are_matched_by_operation_id(self):
        records = events()
        first = records[2]
        second = copy.deepcopy(first)
        second.update(op_id=2, call="DrawPrimitive", vertex_shader=0x123)
        second["vertex_declaration"].update(generation=7, words=[0x20000000, 0xffffffff])
        records[2:4] = [first, second, dict(RESULT, op_id=2, shader_stable=False), dict(RESULT)]
        records[-2]["concurrent_operations"] = True
        commands = self.convert(records)["frames"][0]["commands"]
        self.assertEqual(commands[0]["vertex_declaration_observed"]["generation"], 5)
        self.assertEqual(commands[0]["vertex_declaration_observed"]["words"], DECLARATION["words"])
        self.assertFalse(commands[1]["vertex_declaration_observed"]["shader_stable"])
        self.assertIsNone(commands[1]["vertex_declaration_observed"]["words"])

    def test_reused_operation_ids_in_distinct_frames_remain_separate(self):
        first = events()[:-1]
        second = events()[1:-1]
        for item in second: item["frame"] = 1
        second[1]["vertex_declaration"].update(generation=9, words=[0x20000000, 0xffffffff])
        records = first + second + [{"event": "capture_end", "frames": 2}]
        frames = self.convert(records)["frames"]
        self.assertEqual([frame["commands"][0]["vertex_declaration_observed"]["generation"] for frame in frames], [5, 9])

    def test_shader_and_buffer_stability_are_independent(self):
        for shader_stable, buffer_stable in ((True, False), (False, True)):
            records = copy.deepcopy(buffered.buffered_events())
            records[0]["declaration_capture"] = {"requested": True}
            records[2].update(vertex_shader=0x44, vertex_declaration=copy.deepcopy(DECLARATION))
            records[3].update(shader_stable=shader_stable, buffer_stable=buffer_stable)
            output = self.convert(records)
            declaration = output["frames"][0]["commands"][0]["vertex_declaration_observed"]
            vertex = output["meshes"][0]["buffered_data_observed"]["vertex"]
            with self.subTest(shader_stable=shader_stable, buffer_stable=buffer_stable):
                self.assertEqual(declaration["status"], "candidate" if shader_stable else "unknown")
                self.assertEqual(vertex["status"], "captured" if buffer_stable else "unknown")

    def test_missing_or_mismatched_result_cannot_expose_a_candidate(self):
        for mutation in ("missing", "wrong_id", "wrong_frame"):
            records = events()
            if mutation == "missing": del records[3]
            if mutation == "wrong_id": records[3]["op_id"] = 2
            if mutation == "wrong_frame": records[3]["frame"] = 1
            with self.subTest(mutation=mutation), self.assertRaises(trace_summary.TraceError):
                self.convert(records)

    def test_legacy_draws_do_not_gain_declaration_observations(self):
        records = events(); del records[0]["declaration_capture"]
        del records[2]["vertex_declaration"]; del records[3]["shader_stable"]
        command = self.convert(records)["frames"][0]["commands"][0]
        self.assertNotIn("vertex_declaration_observed", command)


if __name__ == "__main__":
    unittest.main()
