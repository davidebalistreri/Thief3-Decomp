"""Bedroom prop materials are selected by actor, resource and material identity."""
from dataclasses import replace
import json
from pathlib import Path
import sys
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import scene_pbr_packet as pbr
import test_scene_pbr_packet as packet_tests
from scene_ir import MaterialRef
from test_scene_material_overrides import FakeRemaster
from test_scene_textured_packet import Resolver


def inputs():
    scene, lights, config = packet_tests.inputs()
    original, resource = scene.mesh_instances[0], scene.static_mesh_resources[0]
    existing = [s for s in scene.submissions if s.target_id == original.id]
    prefix = f"actor:{scene.source.map}:{scene.source.fingerprints['map']}:"
    resources, instances, submissions, materials = [], [], [], []
    for name, mesh, material in (("D_1224__21", "GENxGOODScrateB", "HSEGoodnessA_D"),
                                  ("StaticMeshActor__258", "GENrafters1", "GENbeamA_D")):
        mid = "material:"+name
        resource_id = "mesh:"+name
        identity = prefix+name
        resources.append(replace(resource, id=resource_id, name=mesh))
        instances.append(replace(original, id=identity, resource_id=resource_id, material_ids=(mid,)*3))
        submissions.extend(replace(s, target_id=identity) for s in existing)
        materials.append(MaterialRef(mid, material))
    scene = replace(scene, static_mesh_resources=scene.static_mesh_resources+tuple(resources),
                    mesh_instances=scene.mesh_instances+tuple(instances), submissions=scene.submissions+tuple(submissions),
                    materials=scene.materials+tuple(materials))
    lights = replace(lights, mesh_instances=scene.mesh_instances, static_mesh_resources=scene.static_mesh_resources)
    return scene, lights, config


class PropMaterialTests(unittest.TestCase):
    setUpClass = classmethod(packet_tests.PbrPacketTests.setUpClass.__func__)
    tearDownClass = classmethod(packet_tests.PbrPacketTests.tearDownClass.__func__)
    parse = packet_tests.PbrPacketTests.parse

    def test_targets_are_qualified_and_original_is_reversible(self):
        scene, lights, config = inputs()
        remaster = FakeRemaster(scene)
        args = (scene, lights, json.dumps(config), Resolver(), remaster)
        baseline, info0 = pbr.create_packet(*args)
        body, info = pbr.create_packet(*args, prop_materials=True)
        self.assertEqual(self.parse(body).returncode, 0)
        selected = [d for d in info["draws"] if d["replacement"] in ("beam_wood_v2", "furniture_wood_v2")]
        self.assertEqual(len(selected), 4)
        self.assertEqual({d["replacement"] for d in selected}, {"beam_wood_v2", "furniture_wood_v2"})
        self.assertTrue(all(d["ao"] == "ambient_occlusion_1" for d in selected))
        self.assertEqual(len(info["bedroom_prop_materials"]), 4)
        self.assertEqual(info["draws"][:len(info0["draws"])-4], info0["draws"][:len(info0["draws"])-4])
        restored, _ = pbr.create_packet(*args)
        self.assertEqual(restored, baseline)
        crate, crate_resource = scene.mesh_instances[-2], scene.static_mesh_resources[-2]
        self.assertIsNone(remaster.prop_material(scene, replace(crate, id=crate.id+"_other"), crate_resource, "HSEGoodnessA_D"))
        self.assertIsNone(remaster.prop_material(scene, crate, replace(crate_resource, name="other"), "HSEGoodnessA_D"))
        self.assertIsNone(remaster.prop_material(scene, crate, crate_resource, "other"))
        with self.assertRaisesRegex(ValueError, "remaster materials"):
            pbr.create_packet(scene, lights, json.dumps(config), Resolver(), None, prop_materials=True)


if __name__ == "__main__":
    unittest.main()
