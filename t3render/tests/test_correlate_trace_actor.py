import importlib.util
from pathlib import Path
import struct
import sys
import unittest


TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("correlate_trace_actor", TOOLS / "correlate_trace_actor.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

from t3mesh import RenderMesh, Section, Skin, StaticMesh
from synthetic_d3d8_tokens import packed_declaration


class CorrelationTest(unittest.TestCase):
    def setUp(self):
        self.actor = {"name": "bed", "mesh": "mesh", "skin": "room", "location": [4, 5, 6],
                      "rotation": [0, 0, 0], "draw_scale": 1}
        render = RenderMesh((0, 0, 0), (1, 1, 1), 1, (0, 0, 0), positions=[(0, 0, 0)] * 3,
                            indices=[0, 1, 2], sections=[Section("s", 1, 0, 3, 0, 3, 0)])
        self.mesh = StaticMesh("mesh", [Skin("room", ["fabric"])], [0], 0, [], render)
        self.draw = {"event": "draw", "frame": 10, "op_id": 1, "call": "DrawIndexedPrimitive",
                     "primitive_type": 4, "primitive_count": 1, "num_vertices": 3, "start_index": 0,
                     "transforms": {"world": [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 4, 5, 6, 1]]}}
        self.result = {"event": "draw_result", "frame": 10, "op_id": 1, "hr": 0, "buffer_stable": True}
        self.end = {"event": "frame_end", "frame": 10, "present_hr": 0}

    def correlate(self, actors=None, records=None):
        return module.correlate(actors or [self.actor], "bed", self.mesh,
                                records or [self.draw, self.result, self.end], 10, 0.1)

    def test_matching_transform_and_section_are_candidates(self):
        result = self.correlate()
        self.assertEqual(result["actors_within_tolerance"], ["bed"])
        self.assertEqual(len(result["matches"]), 1)
        self.assertEqual(result["matches"][0]["material"], "fabric")
        self.assertEqual(result["matches"][0]["draw_hresult"], 0)
        self.assertIn("offline", result["limitations"][0])

    def test_ambiguous_position_is_reported(self):
        other = dict(self.actor, name="other", location=[4.01, 5, 6])
        self.assertEqual(self.correlate([self.actor, other])["actors_within_tolerance"], ["bed", "other"])

    def test_rejects_incompatible_transform_and_section(self):
        for changed in (dict(self.draw, start_index=1), dict(self.draw, primitive_count=True),
                        dict(self.draw, transforms={"world": [[2, 0, 0, 0, 0, 1, 0, 0,
                                                                 0, 0, 1, 0, 4, 5, 6, 1]]})):
            with self.subTest(changed=changed):
                self.assertEqual(self.correlate(records=[changed, self.result, self.end])["matches"], [])

    def test_unsupported_actor_or_missing_frame_is_not_silently_accepted(self):
        for actor in (dict(self.actor, rotation=[0, 1, 0]), dict(self.actor, draw_scale=True),
                      dict(self.actor, skin="missing")):
            with self.subTest(actor=actor), self.assertRaises(ValueError):
                self.correlate([actor])
        with self.assertRaises(ValueError):
            self.correlate(records=[self.draw, self.result])

    def test_payload_comparison_checks_values_and_declared_layout(self):
        self.mesh.render.positions = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        self.mesh.render.uv0 = [(0, 0), (1, 0), (0, 1)]
        vertices = b"".join(struct.pack("<3fI2fII", *pos, 0, *uv, 0, 0)
                            for pos, uv in zip(self.mesh.render.positions, self.mesh.render.uv0))
        payload = {"vertex": {"status": "captured", "stride_bytes": 32, "vertex_count": 3,
                              "data": vertices.hex()},
                   "index": {"status": "captured", "index_format": "uint16", "index_count": 3,
                             "data": struct.pack("<3H", 0, 1, 2).hex()}}
        declaration = {"status": "candidate", "words": packed_declaration()}
        draw = dict(self.draw, min_vertex_index=0)
        result = module.compare_payloads(self.mesh, self.mesh.render.sections[0], draw, payload, declaration)
        self.assertEqual(result, {"status": "compared", "position_bits_equal": True,
                                  "uv0_bits_equal": True, "indices_u16_equal": True})
        payload["index"]["data"] = struct.pack("<3H", 0, 2, 1).hex()
        self.assertFalse(module.compare_payloads(self.mesh, self.mesh.render.sections[0], draw,
                                                 payload, declaration)["indices_u16_equal"])
        declaration["words"] = []
        self.assertEqual(module.compare_payloads(self.mesh, self.mesh.render.sections[0], draw,
                                                 payload, declaration), {"status": "unavailable"})


if __name__ == "__main__":
    unittest.main()
