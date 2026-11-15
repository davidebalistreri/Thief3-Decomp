import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "ir"))
sys.path.insert(0, str(ROOT / "tests"))
import compare_bridge_fixture as comparison
import scene_ir
import test_analyze_scene_bridge as bridge_test
import test_scene_ir as ir_test


def observed():
    records = bridge_test.fixture()
    records[0]["scene_bridge"]["mesh_node"] = "actor_node_v1"
    records[2]["target"]["mesh_node"] = {
        "pool_scan_complete": True, "pool_scan_truncated": False, "nodes_scanned": 1,
        "occurrences": 1, "address": 0x9000, "vtable": 0x10e49d14, "name_address": 0x9100,
        "name": "innbed1a1", "counts": {"vertices": 3, "triangles": 1, "indices": 3, "sections": 1},
        "bounds_bits": bridge_test.bits([0, 0, 0, 1, 1, 0]), "stable": True}
    return records


def offline():
    data = ir_test.example()
    data["mesh_instances"][0]["transform"] = bridge_test.fixture()[3]["transforms"]["world"][0][:]
    return scene_ir.SceneSnapshot.from_dict(data)


class CompareBridgeFixtureTests(unittest.TestCase):
    def test_comparison_remains_partial_with_matching_counts_and_transform(self):
        result = comparison.compare(offline(), observed(), 3, 2, 4, "actor:bed", [0])
        self.assertEqual(result["status"], "partial")
        self.assertEqual(result["complete_observation_window"], "synchronized")
        self.assertEqual(result["actor"]["transform"]["status"], "match")
        self.assertEqual(result["mesh"]["vertex_count"]["status"], "match")
        self.assertEqual(result["mesh"]["index_count"]["status"], "match")
        self.assertEqual(result["mesh"]["section_count"]["status"], "match")
        self.assertEqual(result["mesh"]["bounds"]["status"], "match")
        self.assertEqual(result["mesh"]["resource_id_match"], "unknown")
        self.assertIsNone(result["mesh"]["runtime_material_sections"])
        self.assertEqual(result["offline_bsp_blocks"][0]["runtime"], "unknown")
        for component, expected in zip(result["camera"]["runtime_position_t3_units"], [-4, -5, -6]):
            self.assertAlmostEqual(component, expected)

    def test_signed_zero_is_semantically_equal_but_not_bitwise_equal(self):
        self.assertEqual(comparison.compare_f32([0.0], [-0.0]), "match")
        self.assertEqual(comparison.compare_f32([0.0], [-0.0], bitwise=True), "different")
        self.assertEqual(comparison.compare_f32([1.0], [1.000001]), "different")
        self.assertEqual(comparison.compare_f32([1.0], None), "unknown")

    def test_captured_section_materials_are_compared_without_inventing_missing_names(self):
        records = observed()
        node = records[2]["target"]["mesh_node"]
        node.update(section_array=0x9200, instance_materials=0x9300, sections_truncated=False,
                    sections=[{"name": "Default", "material_name": "wood", "triangles": 1,
                               "first_vertex": 0, "vertex_count": 3, "first_index": 0,
                               "index_count": 3, "unknown": 0}])
        result = comparison.compare(offline(), records, 3, 2, 4, "actor:bed")
        self.assertEqual(result["mesh"]["material_sections_match"], "match")
        node["sections"][0]["material_name"] = None
        self.assertEqual(comparison.compare(offline(), records, 3, 2, 4, "actor:bed")["mesh"]["material_sections_match"], "unknown")
        node["sections"][0]["material_name"] = "stone"
        self.assertEqual(comparison.compare(offline(), records, 3, 2, 4, "actor:bed")["mesh"]["material_sections_match"], "different")

    def test_bsp_array_member_is_a_candidate_not_an_offline_identity_proof(self):
        records = observed()
        observation = records[2]
        observation["count"] = 2
        observation["entries_attempted"] = 2
        observation["target"]["list_scanned"] = 2
        address = 0x17004 + 7 * 0x1ac
        observation["entries"].append({"list_index": 1, "address": address, "vtable": 0x10e4a668,
                                       "type": 5, "flags": 1})
        observation["bsp"] = {"renderer": 0x15000, "source": 0x16000, "blocks": 0x17004,
                              "block_count": 8, "type5_seen": 1, "member_count": 1,
                              "source_stable": True, "candidates_truncated": False,
                              "candidates": [{"list_index": 1, "source_index": 7, "address": address,
                                              "counts": {"vertices": 3, "triangles": 1, "indices": 3, "sections": 1},
                                              "bounds_bits": bridge_test.bits([0, 0, 0, 1, 1, 0]),
                                              "transform_bits": bridge_test.bits(ir_test.IDENTITY), "stable": True,
                                              "section_array": 0x18004, "sections_truncated": False,
                                              "sections": [{"material_name": "wood", "triangles": 1,
                                                            "first_vertex": 0, "vertex_count": 3,
                                                            "first_index": 0, "index_count": 3, "unknown": 0}]}]}
        data = ir_test.example()
        data["mesh_instances"][0]["transform"] = records[3]["transforms"]["world"][0][:]
        data["bsp_blocks"][0]["geometry"]["sections"][0]["material_id"] = "material:wood"
        result = comparison.compare(scene_ir.SceneSnapshot.from_dict(data), records, 3, 2, 4, "actor:bed", [0])
        block = result["offline_bsp_blocks"][0]
        self.assertEqual(block["runtime"], "unique_metadata_candidate")
        candidate = block["runtime_candidates"][0]
        self.assertEqual(candidate["runtime_source_index"], 7)
        self.assertEqual(candidate["checks"]["vertex_count"], "match")
        self.assertEqual(candidate["checks"]["material_sections"], "match")
        self.assertEqual(candidate["checks"]["section_vertex_range_contains_indices"], "match")
        self.assertEqual(result["status"], "partial")
        duplicate = copy.deepcopy(data["bsp_blocks"][0])
        duplicate.update(id="bsp:1", source_index=1)
        data["bsp_blocks"].append(duplicate)
        ambiguous = comparison.compare(scene_ir.SceneSnapshot.from_dict(data), records, 3, 2, 4, "actor:bed", [0])
        self.assertEqual(ambiguous["offline_bsp_blocks"][0]["runtime"], "ambiguous_metadata_candidates")
        self.assertEqual(ambiguous["offline_bsp_blocks"][0]["runtime_candidates"][0]["possible_offline_blocks"], 2)
        data["bsp_blocks"].pop()
        observation["bsp"]["candidates_truncated"] = True
        truncated = comparison.compare(scene_ir.SceneSnapshot.from_dict(data), records, 3, 2, 4, "actor:bed", [0])
        self.assertEqual(truncated["offline_bsp_blocks"][0]["runtime"], "unknown")
        observation["bsp"]["candidates_truncated"] = False
        observation["entries"][0]["type"] = None
        incomplete = comparison.compare(scene_ir.SceneSnapshot.from_dict(data), records, 3, 2, 4, "actor:bed", [0])
        self.assertEqual(incomplete["offline_bsp_blocks"][0]["runtime"], "unknown")
        observation["entries"][0]["type"] = 4
        observation["bsp"]["candidates"][0]["sections"][0]["material_name"] = None
        missing = comparison.compare(scene_ir.SceneSnapshot.from_dict(data), records, 3, 2, 4, "actor:bed", [0])
        self.assertEqual(missing["offline_bsp_blocks"][0]["runtime"], "unknown")
        observation["bsp"]["candidates"][0]["sections"][0]["material_name"] = "wood"
        observation["bsp"]["candidates"][0]["address"] += 4
        result = comparison.compare(scene_ir.SceneSnapshot.from_dict(data), records, 3, 2, 4, "actor:bed", [0])
        self.assertEqual(result["offline_bsp_blocks"][0]["runtime"], "unknown")

    def test_missing_reentry_check_cannot_establish_complete_window(self):
        records = observed()
        records[5].pop("probe_reentered")
        result = comparison.compare(offline(), records, 3, 2, 4, "actor:bed")
        self.assertEqual(result["present_interval"], "synchronized")
        self.assertEqual(result["complete_observation_window"], "unknown")
        self.assertEqual(result["status"], "partial")

    def test_camera_requires_invertible_affine_view(self):
        self.assertIsNone(comparison.camera_position([0] * 16))
        perspective = ir_test.IDENTITY[:]
        perspective[3] = 1
        self.assertIsNone(comparison.camera_position(perspective))
        self.assertEqual(comparison.camera_position(ir_test.IDENTITY), [0, 0, 0])

    def test_cli_validates_trace_and_does_not_emit_addresses(self):
        with tempfile.TemporaryDirectory(prefix="t3render-compare-") as directory:
            trace = Path(directory) / "trace.jsonl"
            scene = Path(directory) / "scene.json"
            trace.write_text("".join(json.dumps(r) + "\n" for r in observed()))
            scene.write_text(offline().to_json())
            command = [sys.executable, "-B", str(ROOT / "tools" / "compare_bridge_fixture.py"), str(trace),
                       "--scene", str(scene), "--frame", "3", "--draw", "2", "--pass", "4",
                       "--instance", "actor:bed", "--bsp", "0"]
            run = subprocess.run(command, text=True, capture_output=True, check=True)
            result = json.loads(run.stdout)
            self.assertEqual(result["mesh"]["vertex_count"]["status"], "match")
            self.assertNotIn(str(0x9000), run.stdout)

    def test_runtime_scene_is_not_accepted_as_offline_fixture(self):
        data = ir_test.example()
        data["source"].update(kind="runtime", frame_id=3)
        with self.assertRaisesRegex(ValueError, "source=offline"):
            comparison.compare(scene_ir.SceneSnapshot.from_dict(data), observed(), 3, 2, 4, "actor:bed")


if __name__ == "__main__":
    unittest.main()
