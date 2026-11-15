"""Bedroom details are bounded offline additions anchored to the submitted bed."""
from dataclasses import replace
import json
from pathlib import Path
import sys
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import scene_pbr_details as details
import scene_pbr_packet as pbr
import test_scene_pbr_bed as bed_tests
import test_scene_pbr_packet as packet_tests
from test_scene_material_overrides import FakeRemaster
from test_scene_textured_packet import Resolver


class DetailSelectionTests(unittest.TestCase):
    def test_uses_bed_transform_and_rejects_unknown_or_out_of_region_geometry(self):
        scene, _, _ = packet_tests.inputs()
        anchor = scene.mesh_instances[0].id
        raw = bed_tests.glb(scene, names=details.MATERIALS)
        parts, info = details.select(scene, anchor, raw, 4, 1)
        self.assertEqual([p["material"] for p in parts], list(details.MATERIALS))
        self.assertEqual(info["visibility"], "not_runtime_observed")
        self.assertEqual(parts[0]["vertices"][0][:3], (4, 5, 6))
        matrix = list(scene.mesh_instances[0].transform); matrix[12] += 3; matrix[0] = -1
        changed = replace(scene, mesh_instances=(replace(scene.mesh_instances[0], transform=tuple(matrix)),))
        moved, _ = details.select(changed, anchor, raw, 4, 1)
        self.assertEqual(moved[0]["vertices"][0][:3], (7, 5, 6))
        self.assertAlmostEqual(moved[0]["vertices"][1][1], 6)
        self.assertAlmostEqual(moved[0]["vertices"][2][0], 6)
        with self.assertRaisesRegex(ValueError, "submitted"):
            details.select(scene, anchor, raw, 4, 9)
        with self.assertRaisesRegex(ValueError, "region"):
            details.select(scene, anchor, bed_tests.glb(scene, names=details.MATERIALS, shift=8), 4, 1)
        with self.assertRaisesRegex(ValueError, "materials"):
            details.select(scene, anchor, bed_tests.glb(scene, names=details.MATERIALS[:-1]), 4, 1)


class DetailPacketTests(unittest.TestCase):
    setUpClass = classmethod(packet_tests.PbrPacketTests.setUpClass.__func__)
    tearDownClass = classmethod(packet_tests.PbrPacketTests.tearDownClass.__func__)
    parse = packet_tests.PbrPacketTests.parse

    def test_additions_preserve_original_draws_and_have_explicit_provenance(self):
        scene, lights, config = packet_tests.inputs()
        args = (scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene))
        baseline, baseline_info = pbr.create_packet(*args)
        body, info = pbr.create_packet(*args, details_glb=bed_tests.glb(scene, names=details.MATERIALS))
        self.assertEqual(self.parse(body).returncode, 0)
        self.assertEqual(info["draws"][:len(baseline_info["draws"])], baseline_info["draws"])
        added = [d for d in info["draws"] if d["target_kind"] == "visual_addition"]
        self.assertEqual([d["material"] for d in added], list(details.MATERIALS))
        self.assertTrue(all(d["source"] == "offline_generated_godot_visual_addition" for d in added))
        self.assertEqual(info["generated_bedroom_details"]["anchor_id"], scene.mesh_instances[0].id)
        restored, _ = pbr.create_packet(*args)
        self.assertEqual(baseline, restored)
        with self.assertRaisesRegex(ValueError, "remaster materials"):
            pbr.create_packet(scene, lights, json.dumps(config), Resolver(), None,
                              details_glb=bed_tests.glb(scene, names=details.MATERIALS))

    def test_artistic_details_cast_to_selected_lights(self):
        scene, lights, config = packet_tests.inputs()
        light_id = f"candidate:light:{scene.source.map}:{scene.source.fingerprints['map']}:lamp"
        lights = replace(lights, lights=(replace(lights.lights[0], id=light_id),))
        config["lights"][0]["id"] = light_id
        actors = {"lamp": {"attached_to": None},
                  "StaticMeshActor__1520": {"mesh": "INNbed1A1", "casts_shadows": True}}
        shadow = {"size": 128, "near_m": .05, "far_m": 15, "bias_m": .015, "softness": .5}
        body, info = pbr.create_packet(scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene),
                                        shadows=shadow, actors=actors, details_glb=bed_tests.glb(scene, names=details.MATERIALS))
        self.assertEqual(self.parse(body).returncode, 0)
        self.assertTrue(all(d["shadow_caster_lights"] == [light_id] for d in info["draws"] if d["target_kind"] == "visual_addition"))


if __name__ == "__main__":
    unittest.main()
