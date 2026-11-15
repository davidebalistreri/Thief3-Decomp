"""SDK shadow geometry stays an explicitly offline, bounded candidate."""
import copy
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import scene_shadow_candidates as shadows
from test_import_t3_scene import mesh
from test_scene_ir import IDENTITY
from test_scoped_scene_snapshot import fixture
import build_scoped_scene_snapshot as scoped


class Bundle:
    def __init__(self):
        self.items = [SimpleNamespace(name="Bed")]
    def of_type(self, _kind):
        return self.items
    def split(self, _resource):
        return []


class ShadowCandidatesTests(unittest.TestCase):
    def setUp(self):
        self.mesh = mesh()
        self.mesh.render.shadow_positions = [(0., 0., 0.), (1., 0., 0.), (0., 1., 0.)]
        self.mesh.render.shadow_normals = [(0., 0., 1.)] * 3
        self.mesh.render.shadow_indices = [0, 1, 2, 0, 2, 1]
        self.prefix = "TestMap:" + "a" * 64
        resource_id = "mesh:" + self.prefix + ":" + "b" * 64 + ":bed"
        geometry = shadows.Geometry.parse(shadows.importer.geometry(self.mesh.render, [None, None]), "mesh")
        resource = SimpleNamespace(id=resource_id, name="Bed", geometry=geometry)
        instances = [SimpleNamespace(id=f"actor:{self.prefix}:{name}", resource_id=resource_id, transform=tuple(IDENTITY))
                     for name in ("First", "Second", "Disabled")]
        bsp_geometry = shadows.Geometry.parse(shadows.importer.geometry(self.mesh.render, ["material:A", "material:B"]), "bsp")
        self.block = SimpleNamespace(id="bsp:test", source_index=0, source_offset=120, block_id=3, geometry=bsp_geometry)
        self.scene = SimpleNamespace(source=SimpleNamespace(map="TestMap", fingerprints={"map": "a" * 64, "bundle": "b" * 64}),
                                     static_mesh_resources=[resource], mesh_instances=instances, bsp_blocks=[self.block])
        self.actors = {name: {"mesh": "Bed", "casts_shadows": name != "Disabled"}
                       for name in ("First", "Second", "Disabled")}
        self.blocks = [SimpleNamespace(offset=120, block_id=3, mesh=self.mesh.render)]
        self.bundle = Bundle()

    def assemble(self):
        with mock.patch.object(shadows, "parse_static_mesh", return_value=self.mesh):
            return shadows.assemble(self.scene, self.actors, self.blocks, [("bundle", self.bundle)])

    def test_shared_mesh_and_world_bsp_keep_runtime_transforms_and_offline_flags(self):
        resources, casters, omitted = self.assemble()
        self.assertEqual(len(resources), 2)
        self.assertEqual(len(casters), 3)
        self.assertEqual(casters[0]["resource_id"], casters[1]["resource_id"])
        self.assertEqual(casters[0]["transform"], IDENTITY)
        self.assertEqual(casters[-1]["target_kind"], "bsp_block")
        self.assertEqual(omitted[0]["reason"], "offline_actor_disabled")
        self.assertEqual(resources[0]["indices"], [0, 1, 2, 0, 2, 1])

    def test_missing_ambiguous_or_mismatched_asset_is_rejected(self):
        self.bundle.items.append(SimpleNamespace(name="BED"))
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            self.assemble()
        self.bundle.items.clear()
        with self.assertRaisesRegex(ValueError, "missing"):
            self.assemble()
        self.bundle.items = [SimpleNamespace(name="Bed")]
        self.mesh.render.positions[0] = (5, 0, 0)
        with self.assertRaisesRegex(ValueError, "geometry differs"):
            self.assemble()

    def test_actor_and_bsp_mismatch_are_rejected(self):
        self.actors["First"]["mesh"] = "Other"
        with self.assertRaisesRegex(ValueError, "actor"):
            self.assemble()
        self.actors["First"]["mesh"] = "Bed"
        self.blocks[0].offset += 1
        with self.assertRaisesRegex(ValueError, "BSP identity"):
            self.assemble()

    def test_invalid_volume_and_transform_fail_closed(self):
        original = copy.deepcopy(self.mesh.render)
        for change in ("length", "index", "nan", "nontriangle"):
            render = copy.deepcopy(original)
            if change == "length": render.shadow_normals.pop()
            elif change == "index": render.shadow_indices[0] = 999
            elif change == "nan": render.shadow_positions[0] = (float("nan"), 0, 0)
            else: render.shadow_indices.pop()
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, "shadow volume"):
                shadows.volume(render)
        self.assertFalse(shadows.affine(IDENTITY))
        reflected = IDENTITY[:]; reflected[0] = -1
        self.assertTrue(shadows.affine(reflected))
        singular = IDENTITY[:]; singular[0] = 0
        with self.assertRaisesRegex(ValueError, "singular"):
            shadows.affine(singular)
        projective = IDENTITY[:]; projective[3] = 1
        with self.assertRaisesRegex(ValueError, "affine"):
            shadows.affine(projective)

    def test_changed_scene_bytes_or_map_fingerprint_never_reach_asset_parsing(self):
        offline, records = fixture()
        scene, _ = scoped.build(offline, records, 3, 4, 1, 4, "b" * 64, "c" * 64)
        offline_bytes = offline.to_json().encode()
        altered = offline.to_dict()
        altered["materials"][0]["name"] = "different"
        with mock.patch.object(shadows.states, "from_trace") as state, \
             self.assertRaisesRegex(ValueError, "scene bytes differ"):
            shadows.extract(scene, scene.to_json().encode(), offline, shadows.SceneSnapshot.from_dict(altered).to_json().encode(),
                            "trace", "actors", Path("game"), 4, 1)
        state.assert_not_called()
        with tempfile.TemporaryDirectory(prefix="t3render-shadow-fingerprint-") as directory:
            path = Path(directory) / "map.gmp"
            path.write_bytes(b"different map")
            with mock.patch.object(shadows.states, "from_trace") as state, \
                 mock.patch.object(shadows.direct, "offline_actors", return_value={}), \
                 mock.patch.object(shadows, "resolve_map", return_value=path), \
                 mock.patch.object(shadows, "Package") as parser, \
                 self.assertRaisesRegex(ValueError, "map fingerprint mismatch"):
                shadows.extract(scene, scene.to_json().encode(), offline, offline_bytes,
                                "trace", "actors", Path(directory), 4, 1)
            state.assert_called_once()
            parser.assert_not_called()


if __name__ == "__main__":
    unittest.main()
