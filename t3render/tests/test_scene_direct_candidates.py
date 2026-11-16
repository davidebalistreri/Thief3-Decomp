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
from test_trace_to_ir_programs import WORDS, program_events


def sample():
    values = [0.] * 80 + [1., 2., 3., 1.] + [0., .01, .01, 1.]
    words = [0xfffe0100, 0x0000ffff]
    return ({"status": "sampled", "count": 22,
             "bits": list(struct.unpack("<88I", struct.pack("<88f", *values)))},
            {"words": words}, struct.pack("<2I", *words))


def spot_sample():
    values = [0.] * (26 * 4)
    values[19*4:20*4] = [-1, 0, 0, 0]
    values[20*4:21*4] = [1, 2, 3, 1]
    values[21*4:22*4] = [0, .01, .01, 1]
    values[22*4:23*4] = [.5, .5, 0, -1.5]
    values[23*4:24*4] = [.5, 0, .5, -2]
    values[25*4:26*4] = [1, 0, 0, -1]
    words = [0xfffe0100, 0x0000ffff]
    return ({"status": "sampled", "count": 26,
             "bits": list(struct.unpack("<104I", struct.pack("<104f", *values)))},
            {"words": words}, struct.pack("<2I", *words))


class DirectCandidateTests(unittest.TestCase):
    def test_sampled_normal_shader_constants_are_bounded_and_complete(self):
        values = [0.] * (39 * 4)
        values[0:4] = [0, .5, 1, 2]
        values[16*4:17*4] = [1, 1, 1, -1]
        values[38*4:39*4] = [.25, -.5, 0, 0]
        sample = {"count": 39, "bits": list(struct.unpack("<156I", struct.pack("<156f", *values)))}
        self.assertEqual(direct.normal_shader_basis(sample), {"tangent_scale": -1, "uv_offset": [.25, -.5]})
        for change in ("count", "base", "scale", "offset"):
            altered = dict(sample, bits=list(sample["bits"]))
            if change == "count": altered["count"] = 38
            if change == "base": altered["bits"][1] = 0
            if change == "scale": altered["bits"][16*4+3] = 0
            if change == "offset": altered["bits"][38*4] = 0x7fc00000
            with self.subTest(change=change), self.assertRaises(ValueError):
                direct.normal_shader_basis(altered)

    def test_normal_map_observation_requires_normal_program_and_bound_sampler(self):
        states = {"13": 1, "14": 1, "16": 2, "17": 2, "18": 2, "19": 3212836864, "20": 0, "24": 0}
        stage = {"stage": 0, "states": states, "texture": {"type": 3, "width": 512, "height": 512,
                 "levels": 10, "format": struct.unpack("<I", b"DXT1")[0], "id": "0x1234"}}
        light = {"mode": "point_approximation", "program_hypothesis": "OmniLightNormal11_Light_Vertex"}
        result = direct.normal_sampler_observation({"stages": [stage]}, light)
        self.assertEqual(result["lod_bias"], -1)
        self.assertNotIn("id", result)
        self.assertIsNone(direct.normal_sampler_observation({}, dict(light, program_hypothesis="OmniLight11_Light_Vertex")))
        self.assertIsNone(direct.normal_sampler_observation({}, dict(light, mode="skipped_spot")))
        for bad in (dict(stage, texture=None), dict(stage, texture_hr=-1)):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                direct.normal_sampler_observation({"stages": [bad]}, light)

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
        self.assertEqual(result["program_source"], "pre_reset_hypothesis")

    def test_same_frame_program_is_used_before_pre_reset_history(self):
        constants, program, binary = sample()
        program["status"] = "candidate"
        light = SimpleNamespace(id="light:map:lamp", kind="omni", position=(1, 2, 3), color=(1, .8, .6))
        actors = {"lamp": {"light": {"kind": "omni", "energy": .4}}}
        with mock.patch.object(direct.trace_to_ir, "observed_vertex_constants", return_value=constants), \
             mock.patch.object(direct.trace_to_ir, "observed_vertex_program", return_value=program), \
             mock.patch.object(direct.trace_to_ir, "pre_reset_vertex_program") as history:
            result = direct.direct_light({}, {}, {}, {}, True, True, True,
                                         {binary: ["OmniLight11_Light_Vertex"]}, [light], actors, 1)
        self.assertEqual(result["program_source"], "same_frame_application_memory")
        history.assert_not_called()

    def test_observed_program_does_not_require_pre_reset_history_budget(self):
        constants, _, _ = sample()
        light = SimpleNamespace(id="light:map:lamp", kind="omni", position=(1, 2, 3), color=(1, .8, .6))
        actors = {"lamp": {"light": {"kind": "omni", "energy": .4}}}
        scene = SimpleNamespace(source=SimpleNamespace(frame_id=0, fingerprints={"trace": "a" * 64, "kernel": "b" * 64}),
                                submissions=[SimpleNamespace(draw_id=1, render_pass=0, stage=3)])
        offline = SimpleNamespace(lights=[light])
        binary = struct.pack(f"<{len(WORDS)}I", *WORDS)
        with mock.patch.object(direct, "offline_actors", return_value=actors), \
             mock.patch.object(direct.bridge.trace_summary, "summarize"), \
             mock.patch.object(direct.runtime, "read_frame", return_value=program_events()), \
             mock.patch.object(direct.candidates, "within_budget", side_effect=[True, True, False]), \
             mock.patch.object(direct.candidates, "shader_index", return_value={binary: ["OmniLight11_Light_Vertex"]}), \
             mock.patch.object(direct.trace_to_ir, "observed_vertex_constants", return_value=constants):
            result = direct.from_trace(scene, offline, b"offline", "trace", "kernel", "actors", 0, 3, 1)
        self.assertEqual(result[1]["program_source"], "same_frame_application_memory")

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

    def test_spot_preview_requires_matching_axis_origin_and_projector(self):
        constants, program, binary = spot_sample()
        spot = SimpleNamespace(id="light:map:spot", kind="spot", position=(1, 2, 3), color=(.4, .5, .6))
        actor = {"rotation": [0, 0, 0], "light": {"kind": "spot", "energy": .4}}
        with mock.patch.object(direct.trace_to_ir, "observed_vertex_constants", return_value=constants), \
             mock.patch.object(direct.trace_to_ir, "pre_reset_vertex_program", return_value=program):
            args = ({}, {}, {}, {}, True, True, True, {binary: ["SpotLight11_Light_Vertex"]}, [spot], {"spot": actor}, 1)
            result = direct.direct_light(*args, preview_spots=True)
            self.assertEqual(result["mode"], "spot_approximation")
            self.assertEqual(len(result["projector_rows"]), 3)
            self.assertEqual(direct.direct_light(*args)["mode"], "skipped_spot")
        for change in ("rotation", "origin", "center", "truncated"):
            altered = dict(constants, bits=constants["bits"][:])
            candidate_actor = dict(actor)
            if change == "rotation": candidate_actor["rotation"] = [0, 8192, 0]
            elif change == "origin": altered["bits"][25*4+3] = struct.unpack("<I", struct.pack("<f", 0))[0]
            elif change == "center": altered["bits"][22*4+3] = struct.unpack("<I", struct.pack("<f", -30))[0]
            else: altered["count"] = 25
            with self.subTest(change=change), self.assertRaises(ValueError):
                direct.spot_projector(altered, candidate_actor, [1, 2, 3])

    def test_projector_observation_keeps_metadata_and_rejects_sampler_mismatch(self):
        stage = {"stage": 3, "states": {"11": 3, "13": 4, "14": 4, "15": 0, "16": 2, "17": 2, "18": 0, "20": 0, "24": 260},
                 "texture": {"id": "0x1234", "type": 3, "width": 256, "height": 256, "levels": 9, "format": 861165636}}
        result = direct.projector_texture_observation({"stages": [stage]}, "genwindowint_p")
        self.assertEqual(result["name"], "genwindowint_p")
        self.assertNotIn("id", result)
        for change in ("address", "projection", "unavailable", "duplicate"):
            item = {**stage, "states": dict(stage["states"])}
            draw = {"stages": [item]}
            if change == "address": item["states"]["13"] = 1
            elif change == "projection": item["states"]["24"] = 0
            elif change == "unavailable": item["texture_hr"] = -1
            else: draw["stages"].append(item)
            with self.subTest(change=change), self.assertRaises(ValueError):
                direct.projector_texture_observation(draw, "candidate")


if __name__ == "__main__":
    unittest.main()
