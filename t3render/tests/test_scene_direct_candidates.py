"""Point-light previews preserve their offline and pre-Reset limitations."""
import struct
import hashlib
import sys
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import scene_direct_candidates as direct


def sample():
    values = [0.] * 80 + [1., 2., 3., 1.] + [0., .01, .01, 1.]
    words = [0xfffe0100, 0x0000ffff]
    return ({"status": "sampled", "count": 22,
             "bits": list(struct.unpack("<88I", struct.pack("<88f", *values)))},
            {"words": words}, struct.pack("<2I", *words))


class DirectCandidateTests(unittest.TestCase):
    def test_omni_uses_sampled_position_and_explicit_offline_color(self):
        constants, program, binary = sample()
        light = SimpleNamespace(id="light:map:lamp", kind="omni", position=(1, 2, 3), color=(1, .8, .6))
        actors = {"lamp": {"light": {"kind": "omni", "energy": .4}}}
        with mock.patch.object(direct.trace_to_ir, "observed_vertex_constants", return_value=constants), \
             mock.patch.object(direct.trace_to_ir, "pre_reset_vertex_program", return_value=program):
            result = direct.direct_light({}, {}, {}, {}, True, True, True,
                                         {binary: ["OmniLight11_Light_Vertex"]}, [light], actors, 1.5)
        self.assertEqual(result["mode"], "point_approximation")
        self.assertEqual(result["position"], [1, 2, 3])
        self.assertAlmostEqual(result["inverse_radius_candidate"], .01)
        self.assertEqual(result["offline_color"], [1, .8, .6])
        self.assertEqual((result["offline_energy"], result["gain"]), (.4, 1.5))

    def test_spot_is_skipped_and_ambiguity_never_becomes_a_light(self):
        constants, program, binary = sample()
        spot = SimpleNamespace(id="light:map:spot", kind="spot", position=(1, 2, 3), color=None)
        omni = SimpleNamespace(id="light:map:lamp", kind="omni", position=(1, 2, 3), color=(1, 1, 1))
        with mock.patch.object(direct.trace_to_ir, "observed_vertex_constants", return_value=constants), \
             mock.patch.object(direct.trace_to_ir, "pre_reset_vertex_program", return_value=program):
            args = ({}, {}, {}, {}, True, True, True)
            result = direct.direct_light(*args, {binary: ["SpotLight11_Light_Vertex"]}, [spot], {}, 1)
            self.assertEqual(result["mode"], "skipped_spot")
            for lights, index in (([omni, spot], {binary: ["OmniLight11_Light_Vertex"]}),
                                  ([spot], {binary: ["OmniLight11_Light_Vertex"]}),
                                  ([omni], {binary: ["unknown"]}),
                                  ([omni], {binary: ["OmniLight11_Light_Vertex"] * 2})):
                with self.subTest(lights=lights, index=index), self.assertRaises(ValueError):
                    direct.direct_light(*args, index, lights, {}, 1)
            with self.assertRaisesRegex(ValueError, "offline light"):
                direct.direct_light(*args, {binary: ["OmniLight11_Light_Vertex"]}, [omni], {}, 1)

    def test_mismatched_offline_or_actor_fingerprint_fails_before_trace_read(self):
        offline_raw = b"offline-scene"
        offline = SimpleNamespace(source=SimpleNamespace(kind="offline", map="map", fingerprints={"actors": "b" * 64}))
        scene = SimpleNamespace(source=SimpleNamespace(kind="runtime", camera_synchronized=True, map="map", frame_id=3,
                                                        fingerprints={"trace": "a" * 64,
                                                                      "offline_scene": hashlib.sha256(offline_raw).hexdigest(),
                                                                      "actors": "b" * 64}))
        with mock.patch.object(direct.runtime, "read_frame") as reader:
            with self.assertRaisesRegex(ValueError, "provenance"):
                direct.from_trace(scene, offline, b"different", "trace", "kernel", "actors", 0, 3, 1)
            with mock.patch.object(direct.preview, "read_limited", return_value=b"different actors"), \
                 self.assertRaisesRegex(ValueError, "fingerprint"):
                direct.from_trace(scene, offline, offline_raw, "trace", "kernel", "actors", 0, 3, 1)
            reader.assert_not_called()


if __name__ == "__main__":
    unittest.main()
