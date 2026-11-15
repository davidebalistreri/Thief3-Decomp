import copy
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest


TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("analyze_scene_bridge", TOOLS / "analyze_scene_bridge.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def bits(values):
    return [struct.unpack("<I", struct.pack("<f", x))[0] for x in values]


def asset_audit():
    return {"descriptor_table": 0x6000, "descriptor_array": 0x6100, "descriptor": 0x6200,
            "descriptor_vtable": 0x10e72e48, "descriptor_copy": 0x10aeecc0,
            "descriptor_count": 1, "descriptor_size": 4, "descriptor_flags": 0,
            "owner_flags": 0, "property_hash": 0x2000, "bucket_head": 3,
            "slot_index": 3, "slot_next": 0, "slot_id_flags": 0x802b, "slot_value": 0x3000,
            "chain": [{"index": 3, "next": 0, "id_flags": 0x802b, "value": 0x3000}],
            "objects_data": 0x8000, "objects_count": 3, "objects_capacity": 3, "object_index": 2,
            "proxy": 0x3000, "proxy_vtable": 0x10e4dfa8, "proxy_index": 2, "resource": 0x4000,
            "resource_vtable": 0x10ec0e20, "name_address": 0x5004, "name_length": 9}


def fixture():
    matrix = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 4, 5, 6, 1]
    camera = {"address": 100, "stable": True, "source_matrix_bits": bits(matrix),
              "derived_matrix_bits": bits(matrix), "combined_matrix_bits": bits(matrix),
              "projection_bits": bits(matrix), "viewport_bits": [0, 0, 640, 480] + bits([0, 1])}
    entry = {"list_index": 0, "address": 200, "vtable": module.EXPECTED_ELEMENT_VTABLE, "type": 4,
             "flags": 1, "owner_candidate": 300, "owner_object_index": 5, "owner_fname": (1521 << 16) | 7,
             "owner_class": 400, "owner_backlink": 200, "owner_table_member": True, "owner_stable": True,
             "owner_name": "StaticMeshActor", "owner_location_bits": bits([4, 5, 6]),
             "owner_rotation_bits": bits([0, 0, 0]), "object_matrix_bits": bits(matrix)}
    asset = {"status": "candidate", "descriptor_match": True, "local_property_found": True,
             "proxy_live_member": True, "proxy_vtable_match": True, "resource_vtable_match": True,
             "stable": True, "name": "INNbed1A1"}
    owner_class = {"address": 400, "index": 7, "fname": 8, "outer": None, "member": True,
                   "stable": True, "name": "StaticMeshActor"}
    target = {"object_scan_complete": True, "list_scan_complete": True, "object_scan_truncated": False,
              "list_scan_truncated": False, "objects_scanned": 50, "actors_named_target": 1,
              "list_scanned": 1, "list_occurrences": 1, "entry": entry, "owner_class": owner_class,
              "outer_count": 0, "outer_truncated": False, "outer": [], "asset": asset}
    header = {"event": "header", "format": "t3render-d3d8-diagnostic", "version": 1,
              "requested_frames": 1, "operation_results": 1, "clear_rect_limit": 256,
              "scene_bridge": {"requested": True, "install_status": "installed", "probe_limit": 128,
                               "entries_per_probe": 128, "target_object_limit": 65536,
                               "target_list_limit": 8192, "synchronization": "present_interval_and_thread_only",
                               "reentry_detection": "saturating_counter_v1"}}
    observation = {"event": "scene_bridge_observation", "frame": 3, "ticket": 1, "thread_id": 9,
                   "next_d3d_operation": 2, "self": 42, "caller": module.EXPECTED_CALLER, "known_caller": True,
                   "pass": 4, "pass_stable": True, "configured_camera": camera, "current_camera": camera,
                   "culling_camera": camera, "count": 1, "array": 500, "object_table_complete": True,
                   "entries_attempted": 1, "entries_truncated": False, "entries": [entry], "target": target}
    draw = {"event": "draw", "frame": 3, "op_id": 2, "thread_id": 9, "call": "DrawIndexedPrimitive",
            "transforms": {"view": matrix, "projection": matrix, "world": [matrix]},
            "viewport": {"x": 0, "y": 0, "width": 640, "height": 480, "min_z": 0, "max_z": 1}}
    draw_result = {"event": "draw_result", "frame": 3, "op_id": 2, "hr": 0}
    frame = {"event": "scene_bridge_frame", "frame": 3, "status": "synchronized",
             "previous_present_observed": True, "previous_present_thread": 9, "present_thread": 9,
             "draw_thread": 9, "probe_thread": 9, "same_thread": True, "present_tracked": True,
             "epoch_unknown": False, "probe_overlap": False, "probe_reentered": False, "present_overlap": False,
             "operation_overlap": False, "reset_seen": False, "reset_pending": False,
             "started": 1, "completed": 1, "pending": 0, "dropped": 0}
    end = {"event": "frame_end", "frame": 3, "present_hr": 0, "had_reset": False, "concurrent_operations": False}
    return [header, {"event": "frame_begin", "frame": 3}, observation, draw, draw_result, frame, end,
            {"event": "capture_end", "frames": 1}]


