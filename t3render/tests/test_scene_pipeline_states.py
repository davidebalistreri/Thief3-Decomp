import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import build_scoped_scene_snapshot as scoped
import scene_legacy_states as states
import scene_pipeline_fixture as gpu_fixture
import scene_ir_to_textured_packet as packet
from test_scoped_scene_snapshot import fixture, candidate_fixture
import test_scene_textured_packet as packet_tests


def observed_state(**updates):
    values = {"7": 1, "14": 1, "15": 0, "19": 2, "20": 1, "22": 3,
              "23": 2, "24": 4, "25": 5, "27": 0, "28": 0, "137": 0, "168": 15, "171": 1}
    values.update(updates)
    return values


class LegacyStateTests(unittest.TestCase):
    def test_frame_stage_modes_convert_to_neutral_pipeline_values(self):
        opaque = states.translate(observed_state())
        self.assertEqual(opaque, states.DEFAULT)
        additive = states.translate(observed_state(**{"14": 0, "19": 2, "20": 2, "23": 4, "27": 1, "168": 7}))
        self.assertEqual((additive["blend_src"], additive["blend_dst"], additive["depth_compare"], additive["depth_write"]),
                         (1, 1, 4, 0))
        multiply = states.translate(observed_state(**{"14": 0, "19": 9, "20": 3, "23": 4, "27": 1, "168": 7}))
        self.assertEqual((multiply["blend_src"], multiply["blend_dst"]), (6, 2))
        alpha = states.translate(observed_state(**{"14": 0, "19": 5, "20": 6, "23": 4, "27": 1, "168": 7}))
        self.assertEqual((alpha["blend_src"], alpha["blend_dst"]), (4, 5))
        tested = states.translate(observed_state(**{"15": 1, "25": 7, "24": 127, "22": 2}))
        self.assertEqual((tested["alpha_compare"], tested["alpha_ref"], tested["cull"]), (7, 127, 2))

    def test_missing_unsupported_or_unimplemented_state_fails_closed(self):
        for change in ({"7": 2}, {"19": 11}, {"15": True}, {"28": 1}, {"137": 1}, {"168": 16}, {"171": 0}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                states.translate(observed_state(**change))
        data = observed_state()
        del data["23"]
        with self.assertRaises(ValueError):
            states.translate(data)

    def test_snapshot_and_trace_must_revalidate_before_state_is_used(self):
        offline, records = fixture()
        for row in records:
            if row.get("event") == "draw":
                row["render_states"] = observed_state()
        offline_bytes = offline.to_json().encode()
        offline_digest = hashlib.sha256(offline_bytes).hexdigest()
        raw = "".join(json.dumps(row) + "\n" for row in records).encode()
        digest = hashlib.sha256(raw).hexdigest()
        scene, _ = scoped.build(offline, records, 3, 4, 1, 4, digest, offline_digest)
        with tempfile.TemporaryDirectory(prefix="t3render-state-trace-") as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_bytes(raw)
            result = states.from_trace(scene, offline, offline_bytes, path, 4, 1)
            self.assertEqual(set(result), {2, 3, 4, 5})
            self.assertTrue(all(state == states.DEFAULT for state in result.values()))
            altered = scene.to_dict()
            altered["mesh_instances"][0]["transform"][12] += 1
            with self.assertRaisesRegex(ValueError, "does not match"):
                states.from_trace(packet.SceneSnapshot.from_dict(altered), offline, offline_bytes, path, 4, 1)
            with self.assertRaisesRegex(ValueError, "fingerprints"):
                states.from_trace(scene, offline, offline_bytes + b" ", path, 4, 1)
            path.write_bytes(raw + b" ")
            with self.assertRaises(ValueError):
                states.from_trace(scene, offline, offline_bytes, path, 4, 1)

    def test_candidate_snapshot_revalidates_and_cannot_hide_provenance(self):
        offline, records = candidate_fixture()
        for row in records:
            if row.get("event") == "draw":
                row["render_states"] = observed_state()
        offline_bytes = offline.to_json().encode()
        offline_digest = hashlib.sha256(offline_bytes).hexdigest()
        raw = "".join(json.dumps(row) + "\n" for row in records).encode()
        digest = hashlib.sha256(raw).hexdigest()
        scene, _ = scoped.build(offline, records, 3, 4, 1, 4, digest, offline_digest,
                                include_asset_candidates=True)
        with tempfile.TemporaryDirectory(prefix="t3render-candidate-state-") as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_bytes(raw)
            self.assertEqual(set(states.from_trace(scene, offline, offline_bytes, path, 4, 1)), {2, 3, 4, 5, 6})
            data = scene.to_dict()
            data["source"]["limitations"].remove(scoped.CANDIDATE_LIMITATION)
            with self.assertRaisesRegex(ValueError, "does not match"):
                states.from_trace(packet.SceneSnapshot.from_dict(data), offline, offline_bytes, path, 4, 1)
            quantized, _ = scoped.build(offline, records, 3, 4, 1, 4, digest, offline_digest,
                                         include_asset_candidates=True, allow_quantized_rotators=True)
            self.assertEqual(set(states.from_trace(quantized, offline, offline_bytes, path, 4, 1)), {2, 3, 4, 5, 6})
            data = quantized.to_dict()
            data["source"]["limitations"].remove(scoped.QUANTIZED_LIMITATION)
            with self.assertRaisesRegex(ValueError, "does not match"):
                states.from_trace(packet.SceneSnapshot.from_dict(data), offline, offline_bytes, path, 4, 1)

    def test_missing_or_invalid_state_cannot_be_serialized(self):
        scene, records = fixture()
        scene, _ = scoped.build(scene, records, 3, 4, 1, 4, "b" * 64, "c" * 64)
        with self.assertRaisesRegex(ValueError, "missing"):
            packet.create_packet(scene, packet_tests.Resolver(), draw_states={})
        invalid = {s.draw_id: dict(states.DEFAULT, alpha_compare=99) for s in scene.submissions}
        with self.assertRaisesRegex(ValueError, "invalid draw state"):
            packet.create_packet(scene, packet_tests.Resolver(), draw_states=invalid)

    def test_self_attached_snapshot_requires_asset_revalidation(self):
        original, records = candidate_fixture()
        data = original.to_dict()
        extra = data["mesh_instances"].pop()
        offline = packet.SceneSnapshot.from_dict(data)
        resource = next(x for x in offline.static_mesh_resources if x.id == extra["resource_id"])
        names = {x.id: x.name for x in offline.materials}
        assets = {"StaticMeshActor__258": {"target_id": extra["id"], "resource": resource,
                   "material_ids": tuple(extra["material_ids"]),
                   "materials": {mid: names[mid] for mid in extra["material_ids"] if mid is not None}}}
        for row in records:
            if row.get("event") == "draw":
                row["render_states"] = observed_state()
        offline_bytes = offline.to_json().encode()
        raw = "".join(json.dumps(row) + "\n" for row in records).encode()
        scene, _ = scoped.build(offline, records, 3, 4, 1, 4, hashlib.sha256(raw).hexdigest(),
                                hashlib.sha256(offline_bytes).hexdigest(), include_asset_candidates=True,
                                self_attached_assets=assets)
        with tempfile.TemporaryDirectory(prefix="t3render-self-state-") as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_bytes(raw)
            with self.assertRaisesRegex(ValueError, "actor export and game directory"):
                states.from_trace(scene, offline, offline_bytes, path, 4, 1)
            with mock.patch.object(scoped.self_attached, "load", return_value=assets) as loader:
                observed = states.from_trace(scene, offline, offline_bytes, path, 4, 1,
                                             self_attached_actors="actors", self_attached_game="game")
                self.assertEqual(set(observed), {2, 3, 4, 5, 6})
                loader.assert_called_once()
            quantized, _ = scoped.build(offline, records, 3, 4, 1, 4, hashlib.sha256(raw).hexdigest(),
                                        hashlib.sha256(offline_bytes).hexdigest(), include_asset_candidates=True,
                                        allow_quantized_rotators=True, self_attached_assets=assets)
            with mock.patch.object(scoped.self_attached, "load", return_value=assets):
                self.assertEqual(set(states.from_trace(quantized, offline, offline_bytes, path, 4, 1,
                                                       self_attached_actors="actors", self_attached_game="game")),
                                 {2, 3, 4, 5, 6})
            altered = scene.to_dict()
            altered["source"]["limitations"].remove(scoped.SELF_ATTACHED_LIMITATION)
            with self.assertRaisesRegex(ValueError, "does not match"):
                states.from_trace(packet.SceneSnapshot.from_dict(altered), offline, offline_bytes, path, 4, 1)

    def test_composition_refuses_intervening_target_or_depth_clears(self):
        scene = packet_tests.scoped_scene()
        draws = {2: {}, 8: {}}
        with mock.patch.object(states.runtime, "read_frame") as reader:
            for flags in (1, 2, 3, 8):
                reader.return_value = [{"event": "clear", "op_id": 5, "flags": flags}]
                with self.subTest(flags=flags), self.assertRaisesRegex(ValueError, "clear"):
                    states.validate_composition_interval(scene, "trace", draws)
            reader.return_value = [{"event": "clear", "op_id": 1, "flags": 3},
                                   {"event": "clear", "op_id": 5, "flags": 4},
                                   {"event": "clear", "op_id": 9, "flags": 3}]
            states.validate_composition_interval(scene, "trace", draws)

    def test_fixture_verifier_rejects_wrong_blend_and_depth_pixels(self):
        image = bytearray(b"P6\n64 64\n255\n" + bytes(64 * 64 * 3))
        header = len(b"P6\n64 64\n255\n")
        for (x, y), color in gpu_fixture.EXPECTED.items():
            start = header + (y * 64 + x) * 3
            image[start:start + 3] = bytes(color)
        self.assertEqual(len(gpu_fixture.verify(image)), 11)
        image[header + (48 * 64 + 48) * 3 + 1] = 0
        with self.assertRaisesRegex(ValueError, "wrong pixel"):
            gpu_fixture.verify(image)


class PipelinePacketTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("host C++ compiler required")
        cls.temp = tempfile.TemporaryDirectory(prefix="t3render-pipeline-packet-")
        cls.binary = Path(cls.temp.name) / "parser"
        result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I",
                                 str(HERE.parent / "src"), "-x", "c++", "-o", str(cls.binary), "-"],
                                input=packet_tests.HARNESS, text=True, capture_output=True, timeout=60)
        if result.returncode:
            cls.temp.cleanup()
            raise RuntimeError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def parse(self, raw):
        return subprocess.run([str(self.binary)], input=raw, capture_output=True, timeout=10)

    def test_v2_preserves_per_draw_states_and_rejects_corruption(self):
        raw = gpu_fixture.fixture()
        self.assertEqual(self.parse(raw).returncode, 0)
        # Header and seven one-pixel textures precede the first v2 draw.
        start = 160 + 7 * 16
        for field, value in ((2, 2), (4, 9), (5, 3), (8, 10), (9, 5), (10, 9), (11, 256), (12, 16)):
            broken = bytearray(raw)
            struct.pack_into("<I", broken, start + field * 4, value)
            with self.subTest(field=field):
                self.assertEqual(self.parse(broken).returncode, 2)
        self.assertEqual(self.parse(raw[:-1]).returncode, 2)


if __name__ == "__main__":
    unittest.main()
