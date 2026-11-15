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
                               "target_list_limit": 8192, "synchronization": "present_interval_and_thread_only"}}
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
             "epoch_unknown": False, "probe_overlap": False, "present_overlap": False,
             "operation_overlap": False, "reset_seen": False, "reset_pending": False,
             "started": 1, "completed": 1, "pending": 0, "dropped": 0}
    end = {"event": "frame_end", "frame": 3, "present_hr": 0, "had_reset": False, "concurrent_operations": False}
    return [header, {"event": "frame_begin", "frame": 3}, observation, draw, draw_result, frame, end,
            {"event": "capture_end", "frames": 1}]


class SceneBridgeAnalysisTests(unittest.TestCase):
    def test_complete_candidate_is_not_promoted_to_draw_proof(self):
        out = module.analyze(fixture(), 3, 2, 4)
        self.assertEqual(out["present_interval"], "synchronized")
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
