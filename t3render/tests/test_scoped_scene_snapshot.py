import copy
from dataclasses import replace
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


def candidate_fixture():
    scene, records = fixture()
    data = scene.to_dict()
    candidate = copy.deepcopy(data["mesh_instances"][0])
    candidate["id"] = "actor:StaticMeshActor__258"
    data["mesh_instances"].append(candidate)
    scene = builder.SceneSnapshot.from_dict(data)
    observation = next(row for row in records if row["event"] == "scene_bridge_observation")
    entry = copy.deepcopy(observation["target"]["entry"])
    entry.update(list_index=2, address=0x201, owner_candidate=0x301, owner_object_index=6,
                 owner_fname=(259 << 16) | 7, owner_backlink=0x201)
    observation["entries"].append(entry)
    observation.update(count=3, entries_attempted=3)
    observation["target"]["list_scanned"] = 3
    begin = copy.deepcopy(next(row for row in records if row["event"] == "scene_bridge_submission_begin" and row["type"] == 4))
    begin.update(ticket=15, section=1, element=entry["address"], owner=entry["owner_candidate"], owner_index=6,
                 owner_fname=entry["owner_fname"], backlink=entry["address"], mesh_node=0x9100,
                 asset_proxy=0, asset_resource=0, asset_name=None)
    draw = copy.deepcopy(next(row for row in records if row["event"] == "draw" and row["op_id"] == 4))
    draw.update(op_id=6, submission_ticket=15)
    frame = next(row for row in records if row["event"] == "scene_bridge_frame")
    frame.update(submission_started=5, submission_completed=5)
    index = records.index(frame)
    records[index:index] = [begin, draw, {"event": "draw_result", "frame": 3, "op_id": 6, "hr": 0},
                            {"event": "scene_bridge_submission_end", "frame": 3, "ticket": 15,
                             "thread_id": 9, "stable": True}]
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

    def test_automatic_anchor_uses_only_one_unambiguous_verified_scope(self):
        scene, records = fixture()
        automatic, manifest = builder.build(scene, records, 3, 4, 1, None, "b" * 64, "c" * 64)
        explicit, _ = self.build(scene, records)
        self.assertEqual(manifest["anchor_draw"], 4)
        self.assertEqual(automatic.to_dict(), explicit.to_dict())
        ambiguous = copy.deepcopy(records)
        next(row for row in ambiguous if row.get("event") == "draw" and row.get("op_id") == 5)["viewport"]["max_z"] = 0.5
        with self.assertRaisesRegex(ValueError, "unambiguous"):
            builder.build(scene, ambiguous, 3, 4, 1, None, "b" * 64, "c" * 64)
        absent = copy.deepcopy(records)
        target = next(row for row in absent if row.get("event") == "scene_bridge_observation")["target"]
        target.update(list_occurrences=0, entry=None)
        with self.assertRaisesRegex(ValueError, "lack validated"):
            builder.build(scene, absent, 3, 4, 1, None, "b" * 64, "c" * 64)

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

    def test_candidate_mesh_is_opt_in_and_distinctly_limited(self):
        scene, records = candidate_fixture()
        with tempfile.TemporaryDirectory(prefix="t3render-candidate-trace-") as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_text("".join(json.dumps(row) + "\n" for row in records))
            builder.bridge.trace_summary.summarize(path)
        normal, _ = builder.build(scene, records, 3, 4, 1, 4, "b" * 64, "c" * 64)
        snapshot, manifest = builder.build(scene, records, 3, 4, 1, 4, "b" * 64, "c" * 64,
                                           include_asset_candidates=True)
        self.assertEqual(len(normal.mesh_instances), 1)
        self.assertEqual(len(snapshot.mesh_instances), 2)
        self.assertEqual(manifest["offline_asset_candidates"], ["actor:StaticMeshActor__258"])
        self.assertIn(builder.CANDIDATE_LIMITATION, snapshot.source.limitations)
        self.assertEqual(snapshot.submissions[-1].draw_id, 6)
        self.assertEqual(snapshot.submissions[-1].lock_shadow, "unverified")
        self.assertIsNone(snapshot.mesh_instances[1].skin)

    def test_candidate_mesh_requires_unique_actor_matching_transform_and_draw_ranges(self):
        scene, records = candidate_fixture()
        for change in ("transform", "start_index", "vertex_range", "oversized_range", "duplicate", "unstable"):
            candidate_scene = scene
            candidate_records = copy.deepcopy(records)
            if change == "transform":
                data = scene.to_dict()
                data["mesh_instances"][1]["transform"][12] += 1
                candidate_scene = builder.SceneSnapshot.from_dict(data)
            elif change == "duplicate":
                data = scene.to_dict()
                extra = copy.deepcopy(data["mesh_instances"][1])
                extra["id"] = "actor:other:StaticMeshActor__258"
                data["mesh_instances"].append(extra)
                candidate_scene = builder.SceneSnapshot.from_dict(data)
            elif change == "unstable":
                next(row for row in candidate_records if row.get("ticket") == 15 and
                     row["event"] == "scene_bridge_submission_end")["stable"] = False
            else:
                draw = next(row for row in candidate_records if row["event"] == "draw" and row["op_id"] == 6)
                if change == "start_index": draw["start_index"] += 3
                elif change == "vertex_range": draw["num_vertices"] = 1
                else: draw["num_vertices"] = 4
            with self.subTest(change=change):
                snapshot, _ = builder.build(candidate_scene, candidate_records, 3, 4, 1, 4, "b" * 64, "c" * 64,
                                            include_asset_candidates=True)
                self.assertEqual(len(snapshot.mesh_instances), 1)
                self.assertNotIn(6, [sub.draw_id for sub in snapshot.submissions])

    def test_candidate_tolerance_keeps_observed_transform(self):
        scene, records = candidate_fixture()
        data = scene.to_dict()
        data["mesh_instances"][1]["transform"][12] += 0.0005
        snapshot, manifest = builder.build(builder.SceneSnapshot.from_dict(data), records, 3, 4, 1, 4,
                                           "b" * 64, "c" * 64, include_asset_candidates=True)
        candidate = next(item for item in snapshot.mesh_instances if item.id.endswith("StaticMeshActor__258"))
        observed = next(row for row in records if row["event"] == "draw" and row["op_id"] == 6)
        self.assertEqual(candidate.transform, tuple(observed["transforms"]["world"][0]))
        self.assertEqual(manifest["candidate_transform_tolerances"]["translation_t3_units"], 0.001)

    def test_quantized_rotator_candidate_requires_owner_transform_and_is_opt_in(self):
        rotation = (0, -12171, 8)
        location = (3140.000244140625, 1500., 272.)
        def matrix(rot):
            rows = builder.rotator_matrix(*rot)
            return [*rows[0], 0, *rows[1], 0, *rows[2], 0, *location, 1]
        offline = matrix(rotation)
        runtime = matrix(tuple(value & ~3 for value in rotation))
        entry = {"owner_location_bits": bits(location),
                 "owner_rotation_bits": [value & 0xffffffff for value in rotation]}
        self.assertFalse(builder.candidate_transform_matches(offline, runtime))
        self.assertTrue(builder.quantized_rotator_matches(offline, runtime, entry))
        negative_offline, negative_runtime = list(offline), list(runtime)
        for index in (0, 1, 2, 4, 5, 6, 8, 9, 10):
            negative_offline[index] *= -2
            negative_runtime[index] *= -2
        self.assertTrue(builder.quantized_rotator_matches(negative_offline, negative_runtime, entry))
        for change in ("rotation", "location", "offline", "runtime", "missing"):
            observed, old, actual = copy.deepcopy(entry), list(offline), list(runtime)
            if change == "rotation": observed["owner_rotation_bits"][1] += 4
            if change == "location": observed["owner_location_bits"] = bits([location[0]+1, *location[1:]])
            if change == "offline": old[0] += .01
            if change == "runtime": actual[5] += .01
            if change == "missing": observed.pop("owner_rotation_bits")
            with self.subTest(change=change):
                self.assertFalse(builder.quantized_rotator_matches(old, actual, observed))

        scene, records = candidate_fixture()
        data = scene.to_dict()
        data["mesh_instances"][1]["transform"] = offline
        scene = builder.SceneSnapshot.from_dict(data)
        observation = next(row for row in records if row["event"] == "scene_bridge_observation")
        owner = observation["entries"][2]
        owner.update(entry, object_matrix_bits=bits(runtime))
        begin = next(row for row in records if row["event"] == "scene_bridge_submission_begin" and row["ticket"] == 15)
        begin["matrix_bits"] = bits(runtime)
        draw = next(row for row in records if row["event"] == "draw" and row["op_id"] == 6)
        # The trace compares float32 bits, so use the same representable values.
        draw["transforms"]["world"][0] = builder.bridge.decode_floats(bits(runtime), 16)
        strict, _ = builder.build(scene, records, 3, 4, 1, 4, "b" * 64, "c" * 64, include_asset_candidates=True)
        candidate, manifest = builder.build(scene, records, 3, 4, 1, 4, "b" * 64, "c" * 64,
                                            include_asset_candidates=True, allow_quantized_rotators=True)
        self.assertNotIn(6, [item.draw_id for item in strict.submissions])
        self.assertIn(6, [item.draw_id for item in candidate.submissions])
        self.assertEqual(manifest["quantized_rotator_candidates"], ["actor:StaticMeshActor__258"])
        self.assertIn(builder.QUANTIZED_LIMITATION, candidate.source.limitations)
        self.assertEqual(candidate.mesh_instances[1].transform, tuple(draw["transforms"]["world"][0]))
        with self.assertRaisesRegex(ValueError, "require offline asset"):
            builder.build(scene, records, 3, 4, 1, 4, "b" * 64, "c" * 64, allow_quantized_rotators=True)

    def test_self_attached_asset_uses_only_observed_runtime_transform(self):
        original, records = candidate_fixture()
        data = original.to_dict()
        offline_instance = data["mesh_instances"].pop()
        scene = builder.SceneSnapshot.from_dict(data)
        resource = next(x for x in scene.static_mesh_resources if x.id == offline_instance["resource_id"])
        material_names = {x.id: x.name for x in scene.materials}
        assets = {"StaticMeshActor__258": {"target_id": offline_instance["id"], "resource": resource,
                   "material_ids": tuple(offline_instance["material_ids"]),
                   "materials": {mid: material_names[mid] for mid in offline_instance["material_ids"] if mid is not None}}}
        strict, _ = builder.build(scene, records, 3, 4, 1, 4, "b" * 64, "c" * 64, include_asset_candidates=True)
        snapshot, manifest = builder.build(scene, records, 3, 4, 1, 4, "b" * 64, "c" * 64,
                                           include_asset_candidates=True, self_attached_assets=assets)
        self.assertNotIn(6, [item.draw_id for item in strict.submissions])
        self.assertIn(6, [item.draw_id for item in snapshot.submissions])
        self.assertEqual(manifest["self_attached_asset_candidates"], [offline_instance["id"]])
        self.assertIn(builder.SELF_ATTACHED_LIMITATION, snapshot.source.limitations)
        self.assertIn("Where offline actor transforms exist, they matched within 0.001 T3 units of translation and 0.000001 in other matrix entries.",
                      snapshot.source.limitations)
        instance = next(x for x in snapshot.mesh_instances if x.id == offline_instance["id"])
        draw = next(x for x in records if x.get("event") == "draw" and x.get("op_id") == 6)
        self.assertEqual(instance.transform, tuple(draw["transforms"]["world"][0]))
        self.assertIsNone(instance.skin)
        self.assertEqual(instance.material_ids, tuple(offline_instance["material_ids"]))
        bad = copy.deepcopy(records)
        next(x for x in bad if x.get("event") == "draw" and x.get("op_id") == 6)["start_index"] += 3
        rejected, _ = builder.build(scene, bad, 3, 4, 1, 4, "b" * 64, "c" * 64,
                                    include_asset_candidates=True, self_attached_assets=assets)
        self.assertNotIn(6, [item.draw_id for item in rejected.submissions])
        with self.assertRaisesRegex(ValueError, "require offline asset"):
            builder.build(scene, records, 3, 4, 1, 4, "b" * 64, "c" * 64, self_attached_assets=assets)
        conflicting = copy.deepcopy(assets)
        conflicting["StaticMeshActor__258"]["resource"] = replace(resource, name="Different mesh")
        with self.assertRaisesRegex(ValueError, "conflicts with offline mesh resource"):
            builder.build(scene, records, 3, 4, 1, 4, "b" * 64, "c" * 64,
                          include_asset_candidates=True, self_attached_assets=conflicting)
        conflicting = copy.deepcopy(assets)
        conflicting["StaticMeshActor__258"]["materials"]["material:wood"] = "Different material"
        with self.assertRaisesRegex(ValueError, "conflicts with offline material"):
            builder.build(scene, records, 3, 4, 1, 4, "b" * 64, "c" * 64,
                          include_asset_candidates=True, self_attached_assets=conflicting)


if __name__ == "__main__":
    unittest.main()
