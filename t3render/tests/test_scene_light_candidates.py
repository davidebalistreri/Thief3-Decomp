"""Shader-constant light candidates remain qualified and revalidated."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import build_scoped_scene_snapshot as scoped
import scene_legacy_states as states
import scene_light_candidates as lights
from test_scoped_scene_snapshot import fixture
from test_scene_pipeline_states import observed_state


def observations():
    point = {"offline_light_id": "light:map:lamp", "kind": "omni", "position": [1, 2, 3],
             "mode": "point_approximation", "offline_color": [1, .5, .25], "offline_energy": .5}
    spot = {"offline_light_id": "light:map:window", "kind": "spot", "position": [4, 5, 6],
            "mode": "spot_approximation", "offline_color": [.5, .5, 1], "offline_energy": .3}
    return {2: copy.deepcopy(point), 3: copy.deepcopy(point), 4: copy.deepcopy(spot), 5: copy.deepcopy(spot)}


class ScopedLightCandidatesTests(unittest.TestCase):
    def setUp(self):
        self.offline, self.records = fixture()
        for row in self.records:
            if row.get("event") == "draw":
                row["render_states"] = observed_state()
        self.offline_bytes = self.offline.to_json().encode()
        self.trace_bytes = "".join(json.dumps(row) + "\n" for row in self.records).encode()
        self.scene, _ = scoped.build(self.offline, self.records, 3, 4, 1, 4,
                                     hashlib.sha256(self.trace_bytes).hexdigest(),
                                     hashlib.sha256(self.offline_bytes).hexdigest())

    def test_sampled_positions_are_candidates_with_unobserved_properties_null(self):
        snapshot, evidence = lights.augment(self.scene, observations(), 4, 1)
        self.assertEqual(snapshot.source.frame_id, 3)
        self.assertEqual(snapshot.submissions, self.scene.submissions)
        self.assertIn(lights.LIMITATION, snapshot.source.limitations)
        self.assertEqual([x.id for x in snapshot.lights], ["candidate:light:map:lamp", "candidate:light:map:window"])
        self.assertEqual(snapshot.lights[0].position, (1, 2, 3))
        self.assertEqual(snapshot.lights[0].kind, "omni")
        self.assertEqual((snapshot.lights[0].color, snapshot.lights[0].radius,
                          snapshot.lights[0].intensity, snapshot.lights[0].enabled), (None, None, None, None))
        self.assertEqual(evidence[0]["supporting_draws"], [2, 3])
        self.assertEqual(snapshot, scoped.SceneSnapshot.from_json(snapshot.to_json()))

    def test_inconsistent_missing_or_unobserved_samples_fail_closed(self):
        for change in ("missing", "position", "kind", "single", "mode", "nonfinite"):
            values = observations()
            if change == "missing": values.pop(2)
            elif change == "position": values[3]["position"][0] += 1
            elif change == "kind": values[3].update(kind="spot", mode="spot_approximation")
            elif change == "single":
                for draw_id in values:
                    values[draw_id]["offline_light_id"] += str(draw_id)
            elif change == "mode": values[2]["mode"] = "skipped_spot"
            else: values[2]["position"][0] = float("nan")
            with self.subTest(change=change), self.assertRaises(ValueError):
                lights.augment(self.scene, values, 4, 1)

    def test_observed_program_source_is_preserved_and_revalidated(self):
        values = observations()
        for item in values.values():
            item["program_source"] = "same_frame_application_memory"
        snapshot, evidence = lights.augment(self.scene, values, 4, 1)
        self.assertIn(lights.OBSERVED_LIMITATION, snapshot.source.limitations)
        self.assertNotIn(lights.LIMITATION, snapshot.source.limitations)
        self.assertEqual(evidence[0]["program_sources"], ["same_frame_application_memory"])
        with tempfile.TemporaryDirectory(prefix="t3render-observed-light-") as directory:
            trace = Path(directory) / "trace.jsonl"
            trace.write_bytes(self.trace_bytes)
            with mock.patch.object(lights, "from_trace", return_value=(snapshot, [])):
                actual = states.from_trace(snapshot, self.offline, self.offline_bytes, trace, 4, 1,
                                           light_actors="actors", light_game="game")
                self.assertEqual(set(actual), {2, 3, 4, 5})
        values[2]["program_source"] = "gpu_readback"
        with self.assertRaisesRegex(ValueError, "light candidate"):
            lights.augment(self.scene, values, 4, 1)

    def test_state_pairing_revalidates_candidates_and_rejects_tampering(self):
        candidate, _ = lights.augment(self.scene, observations(), 4, 1)
        with tempfile.TemporaryDirectory(prefix="t3render-scene-lights-") as directory:
            trace = Path(directory) / "trace.jsonl"
            trace.write_bytes(self.trace_bytes)
            with self.assertRaisesRegex(ValueError, "actor export and game directory"):
                states.from_trace(candidate, self.offline, self.offline_bytes, trace, 4, 1)
            with mock.patch.object(lights, "from_trace", return_value=(candidate, [])) as loader:
                actual = states.from_trace(candidate, self.offline, self.offline_bytes, trace, 4, 1,
                                           light_actors="actors", light_game="game")
                self.assertEqual(set(actual), {2, 3, 4, 5})
                loader.assert_called_once()
                altered = candidate.to_dict()
                altered["lights"][0]["position"][0] += 1
                with self.assertRaisesRegex(ValueError, "revalidated light candidates"):
                    states.from_trace(scoped.SceneSnapshot.from_dict(altered), self.offline, self.offline_bytes,
                                      trace, 4, 1, light_actors="actors", light_game="game")
            altered = candidate.to_dict()
            altered["source"]["limitations"].remove(lights.LIMITATION)
            with self.assertRaisesRegex(ValueError, "revalidated scoped capture"):
                states.from_trace(scoped.SceneSnapshot.from_dict(altered), self.offline, self.offline_bytes,
                                  trace, 4, 1)


if __name__ == "__main__":
    unittest.main()
