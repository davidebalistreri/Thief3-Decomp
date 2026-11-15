"""Candidate-only light correlation; no runtime light identity is invented."""
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools"))
import correlate_runtime_light_candidates as module


def observation(draw, section, position=(1.0, 2.0, 3.0), bits=(1, 2, 3), kind="omni"):
    return {"draw": draw, "section": section, "position": position, "position_bits": bits,
            "kind": kind, "program": "OmniLight11_Light_Vertex", "coefficient": .01}


class RuntimeLightCandidateTests(unittest.TestCase):
    def test_two_bed_sections_and_unique_hardpoint_produce_only_a_candidate(self):
        light = SimpleNamespace(id="light:lamp", position=(1.001, 2, 3), kind="omni")
        matches, rejected = module.correlate([light], [observation(10, 1), observation(11, 2)])
        self.assertEqual(len(matches), 1)
        self.assertEqual(matches[0]["offline_light_id"], "light:lamp")
        self.assertEqual(matches[0]["draws"], [10, 11])
        self.assertEqual(matches[0]["sections"], [1, 2])
        self.assertLess(matches[0]["distance_t3_units"], module.TOLERANCE_T3)
        self.assertNotIn("sampled_position_bits", matches[0])
        self.assertEqual(sum(rejected.values()), 0)

    def test_ambiguity_wrong_kind_single_section_or_unstable_constants_never_match(self):
        lamp = SimpleNamespace(id="light:lamp", position=(1, 2, 3), kind="omni")
        other = SimpleNamespace(id="light:other", position=(1.001, 2, 3), kind="omni")
        pair = [observation(10, 1), observation(11, 2)]
        matches, rejected = module.correlate([lamp, other], pair)
        self.assertEqual(matches, [])
        self.assertEqual(rejected["no_unique_position"], 2)
        wrong = SimpleNamespace(id="light:spot", position=(1, 2, 3), kind="spot")
        matches, rejected = module.correlate([wrong], pair)
        self.assertEqual(matches, [])
        self.assertEqual(rejected["kind_mismatch"], 2)
        self.assertEqual(module.correlate([lamp], [observation(10, 1), observation(11, 1)])[0], [])
        unstable = [observation(10, 1), observation(11, 2, bits=(9, 2, 3))]
        matches, rejected = module.correlate([lamp], unstable)
        self.assertEqual(matches, [])
        self.assertEqual(rejected["inconsistent_sample"], 1)

    def test_incomplete_frame_budget_or_wrong_kernel_fingerprint_cannot_validate(self):
        header = {"capture": {"words_per_frame": 4}}
        self.assertTrue(module.within_budget([1, 2], header, "capture", lambda draw, _: draw))
        self.assertFalse(module.within_budget([3, 2], header, "capture", lambda draw, _: draw))
        self.assertFalse(module.within_budget([1, None], header, "capture", lambda draw, _: draw))
        with tempfile.TemporaryDirectory(prefix="t3render-light-kernel-") as directory:
            path = Path(directory) / "kernel.ibt"
            path.write_bytes(b"not an IBT")
            with self.assertRaisesRegex(ValueError, "fingerprint"):
                module.shader_index(path, "a" * 64)


if __name__ == "__main__":
    unittest.main()
