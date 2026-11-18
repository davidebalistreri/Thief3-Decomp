"""Compare live BSP material ranges with the established SceneIR selector."""
from dataclasses import replace
from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "ir"))
import scene_ir
import scene_material_overrides as overrides
from test_scene_material_overrides import scene_fixture


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


class LiveMaterialSelectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("host C++ compiler required")
        cls.temp = tempfile.TemporaryDirectory(prefix="t3render-live-materials-")
        cls.binary = Path(cls.temp.name) / "fixture"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(HERE / "scene_live_materials_fixture.cpp"),
                        str(ROOT / "src/scene_live_materials.cpp"), "-o", str(cls.binary)],
                       check=True, capture_output=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def compare(self, scene):
        anchor_id = f"actor:{scene.source.map}:{scene.source.fingerprints['map']}:{overrides.ACTOR}"
        anchor = next(item for item in scene.mesh_instances if item.id == anchor_id)
        anchor = replace(anchor, transform=tuple(map(f32, anchor.transform)))
        scene = replace(scene, mesh_instances=tuple(anchor if i.id == anchor_id else i for i in scene.mesh_instances))
        selector = overrides.BedroomSelection(scene)
        materials = {material.id: material.name for material in scene.materials}
        sections = replacements = 0
        for block in scene.bsp_blocks:
            geometry = block.geometry
            positions = [tuple(map(f32, p)) for p in geometry.positions]
            for part in geometry.sections:
                name = materials.get(part.material_id, "unknown")
                if name.lower() not in overrides.MATERIALS:
                    continue
                selected = geometry.indices[part.first_index:part.first_index + part.index_count]
                expected = selector.partition("bsp_block", name, [positions[index] for index in selected])
                expected = [(part.first_index + first, end-first,
                             "remaster:material:" + value if value else "-")
                            for first, end, value in expected]
                values = [*anchor.transform, len(positions), len(geometry.indices), part.first_index, part.index_count, name]
                values.extend(value for point in positions for value in point)
                values.extend(geometry.indices)
                result = subprocess.run([str(self.binary)], input=" ".join(map(str, values)), text=True,
                                        check=True, capture_output=True, timeout=30)
                actual = [(int(a), int(b), material) for a, b, material in
                          (line.split() for line in result.stdout.splitlines())]
                self.assertEqual(actual, expected, (block.id, name))
                sections += 1
                replacements += sum(count//3 for _, count, material in actual if material != "-")
        self.assertGreater(sections, 0)
        return sections, replacements

    def test_synthetic_floor_wall_ceiling_and_outside(self):
        sections, replacements = self.compare(scene_fixture())
        self.assertEqual((sections, replacements), (2, 2))

    def test_head_wall_is_separate_from_side_wall_and_outside(self):
        matrix = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
        # T3 local metres: the first plane is behind the headboard, the
        # second is the side wall, and the last is outside the bedroom bounds.
        points = [(-.11, -.5, .2), (-.11, .5, .2), (-.11, -.5, 1.8),
                  (.5, -2, .2), (1.5, -2, .2), (.5, -2, 1.8),
                  (-.5, -.5, .2), (-.5, .5, .2), (-.5, -.5, 1.8)]
        values = [*matrix, len(points), 9, 0, 9, 'STMwall512D_D']
        values.extend(value / overrides.SCALE for point in points for value in point)
        values.extend(range(9))
        result = subprocess.run([str(self.binary), '--head-wall'], input=' '.join(map(str, values)),
                                text=True, check=True, capture_output=True, timeout=30)
        self.assertEqual(result.stdout.splitlines(), [
            '0 3 remaster:material:stone_wall_v2 1',
            '3 3 remaster:material:stone_wall_v2 0',
            '6 3 - 0'])

    @unittest.skipUnless(os.environ.get("T3RENDER_CAPTURED_SCENE"), "local captured SceneIR not supplied")
    def test_local_capture_matches_previous_sceneir_selector(self):
        path = Path(os.environ["T3RENDER_CAPTURED_SCENE"])
        scene = scene_ir.SceneSnapshot.from_json(path.read_bytes())
        self.assertEqual(scene.source.kind, "runtime")
        self.assertTrue(scene.source.camera_synchronized)
        sections, replacements = self.compare(scene)
        self.assertGreater(replacements, 0)
        print(f"captured_scene_material_comparison=ok sections={sections} triangles={replacements}")


if __name__ == "__main__":
    unittest.main()
