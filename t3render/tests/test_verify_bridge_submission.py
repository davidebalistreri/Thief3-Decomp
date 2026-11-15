import copy
import json
from pathlib import Path
import tempfile
import unittest

from test_analyze_scene_bridge import bits, module as bridge
from test_scene_bridge_submission import scoped_fixture
import test_scene_ir as ir_test
import verify_bridge_submission as verifier


def complete_fixture():
    records = scoped_fixture("bsp")
    header, observation, frame = records[0], records[2], records[7]
    frame.update(submission_started=4, submission_completed=4)
    target = observation["target"]
    target["outer_count"] = 1
    target["outer"] = [{"address": 1234, "index": 2, "fname": 3, "outer": 0, "member": True,
                        "stable": True, "name": "LOADGAME:SouthQuarter1_int1"}]
    node = target["mesh_node"]
    node["counts"] = {"vertices": 3, "triangles": 3, "indices": 9, "sections": 3}
    node["bounds_bits"] = bits([0, 0, 0, 1, 1, 0])
    node["sections"] = [{"name": "Default", "material_name": "wood", "triangles": 1, "first_vertex": 0,
                         "vertex_count": 3, "first_index": i * 3, "index_count": 3, "unknown": 0}
                        for i in range(3)]
    block = observation["bsp"]["candidates"][0]
    block["counts"] = {"vertices": 3, "triangles": 2, "indices": 6, "sections": 2}
    block["bounds_bits"] = bits([0, 0, 0, 1, 1, 0])
    block["sections"] = [{k: v for k, v in section.items() if k != "name"} for section in node["sections"][:2]]

    data = ir_test.example()
    data["mesh_instances"][0]["id"] = "actor:StaticMeshActor__1520"
    data["mesh_instances"][0]["transform"] = records[4]["transforms"]["world"][0]
    data["mesh_instances"][0]["material_ids"] = ["material:wood"] * 3
    for geometry, count in ((data["static_mesh_resources"][0]["geometry"], 3),
                            (data["bsp_blocks"][0]["geometry"], 2)):
        geometry["indices"] = [0, 1, 2] * count
        geometry["sections"] = [{"name": "Default", "first_index": i * 3, "index_count": 3,
                                 "material_id": "material:wood"} for i in range(count)]
    scene = verifier.SceneSnapshot.from_dict(data)
    mesh_begin = scoped_fixture()[3]
    bsp_begin = records[3]
    template_draw = records[4]
    all_records = [header, records[1], observation]
    for n, (kind, section) in enumerate((("bsp", 0), ("bsp", 1), ("mesh", 1), ("mesh", 2))):
        ticket, op = 11 + n, 2 + n
        begin = copy.deepcopy(bsp_begin if kind == "bsp" else mesh_begin)
        begin.update(ticket=ticket, section=section)
        draw = copy.deepcopy(template_draw)
        draw.update(op_id=op, submission_ticket=ticket, primitive_type=4, min_vertex_index=0,
                    num_vertices=3, start_index=section * 3, primitive_count=1)
        all_records.extend([begin, draw, {"event": "draw_result", "frame": 3, "op_id": op, "hr": 0},
                            {"event": "scene_bridge_submission_end", "frame": 3, "ticket": ticket,
                             "thread_id": 9, "stable": True}])
    all_records.extend([frame, records[8], records[9]])
    return scene, all_records


class VerifySubmissionTests(unittest.TestCase):
    def verify(self, scene, records):
        with tempfile.TemporaryDirectory(prefix="t3render-verify-submission-") as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_text("".join(json.dumps(row) + "\n" for row in records))
            bridge.trace_summary.summarize(path)
        return verifier.verify(scene, records, 3, 4, "b" * 64, "c" * 64)

    def test_scoped_mesh_and_bsp_validate_without_inventing_payload(self):
        scene, records = complete_fixture()
        result = self.verify(scene, records)
        self.assertEqual(result["status"], "validated")
        self.assertEqual(result["validated_stages"], [1])
        self.assertEqual([x["draw"] for x in result["scoped_verified_draws"]], [2, 3, 4, 5])
        self.assertTrue(all(x["same_frame_lock_shadow"] == "unverified" for x in result["scoped_verified_draws"]))
        self.assertEqual(result["resource_key"]["name"], "INNbed1A1")

    def test_wrong_section_or_different_stage_cannot_validate(self):
        scene, records = complete_fixture()
        bad = copy.deepcopy(records)
        bad[4]["start_index"] = 3
        self.assertEqual(self.verify(scene, bad)["status"], "partial")
        bad = copy.deepcopy(records)
        bad[11]["stage"] = 3
        bad[15]["stage"] = 3
        self.assertEqual(self.verify(scene, bad)["status"], "partial")


if __name__ == "__main__":
    unittest.main()