class SceneBridgeAnalysisTests(unittest.TestCase):
    def test_complete_candidate_is_not_promoted_to_draw_proof(self):
        out = module.analyze(fixture(), 3, 2, 4)
        self.assertEqual(out["present_interval"], "synchronized")
        self.assertEqual(out["complete_observation_window"], "synchronized")
        self.assertEqual(out["static_caller"], "matches_verified_callsite")
        self.assertEqual(out["actor_membership_and_backlink"], "matched")
        self.assertEqual(out["actor_uniqueness"], "one_in_complete_observed_scans")
        self.assertEqual(out["runtime_asset"], {"status": "candidate", "name": "INNbed1A1", "matches_expected_name": True})
        self.assertEqual(out["actor_transform"]["location_t3_units"], [4, 5, 6])
        self.assertEqual(out["actor_transform"]["rotation_pitch_yaw_roll_units"], [0, 0, 0])
        self.assertEqual(out["actor_transform"]["element_matrix_16"], fixture()[3]["transforms"]["world"][0])
        self.assertTrue(all(value == "equal_bits" for value in out["comparisons"].values()))
        self.assertEqual(out["actor_submitted_draw"], "unknown")
        self.assertNotIn(str(module.EXPECTED_ELEMENT_VTABLE), json.dumps(out))
        self.assertNotIn(str(module.EXPECTED_CALLER), json.dumps(out))

    def test_bad_pass_caller_frame_or_thread_cannot_claim_identity(self):
        changes = ((2, "caller", 0), (2, "pass_stable", False), (2, "pass", 5),
                   (3, "thread_id", 2), (5, "reset_pending", True), (6, "had_reset", True))
        for row, key, value in changes:
            with self.subTest(key=key):
                records = copy.deepcopy(fixture())
                records[row][key] = value
                result = module.analyze(records, 3, 2, 4)
                self.assertEqual(result["actor_membership_and_backlink"], "unknown")
                self.assertEqual(result["runtime_asset"]["status"], "unknown")

    def test_historical_trace_cannot_claim_complete_probe_window(self):
        records = copy.deepcopy(fixture())
        del records[0]["scene_bridge"]["reentry_detection"]
        del records[5]["probe_reentered"]
        result = module.analyze(records, 3, 2, 4)
        self.assertEqual(result["present_interval"], "synchronized")
        self.assertEqual(result["complete_observation_window"], "unknown")
        self.assertEqual(result["actor_membership_and_backlink"], "matched")
        with tempfile.TemporaryDirectory(prefix="t3render-bridge-old-") as directory:
            path = Path(directory) / "old.jsonl"
            path.write_text("".join(json.dumps(r) + "\n" for r in records))
            selected, _ = module.read_selected(path, 3, 2)
            self.assertEqual(module.analyze(selected, 3, 2, 4)["complete_observation_window"], "unknown")

    def test_ambiguous_scans_and_weak_asset_remain_unknown(self):
        for key, value in (("actors_named_target", 2), ("list_occurrences", 2),
                           ("list_scan_truncated", True), ("object_scan_complete", False)):
            with self.subTest(key=key):
                records = copy.deepcopy(fixture())
                records[2]["target"][key] = value
                result = module.analyze(records, 3, 2, 4)
                self.assertEqual(result["actor_uniqueness"], "unknown")
                self.assertEqual(result["runtime_asset"]["status"], "unknown")
        records = copy.deepcopy(fixture())
        records[2]["target"]["asset"]["descriptor_match"] = False
        self.assertEqual(module.analyze(records, 3, 2, 4)["runtime_asset"]["status"], "unknown")
        records[2]["target"]["entry"]["owner_backlink"] = 0
        self.assertEqual(module.analyze(records, 3, 2, 4)["actor_membership_and_backlink"], "unknown")

    def test_new_property_audit_is_consistent_without_promoting_candidate(self):
        records = copy.deepcopy(fixture())
        records[0]["scene_bridge"]["asset_audit"] = "local_property_v1"
        records[2]["target"]["asset"]["audit"] = asset_audit()
        result = module.analyze(records, 3, 2, 4)
        self.assertEqual(result["asset_property_audit"], "consistent")
        self.assertEqual(result["runtime_asset"]["status"], "candidate")
        for change in ("slot_value", "proxy_index", "broken_chain", "missing_audit"):
            with self.subTest(change=change):
                bad = copy.deepcopy(records)
                asset = bad[2]["target"]["asset"]
                if change == "slot_value": asset["audit"]["slot_value"] = 0x9999
                elif change == "proxy_index": asset["audit"]["proxy_index"] = 1
                elif change == "broken_chain": asset["audit"]["chain"][0]["next"] = 3
                else: asset["audit"] = None
                self.assertEqual(module.analyze(bad, 3, 2, 4)["asset_property_audit"], "unknown")
                with tempfile.TemporaryDirectory(prefix="t3render-bridge-audit-") as directory:
                    path = Path(directory) / "trace.jsonl"
                    path.write_text("".join(json.dumps(r) + "\n" for r in bad))
                    with self.assertRaises(ValueError):
                        module.read_selected(path, 3, 2)

    def test_guarded_mesh_node_reports_counts_and_bounds_without_addresses(self):
        records = copy.deepcopy(fixture())
        records[0]["scene_bridge"]["mesh_node"] = "actor_node_v1"
        records[0]["scene_bridge"]["mesh_sections"] = "actor_materials_v1"
        records[2]["target"]["mesh_node"] = {
            "pool_scan_complete": True, "pool_scan_truncated": False, "nodes_scanned": 2,
            "occurrences": 1, "address": 0x9000, "vtable": 0x10e49d14, "name_address": 0x9100,
            "name": "innbed1a1", "counts": {"vertices": 257, "triangles": 236, "indices": 708, "sections": 3},
            "bounds_bits": bits([-1, -2, -3, 1, 2, 3]), "stable": True,
            "section_array": 0x9200, "instance_materials": 0x9300, "sections_truncated": False,
            "sections": [{"name": "Default", "material_name": name, "triangles": count // 3,
                          "first_vertex": 0, "vertex_count": 257, "first_index": first,
                          "index_count": count, "unknown": 0}
                         for name, first, count in (("BF", 0, 24), ("GENgoodnessA_D", 24, 480),
                                                    ("GENiFABRICwoolA2_D", 504, 204))]}
        node = module.analyze(records, 3, 2, 4)["runtime_mesh_node"]
        self.assertEqual(node["status"], "candidate")
        self.assertEqual(node["counts"]["vertices"], 257)
        self.assertEqual(node["bounds_t3_units"], [-1, -2, -3, 1, 2, 3])
        self.assertTrue(node["name_matches_asset"])
        self.assertEqual(node["sections"][1]["material_name"], "GENgoodnessA_D")
        self.assertNotIn(str(0x9000), json.dumps(node))
        records[2]["target"]["mesh_node"]["vtable"] = 0
        self.assertEqual(module.analyze(records, 3, 2, 4)["runtime_mesh_node"]["status"], "unknown")
        with tempfile.TemporaryDirectory(prefix="t3render-bridge-node-") as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_text("".join(json.dumps(r) + "\n" for r in records))
            with self.assertRaises(ValueError):
                module.read_selected(path, 3, 2)

    def test_compare_is_bit_exact_and_rejects_nonfinite(self):
        records = copy.deepcopy(fixture())
        records[3]["transforms"]["world"][0][12] = 10
        result = module.analyze(records, 3, 2, 4)
        self.assertEqual(result["comparisons"]["object_matrix_vs_draw_world"], "different")
        records[3]["transforms"]["view"][0] = float("nan")
        self.assertEqual(module.analyze(records, 3, 2, 4)["comparisons"]["camera_derived_vs_draw_view"], "unknown")

    def test_signed_rotator_and_casefold_asset_are_checked(self):
        records = copy.deepcopy(fixture())
        records[2]["target"]["entry"]["owner_rotation_bits"] = [0xffffffff, 2, 0x80000000]
        records[2]["target"]["asset"]["name"] = "innbed1a1"
        result = module.analyze(records, 3, 2, 4)
        self.assertEqual(result["actor_transform"]["rotation_pitch_yaw_roll_units"], [-1, 2, -2147483648])
        self.assertTrue(result["runtime_asset"]["matches_expected_name"])
        records[2]["target"]["asset"]["name"] = "different"
        self.assertFalse(module.analyze(records, 3, 2, 4)["runtime_asset"]["matches_expected_name"])

    def test_intervening_probe_replaces_temporal_candidate(self):
        records = copy.deepcopy(fixture())
        later = copy.deepcopy(records[2])
        later["pass"] = 8
        records.insert(3, later)
        result = module.analyze(records, 3, 2, 4)
        self.assertEqual(result["pass"], "unknown")
        self.assertEqual(result["actor_membership_and_backlink"], "unknown")

    def test_real_trace_validator_and_selected_reader(self):
        with tempfile.TemporaryDirectory(prefix="t3render-bridge-test-") as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_text("".join(json.dumps(r) + "\n" for r in fixture()))
            records, sha = module.read_selected(path, 3, 2)
            self.assertEqual(len(sha), 64)
            self.assertEqual(module.analyze(records, 3, 2, 4)["runtime_asset"]["status"], "candidate")
            path.write_text(path.read_text().replace('"status": "synchronized"', '"status": "synchronized", "status": "unknown"'))
            with self.assertRaises(ValueError):
                module.read_selected(path, 3, 2)


if __name__ == "__main__":
    unittest.main()
