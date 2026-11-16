"""Recorded callback replay remains distinct from a live SceneIR frame."""
import copy
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import export_live_catalog as catalog
import replay_scene_live_capture as replay
from test_scene_trace_session import two_frames, write_trace


def synthetic_catalog(scene):
    data = scene.to_dict()
    source = data["source"]
    bundle = "b" * 64
    source["fingerprints"]["bundle"] = bundle
    map_name, map_hash = source["map"], source["fingerprints"]["map"]
    bed = f"mesh:{map_name}:{map_hash}:{bundle}:innbed1a1"
    actor = f"actor:{map_name}:{map_hash}:StaticMeshActor__1520"
    data["mesh_instances"][0].update(id=actor, resource_id=bed)
    data["static_mesh_resources"][0]["id"] = bed
    for block in data["bsp_blocks"]:
        block["id"] = f"bsp:{map_name}:{map_hash}:{block['source_index']}:{block['source_offset']}"
    return catalog.export(json.dumps(data).encode())[0]


class CaptureReplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which(os.environ.get("CXX", "c++")):
            raise unittest.SkipTest("C++ compiler required")

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="t3render-capture-replay-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        scene, self.records = two_frames()
        self.trace = self.root / "trace.jsonl"
        self.catalog = self.root / "catalog"
        self.catalog.write_bytes(synthetic_catalog(scene))
        write_trace(self.trace, self.records)

    def run_replay(self):
        return replay.replay(self.trace, self.catalog, [3, 4], 4)

    def test_synthetic_callbacks_produce_captured_replay_frames(self):
        result = self.run_replay()
        self.assertEqual(result["source"], "captured_replay")
        self.assertEqual([f["result"] for f in result["frames"]], ["published", "published"])
        for frame in result["frames"]:
            self.assertEqual(frame["source"], "captured_replay")
            self.assertTrue(frame["flat_valid"])
            self.assertEqual((len(frame["meshes"]), len(frame["bsp"]), frame["flat_draws"], frame["flat_triangles"]),
                             (1, 1, 4, 4))
            self.assertEqual(frame["meshes"][0]["sections"], [1, 2])
            self.assertEqual(frame["bsp"][0]["sections"], [0, 1])
        self.assertFalse(result["frames"][1]["camera_changed_from_previous"])
        self.assertNotIn('"0x1234"', json.dumps(result))

    def test_changed_draw_camera_rejects_only_its_frame(self):
        changed = copy.deepcopy(self.records)
        draw = next(row for row in changed if row.get("event") == "draw" and row.get("frame") == 4)
        draw["transforms"]["view"][12] += 1
        write_trace(self.trace, changed)
        result = self.run_replay()
        first, second = result["frames"]
        self.assertEqual((first["result"], second["result"]), ("published", "rejected"))
        self.assertGreater(second["draw_camera_bad"], 0)
        self.assertNotIn("camera", second)

    def test_unstable_scope_rejects_frame(self):
        changed = copy.deepcopy(self.records)
        end = next(row for row in changed if row.get("event") == "scene_bridge_submission_end" and row.get("frame") == 4)
        end["stable"] = False
        write_trace(self.trace, changed)
        self.assertEqual([f["result"] for f in self.run_replay()["frames"]], ["published", "rejected"])

    def test_corrupt_catalog_and_incomplete_window_are_rejected(self):
        raw = bytearray(self.catalog.read_bytes())
        raw[12] ^= 1
        self.catalog.write_bytes(raw)
        with self.assertRaisesRegex(ValueError, "catalog fingerprint"):
            self.run_replay()
        self.catalog.write_bytes(synthetic_catalog(two_frames()[0]))
        changed = copy.deepcopy(self.records)
        window = next(row for row in changed if row.get("event") == "scene_bridge_frame" and row.get("frame") == 4)
        window["submission_dropped"] = 1
        write_trace(self.trace, changed)
        with self.assertRaises(ValueError):
            self.run_replay()

    def test_generated_text_cannot_escape_cpp_literal(self):
        emitter = replay.Emitter()
        emitter.text("value", 'a"\\;evil', "available", 64)
        generated = '\n'.join(emitter.lines)
        self.assertNotIn('evil', generated)
        self.assertIn(r'\x22\x5c\x3b', generated)

    @unittest.skipUnless(os.environ.get("T3RENDER_CAPTURED_TRACE") and
                         os.environ.get("T3RENDER_CAPTURED_CATALOG") and
                         os.environ.get("T3RENDER_CAPTURED_SCENE_DIR"), "local golden capture not supplied")
    def test_local_golden_frames_match_camera_bed_and_bsp(self):
        report = replay.replay(Path(os.environ["T3RENDER_CAPTURED_TRACE"]),
                               Path(os.environ["T3RENDER_CAPTURED_CATALOG"]),
                               [2863, 2864, 2865], 0, Path(os.environ["T3RENDER_CAPTURED_SCENE_DIR"]))
        self.assertEqual([f["result"] for f in report["frames"]], ["published"] * 3)
        self.assertEqual([(len(f["meshes"]), len(f["bsp"]), f["flat_draws"], f["flat_triangles"])
                          for f in report["frames"]], [(1, 9, 18, 450)] * 3)
        self.assertEqual([f["reference_match"] for f in report["frames"]], ["camera_bed_and_bsp_sections"] * 3)
        self.assertFalse(report["frames"][1]["camera_changed_from_previous"])
        self.assertTrue(report["frames"][2]["camera_changed_from_previous"])
        changed = copy.deepcopy(report["frames"])
        changed[1]["meshes"][0]["transform"][12] += 1
        with self.assertRaisesRegex(ValueError, "transform"):
            replay.compare_snapshots(changed, Path(os.environ["T3RENDER_CAPTURED_SCENE_DIR"]), report["trace_sha256"], 0)

    @unittest.skipUnless(os.environ.get("T3RENDER_EARLY_LIVE_TRACE") and
                         os.environ.get("T3RENDER_CAPTURED_CATALOG"), "early live capture not supplied")
    def test_early_bed_absent_capture_does_not_invent_map_witness(self):
        # The historical capture has a matching BSP candidate, but predates
        # the independent map-witness field. Its owner map is not observable.
        report = replay.replay(Path(os.environ["T3RENDER_EARLY_LIVE_TRACE"]),
                               Path(os.environ["T3RENDER_CAPTURED_CATALOG"]), [1243, 1244, 1245])
        self.assertEqual(report["source"], "captured_replay")
        self.assertEqual([item["result"] for item in report["frames"]], ["rejected"] * 3)
        self.assertEqual([item["selected_probes"] for item in report["frames"]], [1, 2, 3])
        self.assertTrue(all(item["last_rejection"] & 128 for item in report["frames"])) # no validated viewport
        self.assertTrue(all(not item["bsp_scopes"] and not item["bed_scopes"] for item in report["frames"]))
        if os.environ.get("T3RENDER_CAPTURED_TRACE"):
            earlier, _, _ = replay.read_capture(Path(os.environ["T3RENDER_EARLY_LIVE_TRACE"]), [1243])
            golden, _, _ = replay.read_capture(Path(os.environ["T3RENDER_CAPTURED_TRACE"]), [2864])
            def observation(rows, frame):
                matches = [item for item in rows[frame] if item["event"] == "scene_bridge_observation" and item["pass"] == 0]
                self.assertEqual(len(matches), 1)
                return matches[0]
            early_obs, golden_obs = observation(earlier, 1243), observation(golden, 2864)
            self.assertIsNone(early_obs.get("map_witness"))
            self.assertEqual(early_obs["target"]["list_occurrences"], 0)
            def block(obs):
                matches = [item for item in obs["bsp"]["candidates"] if item["source_index"] == 7]
                self.assertEqual(len(matches), 1)
                return {key: matches[0][key] for key in ("counts", "bounds_bits", "transform_bits", "sections")}
            self.assertEqual(block(early_obs), block(golden_obs))


if __name__ == "__main__":
    unittest.main()
