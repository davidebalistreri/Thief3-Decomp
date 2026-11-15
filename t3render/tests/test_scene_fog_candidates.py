"""Fog observations and offline colors are kept as explicit hypotheses."""
import copy
import hashlib
import json
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import scene_fog_candidates as fog


def actors():
    return {"LevelInfo__0": {"name": "LevelInfo__0", "cls": "LevelInfo", "properties": {
        "bDistanceFog": True, "DistanceFogStart": 50., "DistanceFogEnd": 3500.,
        "DistanceFogColor": [{"name": k, "type": "Byte", "value": v} for k, v in (("B", 36), ("G", 15), ("R", 11))]}}}


def sample(center=(1, 2, 3, 1), interval=(50, 3500, 1 / 3450, .5)):
    values = [0.] * (29 * 4)
    values[26*4:27*4], values[28*4:29*4] = center, interval
    words = [0xfffe0100, 0x0000ffff]
    return ({"status": "sampled", "count": 29,
             "bits": list(struct.unpack("<116I", struct.pack("<116f", *values)))},
            {"words": words}, struct.pack("<2I", *words))


class FogCandidateTests(unittest.TestCase):
    def test_offline_fog_requires_unique_complete_level_and_explicit_color(self):
        expected = fog.offline_fog(actors())
        self.assertEqual(expected["offline_color"], [11/255, 15/255, 36/255])
        variants = []
        for change in ("disabled", "range", "missing", "duplicate", "color", "levels"):
            data = copy.deepcopy(actors())
            props = data["LevelInfo__0"]["properties"]
            if change == "disabled": props["bDistanceFog"] = False
            if change == "range": props["DistanceFogEnd"] = 50
            if change == "missing": props["DistanceFogColor"].pop()
            if change == "duplicate": props["DistanceFogColor"].append(props["DistanceFogColor"][0])
            if change == "color": props["DistanceFogColor"][0]["value"] = 256
            if change == "levels": data["another"] = copy.deepcopy(data["LevelInfo__0"])
            variants.append(data)
        for data in variants:
            with self.subTest(data=data), self.assertRaises(ValueError):
                fog.offline_fog(data)

    def test_program_camera_and_fog_range_are_cross_checked(self):
        constants, program, binary = sample()
        offline = fog.offline_fog(actors())
        args = ({}, {}, {}, {}, True, True, True)
        with mock.patch.object(fog.trace_to_ir, "observed_vertex_constants", return_value=constants), \
             mock.patch.object(fog.trace_to_ir, "pre_reset_vertex_program", return_value=program):
            result = fog.fog_for_draw(*args, {binary: ["Fog11_Light_Vertex"]}, offline, (1, 2, 3))
            self.assertEqual(result["camera_position"], [1, 2, 3])
            self.assertEqual(result["offline_color"], [11/255, 15/255, 36/255])
            for index, camera in (({}, (1, 2, 3)), ({binary: ["Fog11_Light_Vertex"] * 2}, (1, 2, 3)),
                                  ({binary: ["other"]}, (1, 2, 3)), ({binary: ["Fog11_Light_Vertex"]}, (2, 2, 3))):
                with self.subTest(index=index, camera=camera), self.assertRaises(ValueError):
                    fog.fog_for_draw(*args, index, offline, camera)
        for values in (sample(interval=(51, 3500, 1 / 3450, .5))[0],
                       sample(interval=(50, 3500, .01, .5))[0],
                       sample(center=(1, 2, 3, 0))[0], dict(constants, count=28), None):
            with mock.patch.object(fog.trace_to_ir, "observed_vertex_constants", return_value=values), \
                 mock.patch.object(fog.trace_to_ir, "pre_reset_vertex_program", return_value=program), \
                 self.assertRaises(ValueError):
                fog.fog_for_draw(*args, {binary: ["Fog11_Light_Vertex"]}, offline, (1, 2, 3))

    def test_provenance_and_incomplete_observation_budget_fail_closed(self):
        actor_bytes = json.dumps({"actors": list(actors().values())}).encode()
        offline_bytes = b"fixture"
        fingerprint = {"actors": hashlib.sha256(actor_bytes).hexdigest(), "kernel": "a" * 64}
        scene = SimpleNamespace(source=SimpleNamespace(kind="runtime", camera_synchronized=True, map="map", frame_id=7,
            fingerprints={**fingerprint, "trace": "b" * 64, "offline_scene": hashlib.sha256(offline_bytes).hexdigest()}))
        offline = SimpleNamespace(source=SimpleNamespace(kind="offline", map="map", fingerprints=fingerprint))
        records = [{"event": "header"}, {"event": "frame_begin"}, {"event": "draw"}]
        with mock.patch.object(fog.direct.preview, "read_limited", return_value=actor_bytes), \
             mock.patch.object(fog.bridge.trace_summary, "summarize"), \
             mock.patch.object(fog.runtime, "read_frame", return_value=records), \
             mock.patch.object(fog.candidates, "within_budget", side_effect=[True, False, True]), \
             mock.patch.object(fog.candidates, "shader_index") as index:
            with self.assertRaisesRegex(ValueError, "budget"):
                fog.from_trace(scene, offline, offline_bytes, "trace", "kernel", "actors", 0, 6)
            index.assert_not_called()
        with mock.patch.object(fog.direct.preview, "read_limited", return_value=actor_bytes + b" "):
            with self.assertRaisesRegex(ValueError, "fingerprint"):
                fog.from_trace(scene, offline, offline_bytes, "trace", "kernel", "actors", 0, 6)
        scene.source.camera_synchronized = False
        with self.assertRaisesRegex(ValueError, "provenance"):
            fog.from_trace(scene, offline, offline_bytes, "trace", "kernel", "actors", 0, 6)


if __name__ == "__main__":
    unittest.main()
