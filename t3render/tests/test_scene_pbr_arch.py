"""The bedroom arch keeps its stone section while replacing the rosette."""
from dataclasses import replace
import json
from pathlib import Path
import sys
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import scene_pbr_arch as arch
import scene_pbr_packet as pbr
import test_scene_pbr_candlestick as candle_tests
import test_scene_pbr_packet as packet_tests
from scene_ir import MaterialRef
from test_scene_material_overrides import FakeRemaster
from test_scene_textured_packet import Resolver


def inputs():
    scene, lights, config = packet_tests.inputs()
    bed_instance, bed_resource = scene.mesh_instances[0], scene.static_mesh_resources[0]
    identity = f"actor:{scene.source.map}:{scene.source.fingerprints['map']}:StaticMeshActor__59"
    geometry = bed_resource.geometry
    sections = (replace(geometry.sections[0], first_index=0, material_id="material:rosette"),
                replace(geometry.sections[1], first_index=3, material_id="material:wall"))
    resource = replace(bed_resource, id="mesh:arch", name="GENsknDOORFRMarc4x10half",
                       geometry=replace(geometry, indices=geometry.indices[:6], sections=sections))
    instance = replace(bed_instance, id=identity, resource_id=resource.id,
                       material_ids=("material:rosette", "material:wall"))
    previous = [s for s in scene.submissions if s.target_id == bed_instance.id]
    submissions = tuple(replace(s, target_id=identity, section=i) for i, s in enumerate(previous))
    scene = replace(scene, static_mesh_resources=scene.static_mesh_resources+(resource,),
                    mesh_instances=scene.mesh_instances+(instance,), submissions=scene.submissions+submissions,
                    materials=scene.materials+(MaterialRef("material:rosette", "CIVGoodness_D"),))
    lights = replace(lights, mesh_instances=scene.mesh_instances, static_mesh_resources=scene.static_mesh_resources)
    return scene, lights, config


class ArchSelectionTests(unittest.TestCase):
    def test_requires_both_sections_and_places_generated_mesh_with_actor(self):
        scene, _, _ = inputs()
        raw = candle_tests.glb(scene, names=arch.MATERIALS)
        parts, info = arch.select(scene, raw, 4, 1)
        self.assertEqual([p["material"] for p in parts], list(arch.MATERIALS))
        self.assertEqual((info["replaced_section"], info["preserved_section"]), (0, 1))
        self.assertEqual(parts[0]["vertices"][0][:3], (4, 5, 6))
        matrix = list(scene.mesh_instances[-1].transform); matrix[12] += 3; matrix[0] = -1
        changed = replace(scene, mesh_instances=scene.mesh_instances[:-1]+(replace(scene.mesh_instances[-1], transform=tuple(matrix)),))
        moved, _ = arch.select(changed, raw, 4, 1)
        self.assertEqual(moved[0]["vertices"][0][:3], (7, 5, 6))
        self.assertAlmostEqual(moved[0]["vertices"][1][1], 6)
        self.assertAlmostEqual(moved[0]["vertices"][2][0], 6)
        with self.assertRaisesRegex(ValueError, "both source sections"):
            arch.select(replace(scene, submissions=scene.submissions[:-1]), raw, 4, 1)
        with self.assertRaisesRegex(ValueError, "bounds"):
            arch.select(scene, candle_tests.glb(scene, names=arch.MATERIALS, shift=1), 4, 1)
        with self.assertRaisesRegex(ValueError, "materials"):
            arch.select(scene, candle_tests.glb(scene, names=arch.MATERIALS[:1]), 4, 1)


class ArchPacketTests(unittest.TestCase):
    setUpClass = classmethod(packet_tests.PbrPacketTests.setUpClass.__func__)
    tearDownClass = classmethod(packet_tests.PbrPacketTests.tearDownClass.__func__)
    parse = packet_tests.PbrPacketTests.parse

    def test_replaces_only_rosette_and_keeps_original_stone_geometry(self):
        scene, lights, config = inputs()
        args = (scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene))
        baseline, baseline_info = pbr.create_packet(*args)
        body, info = pbr.create_packet(*args, arch_glb=candle_tests.glb(scene, names=arch.MATERIALS))
        self.assertEqual(self.parse(body).returncode, 0)
        target = scene.mesh_instances[-1].id
        original = next(i for i, d in enumerate(baseline_info["draws"]) if d["target_id"] == target and d["section"] == 1)
        preserved = next(i for i, d in enumerate(info["draws"]) if d["target_id"] == target and d["section"] == 1)
        self.assertEqual(packet_tests.positions_by_draw(baseline)[original], packet_tests.positions_by_draw(body)[preserved])
        self.assertEqual(info["draws"][preserved]["replacement"], "stone_wall_v2")
        self.assertEqual([d["section"] for d in info["skipped"] if d["reason"] == "replaced_by_generated_rosette"], [0])
        self.assertEqual([d["material"] for d in info["draws"] if d["replacement"] == "arch_high"], list(arch.MATERIALS))
        restored, _ = pbr.create_packet(*args)
        self.assertEqual(baseline, restored)
        with self.assertRaisesRegex(ValueError, "remaster materials"):
            pbr.create_packet(scene, lights, json.dumps(config), Resolver(), None,
                              arch_glb=candle_tests.glb(scene, names=arch.MATERIALS))

    def test_generated_and_preserved_sections_follow_actor_shadow_flag(self):
        scene, lights, config = inputs()
        light_id = f"candidate:light:{scene.source.map}:{scene.source.fingerprints['map']}:lamp"
        lights = replace(lights, lights=(replace(lights.lights[0], id=light_id),))
        config["lights"][0]["id"] = light_id
        actors = {"lamp": {"attached_to": None},
                  "StaticMeshActor__1520": {"mesh": "INNbed1A1", "casts_shadows": True},
                  "StaticMeshActor__59": {"mesh": "GENsknDOORFRMarc4x10half", "casts_shadows": True}}
        shadow = {"size": 128, "near_m": .05, "far_m": 15, "bias_m": .015, "softness": .5}
        args = (scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene))
        raw = candle_tests.glb(scene, names=arch.MATERIALS)
        body, info = pbr.create_packet(*args, shadows=shadow, actors=actors, arch_glb=raw)
        self.assertEqual(self.parse(body).returncode, 0)
        target = scene.mesh_instances[-1].id
        selected = [d for d in info["draws"] if d["target_id"] == target]
        self.assertEqual(len(selected), 3)
        self.assertTrue(all(d["shadow_caster_lights"] == [light_id] for d in selected))
        actors["StaticMeshActor__59"]["casts_shadows"] = False
        _, disabled = pbr.create_packet(*args, shadows=shadow, actors=actors, arch_glb=raw)
        self.assertTrue(all(not d["shadow_caster_lights"] for d in disabled["draws"] if d["target_id"] == target))


if __name__ == "__main__":
    unittest.main()
