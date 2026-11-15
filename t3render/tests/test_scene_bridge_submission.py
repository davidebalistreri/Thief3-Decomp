import copy
import json
from pathlib import Path
import tempfile
import unittest

from test_analyze_scene_bridge import asset_audit, bits, fixture, module


def scoped_fixture(kind="static_mesh"):
    records = fixture()
    header, observation, draw, frame = records[0], records[2], records[3], records[5]
    header["scene_bridge"].update({"submission_scope": "section_call_v1", "submission_install_status": "installed",
                                   "asset_audit": "local_property_v1", "mesh_node": "actor_node_v1",
                                   "mesh_sections": "actor_materials_v1"})
    frame.update({"submission_started": 1, "submission_completed": 1, "submission_pending": 0, "submission_dropped": 0})
    target = observation["target"]
    target["asset"]["audit"] = asset_audit()
    target["mesh_node"] = {
        "pool_scan_complete": True, "pool_scan_truncated": False, "nodes_scanned": 1, "occurrences": 1,
        "address": 0x9000, "vtable": 0x10e49d14, "name_address": 0x9100, "name": "innbed1a1",
        "counts": {"vertices": 3, "triangles": 1, "indices": 3, "sections": 1},
        "bounds_bits": bits([-1, -1, -1, 1, 1, 1]), "stable": True,
        "section_array": 0x9200, "instance_materials": 0x9300, "sections_truncated": False,
        "sections": [{"name": "Default", "material_name": "material", "triangles": 1,
                      "first_vertex": 0, "vertex_count": 3, "first_index": 0, "index_count": 3, "unknown": 0}]}
    entry = target["entry"]
    begin = {"event": "scene_bridge_submission_begin", "frame": 3, "ticket": 11, "thread_id": 9,
             "stage": 1, "section": 0, "self": 42, "pass": 4, "element": entry["address"],
             "vtable": entry["vtable"], "type": 4, "matrix_bits": entry["object_matrix_bits"],
             "owner": entry["owner_candidate"], "owner_index": entry["owner_object_index"],
             "owner_fname": entry["owner_fname"], "backlink": entry["owner_backlink"], "mesh_node": 0x9000,
             "asset_proxy": 0x3000, "asset_resource": 0x4000, "asset_name": "innbed1a1",
             "bsp_source": 0, "bsp_blocks": 0, "bsp_index": 0}
    if kind == "bsp":
        block_base, index = 0x10000, 7
        bsp_entry = copy.deepcopy(entry)
        bsp_entry.update({"list_index": 1, "address": block_base + index * 0x1ac, "vtable": 0x10e4a668, "type": 5})
        observation["entries"].append(bsp_entry)
        observation.update({"count": 2, "entries_attempted": 2})
        target["list_scanned"] = 2
        section = {k: v for k, v in target["mesh_node"]["sections"][0].items() if k != "name"}
        observation["bsp"] = {"renderer": 0x20000, "source": 0x21000, "blocks": block_base,
                              "block_count": 8, "type5_seen": 1, "member_count": 1,
                              "source_stable": True, "candidates_truncated": False,
                              "candidates": [{"list_index": 1, "source_index": index, "address": bsp_entry["address"],
                                  "section_array": 0x22000, "stable": True, "sections_truncated": False,
                                  "counts": {"vertices": 3, "triangles": 1, "indices": 3, "sections": 1},
                                  "bounds_bits": bits([-1, -1, -1, 1, 1, 1]),
                                  "transform_bits": entry["object_matrix_bits"], "sections": [section]}]}
        header["scene_bridge"]["bsp_candidates"] = "vbsp_array_v1"
        begin.update({"element": bsp_entry["address"], "vtable": 0x10e4a668, "type": 5, "owner": 0,
                      "owner_index": 0, "owner_fname": 0, "backlink": 0, "mesh_node": 0,
                      "asset_proxy": 0, "asset_resource": 0, "asset_name": None,
                      "bsp_source": 0x21000, "bsp_blocks": block_base, "bsp_index": index})
    draw["submission_ticket"] = 11
    records.insert(3, begin)
    records.insert(6, {"event": "scene_bridge_submission_end", "frame": 3, "ticket": 11,
                       "thread_id": 9, "stable": True})
    return records


class SubmissionTests(unittest.TestCase):
    def validated(self, records):
        with tempfile.TemporaryDirectory(prefix="t3render-submission-") as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_text("".join(json.dumps(row) + "\n" for row in records))
            return module.read_selected(path, 3, 2)[0]

    def test_static_mesh_and_bsp_are_scoped(self):
        for kind in ("static_mesh", "bsp"):
            with self.subTest(kind=kind):
                records = self.validated(scoped_fixture(kind))
                result = module.analyze(records, 3, 2, 4)
                self.assertEqual(result["submission"]["kind"], kind)
                self.assertEqual(result["submission"]["section"], 0)
                if kind == "static_mesh":
                    self.assertEqual(result["actor_submitted_draw"], "scoped_virtual_call")
                else:
                    self.assertEqual(result["submission"]["runtime_index"], 7)

    def test_changed_identity_unstable_or_incomplete_stays_unknown(self):
        for change in ("owner", "mesh_node", "asset_resource", "matrix", "stable", "dropped", "pass"):
            with self.subTest(change=change):
                records = scoped_fixture()
                if change == "owner": records[3]["owner"] += 1
                elif change == "mesh_node": records[3]["mesh_node"] += 1
                elif change == "asset_resource": records[3]["asset_resource"] += 1
                elif change == "matrix": records[3]["matrix_bits"] = bits([1] * 16)
                elif change == "stable": records[6]["stable"] = False
                elif change == "dropped": records[7]["submission_dropped"] = 1
                else: records[3]["pass"] += 1
                selected = self.validated(records)
                self.assertEqual(module.analyze(selected, 3, 2, 4)["submission"]["status"], "unknown")

    def test_draw_cannot_claim_scope_after_return_or_wrong_thread(self):
        for change in ("returned", "thread", "ticket"):
            with self.subTest(change=change):
                records = scoped_fixture()
                if change == "returned": records.insert(4, records.pop(6))
                elif change == "thread": records[4]["thread_id"] = 10
                else: records[4]["submission_ticket"] = 12
                with self.assertRaises(ValueError):
                    self.validated(records)

    def test_historical_trace_stays_unproved(self):
        self.assertEqual(module.submission_for_draw(fixture(), 2, 4)["status"], "unknown")

    def test_draw_result_after_scope_is_not_direct_proof(self):
        records = scoped_fixture()
        records.insert(5, records.pop(6))
        selected = self.validated(records)
        self.assertEqual(module.submission_for_draw(selected, 2, 4)["status"], "unknown")

    def test_two_indistinguishable_observations_are_ambiguous(self):
        records = scoped_fixture()
        duplicate = copy.deepcopy(records[2])
        duplicate["ticket"] = 2
        records.insert(3, duplicate)
        records[8].update(started=2, completed=2)
        selected = self.validated(records)
        self.assertEqual(module.submission_for_draw(selected, 2, 4)["status"], "unknown")


if __name__ == "__main__":
    unittest.main()
