"""The generated bed replaces the semantic actor, its materials and its caster."""
from dataclasses import replace
import json
from pathlib import Path
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent/"tools"))
import scene_pbr_bed as bed
import scene_pbr_packet as pbr
import test_scene_pbr_packet as packet_tests
from gltf import GLTFBuilder
from test_scene_material_overrides import FakeRemaster
from test_scene_textured_packet import Resolver


def glb(scene, names=bed.MATERIALS, shift=0):
    geometry = scene.static_mesh_resources[0].geometry
    scale = scene.conventions.meters_per_unit
    builder = GLTFBuilder()
    positions = [(x*scale+shift,z*scale,y*scale) for x,y,z in geometry.positions]
    normals = [(x,z,y) for x,y,z in geometry.normals]
    attributes = {"POSITION": builder.add_floats(positions, "VEC3"),
                  "NORMAL": builder.add_floats(normals, "VEC3"),
                  "TEXCOORD_0": builder.add_floats(geometry.uv0, "VEC2")}
    indices = builder.add_indices((0,2,1))
    primitives = [{"attributes": attributes, "indices": indices, "material": builder.material(name)} for name in names]
    mesh = builder.mesh("bed", primitives)
    builder.node("bed", mesh, root=True)
    with tempfile.TemporaryDirectory(prefix="t3render-bed-glb-") as directory:
        path = Path(directory)/"bed.glb"
        builder.write_glb(path)
        return path.read_bytes()


class GeneratedBedTests(unittest.TestCase):
    def test_selects_actor_transform_and_materials_without_draw_numbers(self):
        scene, _, _ = packet_tests.inputs()
        anchor = scene.mesh_instances[0].id
        parts, report = bed.select(scene, anchor, glb(scene), 4, 1)
        self.assertEqual([p["material"] for p in parts], list(bed.MATERIALS))
        self.assertEqual(report["resource_name"], "INNbed1A1")
        self.assertEqual(report["target_id"], anchor)
        self.assertEqual(report["triangles"], 6)
        self.assertEqual(parts[0]["vertices"][0][:3], (4, 5, 6))
        for actual, expected in zip(parts[0]["vertices"][1][:3], (5, 5, 6)):
            self.assertAlmostEqual(actual, expected)
        for actual, expected in zip(parts[0]["vertices"][2][:3], (4, 6, 6)):
            self.assertAlmostEqual(actual, expected)
        moved = list(scene.mesh_instances[0].transform); moved[12] += 3
        changed = replace(scene, mesh_instances=(replace(scene.mesh_instances[0], transform=tuple(moved)),))
        result, _ = bed.select(changed, anchor, glb(scene), 4, 1)
        self.assertEqual(result[0]["vertices"][0][:3], (7, 5, 6))
        mirrored = list(scene.mesh_instances[0].transform); mirrored[0] = -1
        reflected = replace(scene, mesh_instances=(replace(scene.mesh_instances[0], transform=tuple(mirrored)),))
        result, _ = bed.select(reflected, anchor, glb(scene), 4, 1)
        self.assertAlmostEqual(result[0]["vertices"][1][1], 6)
        self.assertAlmostEqual(result[0]["vertices"][2][0], 3)

    def test_missing_material_wrong_bounds_or_unsubmitted_actor_fail_closed(self):
        scene, _, _ = packet_tests.inputs()
        anchor = scene.mesh_instances[0].id
        with self.assertRaisesRegex(ValueError, "materials"):
            bed.select(scene, anchor, glb(scene, bed.MATERIALS[:-1]), 4, 1)
        with self.assertRaisesRegex(ValueError, "bounds"):
            bed.select(scene, anchor, glb(scene, shift=1), 4, 1)
        with self.assertRaisesRegex(ValueError, "submitted"):
            bed.select(scene, anchor, glb(scene), 4, 9)


class GeneratedBedPacketTests(unittest.TestCase):
    setUpClass = classmethod(packet_tests.PbrPacketTests.setUpClass.__func__)
    tearDownClass = classmethod(packet_tests.PbrPacketTests.tearDownClass.__func__)
    parse = packet_tests.PbrPacketTests.parse

    def test_semantic_override_replaces_all_bed_sections_and_preserves_caster_flags(self):
        scene, lights, config = packet_tests.inputs()
        name = f"candidate:light:{scene.source.map}:{scene.source.fingerprints['map']}:lamp"
        lights = replace(lights, lights=(replace(lights.lights[0], id=name),))
        config["lights"][0]["id"] = name
        actors = {"lamp": {"attached_to": None},
                  "StaticMeshActor__1520": {"mesh": "INNbed1A1", "casts_shadows": True}}
        shadow = {"size": 128, "near_m": .05, "far_m": 15, "bias_m": .015, "softness": .5}
        baseline, _ = pbr.create_packet(scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene),
                                        shadows=shadow, actors=actors)
        raw, info = pbr.create_packet(scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene),
                                      shadows=shadow, actors=actors, bed_glb=glb(scene))
        self.assertEqual(self.parse(raw).returncode, 0)
        replacement = [item for item in info["draws"] if item["replacement"] == "bed_high"]
        self.assertEqual(len(replacement), 6)
        self.assertTrue(all(item["target_id"] == scene.mesh_instances[0].id for item in replacement))
        self.assertTrue(all(item["shadow_caster_lights"] == [name] for item in replacement))
        self.assertEqual(sum(item["reason"] == "replaced_by_generated_bed" for item in info["skipped"]), 2)
        self.assertEqual(info["generated_bed"]["triangles"], 6)
        restored, _ = pbr.create_packet(scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene),
                                        shadows=shadow, actors=actors)
        self.assertEqual(restored, baseline)
        actors["StaticMeshActor__1520"]["casts_shadows"] = False
        _, disabled = pbr.create_packet(scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene),
                                        shadows=shadow, actors=actors, bed_glb=glb(scene))
        self.assertTrue(all(not item["shadow_caster_lights"] for item in disabled["draws"] if item["replacement"] == "bed_high"))
        with self.assertRaisesRegex(ValueError, "remaster materials"):
            pbr.create_packet(scene, lights, json.dumps(config), Resolver(), None, bed_glb=glb(scene))
        unshadowed, stats = pbr.create_packet(scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene),
                                              bed_glb=glb(scene))
        self.assertEqual(unshadowed[:8], pbr.MAGIC)
        self.assertEqual(self.parse(unshadowed).returncode, 0)
        self.assertEqual(stats["generated_bed"]["triangles"], 6)


if __name__ == "__main__":
    unittest.main()
