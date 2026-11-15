"""Shadow batches use semantic lights and transformed SDK volume candidates."""
from pathlib import Path
import sys
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import scene_shadow_render as shadows
import scene_stencil_fixture as fixture
from test_scene_ir import IDENTITY


class ShadowRenderTests(unittest.TestCase):
    def setUp(self):
        vertices = fixture.cube_volume()
        resource = {"id": "shadow:box", "positions": [p for p, _ in vertices],
                    "face_normals": [n for _, n in vertices], "indices": list(range(len(vertices)))}
        caster = {"target_kind": "mesh_instance", "target_id": "actor:map:box", "resource_id": resource["id"],
                  "transform": IDENTITY, "reflected": False}
        self.document = {"resources": [resource], "casters": [caster]}
        self.light = {"offline_light_id": "light:map:lamp", "mode": "point_approximation", "position": [-2, 0, 1]}
        self.candidates = {2: self.light, 3: dict(self.light)}
        self.actors = {"lamp": {"attached_to": None}}

    def test_builds_world_vertices_and_skips_enclosing_or_own_casters(self):
        result = shadows.prepare(self.document, self.candidates, ["light:map:lamp"], self.actors)
        batch = result["light:map:lamp"]
        self.assertEqual(len(batch["vertices"]), len(fixture.cube_volume()))
        self.assertEqual(batch["casters"], ["actor:map:box"])
        self.assertEqual(batch["position"], (-2, 0, 1))
        positions = [p for p, _ in fixture.cube_volume()]
        normals = [n for _, n in fixture.cube_volume()]
        self.assertTrue(shadows._encloses(positions, normals, list(range(len(positions))), (0, 0, 2.25)))
        self.assertFalse(shadows._encloses(positions, normals, list(range(len(positions))), (-2, 0, 1)))
        with self.assertRaisesRegex(ValueError, "no candidate caster"):
            shadows.prepare(self.document, {2: dict(self.light, position=[0, 0, 2.25])},
                            ["light:map:lamp"], self.actors)
        with self.assertRaisesRegex(ValueError, "no candidate caster"):
            shadows.prepare(self.document, self.candidates, ["light:map:lamp"], {"lamp": {"attached_to": "box"}})

    def test_ambiguous_light_positions_and_unknown_ids_are_rejected(self):
        for candidates, selected in ((self.candidates, ["light:map:other"]),
                                     ({2: self.light, 3: dict(self.light, position=[-3, 0, 1])}, ["light:map:lamp"]),
                                     (self.candidates, ["light:map:lamp", "light:map:lamp"])):
            with self.subTest(selected=selected), self.assertRaises(ValueError):
                shadows.prepare(self.document, candidates, selected, self.actors)

    def test_expanded_volume_budget_is_enforced_before_copying(self):
        with mock.patch.object(shadows, "MAX_EXPANDED_VERTICES", 3), \
             self.assertRaisesRegex(ValueError, "exceed renderer bounds"):
            shadows.prepare(self.document, self.candidates, ["light:map:lamp"], self.actors)

    def test_reflected_transform_reverses_winding_and_transforms_normals(self):
        matrix = IDENTITY[:]
        matrix[0], matrix[5], matrix[12] = -2, 3, 10
        self.document["casters"][0].update(transform=matrix, reflected=True)
        batch = shadows.prepare(self.document, self.candidates, ["light:map:lamp"], self.actors)["light:map:lamp"]
        positions = self.document["resources"][0]["positions"]
        for out, index in zip(batch["vertices"][:3], (0, 2, 1)):
            p = positions[index]
            self.assertEqual(out[:3], (10-2*p[0], 3*p[1], p[2]))
            self.assertAlmostEqual(sum(x*x for x in out[5:8]), 1)


if __name__ == "__main__":
    unittest.main()
