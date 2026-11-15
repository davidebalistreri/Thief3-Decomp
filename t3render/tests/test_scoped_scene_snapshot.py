import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import build_scoped_scene_snapshot as builder
import scene_ir_to_d3d12_packet as preview
from test_verify_bridge_submission import complete_fixture
from test_analyze_scene_bridge import bits
from test_scene_ir import IDENTITY


def fixture():
    scene, records = complete_fixture()
    # BSP positions are already in world coordinates; use the identity world
    # transform instead of the mesh transform shared by the generic fixture.
    records[2]["entries"][1]["object_matrix_bits"] = bits(IDENTITY)
    records[2]["bsp"]["candidates"][0]["transform_bits"] = bits(IDENTITY)
    for index in (3, 7):
        records[index]["matrix_bits"] = bits(IDENTITY)
        records[index + 1]["transforms"]["world"][0] = IDENTITY[:]
    return scene, records


class ScopedSceneSnapshotTests(unittest.TestCase):
    def build(self, scene, records):
        with tempfile.TemporaryDirectory(prefix="t3render-scoped-scene-") as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_text("".join(json.dumps(row) + "\n" for row in records))
            builder.bridge.trace_summary.summarize(path)
        return builder.build(scene, records, 3, 4, 1, 4, "b" * 64, "c" * 64)

    def test_runtime_snapshot_roundtrip_renders_only_submitted_sections(self):
        scene, records = fixture()
        snapshot, manifest = self.build(scene, records)
        self.assertEqual(snapshot.version, 1)
        self.assertEqual(snapshot.source.kind, "runtime")
        self.assertEqual(snapshot.source.frame_id, 3)
        self.assertEqual(snapshot.camera.source.fingerprint, "b" * 64)
        self.assertEqual(snapshot.mesh_instances[0].transform, tuple(records[12]["transforms"]["world"][0]))
        self.assertIsNone(snapshot.mesh_instances[0].skin)
        self.assertEqual(snapshot.lights, ())
        self.assertEqual([s.draw_id for s in snapshot.submissions], [2, 3, 4, 5])
        self.assertTrue(all(s.lock_shadow == "unverified" for s in snapshot.submissions))
        self.assertEqual(manifest["bsp_blocks"], 1)
        self.assertEqual(builder.SceneSnapshot.from_json(snapshot.to_json()).to_dict(), snapshot.to_dict())
        packet, info = preview.create_packet(snapshot, snapshot.camera, [snapshot.mesh_instances[0].id], [0])
        self.assertEqual(info["triangles"], 4)
        self.assertEqual(info["submission_scope"], {"render_pass": 4, "stage": 1})
        self.assertEqual([(x["kind"], x["section"]) for x in info["sources"]],
                         [("mesh_instance", 1), ("mesh_instance", 2), ("bsp_block", 0), ("bsp_block", 1)])
        self.assertIn("not_submitted_in_selected_scope", str(info["excluded_sections"]))
        self.assertEqual(packet.splitlines()[7], "4")

    def test_incomplete_submission_or_wrong_anchor_fails_closed(self):
        scene, records = fixture()
        bad = copy.deepcopy(records)
        bad[4]["start_index"] = 3
        with self.assertRaisesRegex(ValueError, "lack validated"):
            self.build(scene, bad)
        with self.assertRaisesRegex(ValueError, "anchor must be"):
            builder.build(scene, records, 3, 4, 1, 2, "b" * 64, "c" * 64)

    def test_viewport_differences_do_not_enter_same_camera_snapshot(self):
        scene, records = fixture()
        records[4]["viewport"]["max_z"] = 0.5  # first BSP draw only
        snapshot, _ = self.build(scene, records)
        self.assertEqual([s.draw_id for s in snapshot.submissions], [3, 4, 5])

    def test_v1_rejects_missing_targets_sections_or_provenance(self):
        scene, records = fixture()
        snapshot, _ = self.build(scene, records)
        for change in ("target", "section", "duplicate", "trace", "offline", "pointer", "unknown"):
            data = snapshot.to_dict()
            sub = data["submissions"][0]
            if change == "target": sub["target_id"] = "missing"
            elif change == "section": sub["section"] = 999
            elif change == "duplicate": data["submissions"][1]["draw_id"] = sub["draw_id"]
            elif change == "trace": data["source"]["fingerprints"].pop("trace")
            elif change == "offline": data["source"].update(kind="offline", frame_id=None, camera_synchronized=False)
            elif change == "pointer": sub["target_id"] = "0x12345678"
            else: sub["runtime_pointer"] = 123
            with self.subTest(change=change), self.assertRaises(ValueError):
                builder.SceneSnapshot.from_dict(data)

    def test_preview_rejects_ambiguous_or_missing_submission_scope(self):
        scene, records = fixture()
        snapshot, _ = self.build(scene, records)
        data = snapshot.to_dict()
        extra = dict(data["submissions"][0], stage=2, draw_id=99)
        data["submissions"].append(extra)
        snapshot = builder.SceneSnapshot.from_dict(data)
        with self.assertRaisesRegex(ValueError, "unambiguous"):
            preview.create_packet(snapshot, snapshot.camera, [snapshot.mesh_instances[0].id], [0])
        with self.assertRaisesRegex(ValueError, "not submitted"):
            preview.create_packet(snapshot, snapshot.camera, [snapshot.mesh_instances[0].id], [0], stage=2)
        with self.assertRaisesRegex(ValueError, "not found"):
            preview.create_packet(snapshot, snapshot.camera, [], [0], render_pass=4, stage=3)


if __name__ == "__main__":
    unittest.main()
