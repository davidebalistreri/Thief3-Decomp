"""Pre-Reset ambient programs remain optional hypotheses, never runtime lights."""
import struct
import sys
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import scene_ambient_candidates as ambient


def bits(values):
    return list(struct.unpack(f"<{len(values)}I", struct.pack(f"<{len(values)}f", *values)))


class AmbientCandidateTests(unittest.TestCase):
    def sample(self, color=(.1, .2, .3, 1), base=(0, .5, 1, 2)):
        words = [0xfffe0100, 0x0000ffff]
        data = bits(base) + [0] * (17 * 4) + bits(color)
        return {"status": "sampled", "count": 19, "bits": data}, {"words": words}, struct.pack("<2I", *words)

    def test_requires_unique_program_and_valid_sampled_color(self):
        constants, program, binary = self.sample()
        with mock.patch.object(ambient.trace_to_ir, "observed_vertex_constants", return_value=constants), \
             mock.patch.object(ambient.trace_to_ir, "pre_reset_vertex_program", return_value=program):
            args = ({}, {}, {}, {}, True, True, True)
            result = ambient.ambient_color(*args, {binary: ["Ambient11_Light_Vertex"]})
            self.assertAlmostEqual(result[0], .1)
            for index in ({}, {binary: ["other"]}, {binary: ["Ambient11_Light_Vertex"] * 2}):
                with self.subTest(index=index), self.assertRaisesRegex(ValueError, "program"):
                    ambient.ambient_color(*args, index)

    def test_unavailable_truncated_invalid_or_out_of_range_constants_fail(self):
        args = ({}, {}, {}, {}, True, True, True)
        constants, program, binary = self.sample()
        index = {binary: ["Ambient11_Light_Vertex"]}
        for sample in (None, {"status": "unavailable"}, dict(constants, count=18),
                       self.sample(color=(.1, float("nan"), .3, 1))[0],
                       self.sample(color=(.1, 1.1, .3, 1))[0], self.sample(base=(0, .5, .5, 2))[0]):
            with self.subTest(sample=sample), \
                 mock.patch.object(ambient.trace_to_ir, "observed_vertex_constants", return_value=sample), \
                 mock.patch.object(ambient.trace_to_ir, "pre_reset_vertex_program", return_value=program), \
                 self.assertRaises(ValueError):
                ambient.ambient_color(*args, index)

    def test_incomplete_frame_budget_cannot_be_used(self):
        scene = SimpleNamespace(source=SimpleNamespace(kind="runtime", camera_synchronized=True,
                                                       frame_id=7, fingerprints={"trace": "a" * 64}))
        records = [{"event": "header"}, {"event": "frame_begin"}, {"event": "draw"}]
        with mock.patch.object(ambient.bridge.trace_summary, "summarize"), \
             mock.patch.object(ambient.runtime, "read_frame", return_value=records), \
             mock.patch.object(ambient.light_candidates, "within_budget", side_effect=[True, False, True]), \
             mock.patch.object(ambient.light_candidates, "shader_index") as index:
            with self.assertRaisesRegex(ValueError, "budget"):
                ambient.from_trace(scene, "trace", "kernel", 0, 1)
            index.assert_not_called()


if __name__ == "__main__":
    unittest.main()
