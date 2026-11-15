"""The candlestick override follows qualified actors and preserves switching."""
from dataclasses import replace
import json
from pathlib import Path
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import scene_pbr_candlestick as candle
import scene_pbr_packet as pbr
import test_scene_pbr_packet as packet_tests
from gltf import GLTFBuilder
from test_scene_material_overrides import FakeRemaster
from test_scene_textured_packet import Resolver


def inputs():
    scene, lights, config = packet_tests.inputs()
    bed_instance, bed_resource = scene.mesh_instances[0], scene.static_mesh_resources[0]
    identity = f"actor:{scene.source.map}:{scene.source.fingerprints['map']}:D_384__5"
    resource = replace(bed_resource, id="mesh:candlestick", name="GENxLIGHT2candles")
    instance = replace(bed_instance, id=identity, resource_id=resource.id)
    submissions = tuple(replace(s, target_id=identity) for s in scene.submissions if s.target_id == bed_instance.id)
    scene = replace(scene, static_mesh_resources=scene.static_mesh_resources+(resource,),
                    mesh_instances=scene.mesh_instances+(instance,), submissions=scene.submissions+submissions)
    light_id = f"candidate:light:{scene.source.map}:{scene.source.fingerprints['map']}:D_384__5"
    lights = replace(lights, mesh_instances=scene.mesh_instances, static_mesh_resources=scene.static_mesh_resources,
                     lights=(replace(lights.lights[0], id=light_id),))
    config["lights"][0]["id"] = light_id
    actors = {"StaticMeshActor__1520": {"mesh": "INNbed1A1", "casts_shadows": True},
              "D_384__5": {"mesh": "GENxLIGHT2candles", "casts_shadows": True, "attached_to": None,
                            "light": {"kind": "omni", "on": True}}}
    return scene, lights, config, actors


def glb(scene, names=candle.MATERIALS, shift=0):
    geometry = scene.static_mesh_resources[-1].geometry
    scale = scene.conventions.meters_per_unit
    builder = GLTFBuilder()
    attrs = {"POSITION": builder.add_floats([(x*scale+shift,z*scale,y*scale) for x,y,z in geometry.positions], "VEC3"),
             "NORMAL": builder.add_floats([(x,z,y) for x,y,z in geometry.normals], "VEC3"),
             "TEXCOORD_0": builder.add_floats(geometry.uv0, "VEC2")}
    indices = builder.add_indices((0,2,1))
    primitives = [{"attributes": attrs, "indices": indices, "material": builder.material(name)} for name in names]
    builder.node("candlestick", builder.mesh("candlestick", primitives), root=True)
    with tempfile.TemporaryDirectory(prefix="t3render-candle-glb-") as directory:
        path = Path(directory)/"candle.glb"
        builder.write_glb(path)
        return path.read_bytes()


class CandleSelectionTests(unittest.TestCase):
    def test_uses_actor_transform_and_rejects_bad_bounds_materials_or_scope(self):
        scene, _, _, _ = inputs()
        parts, report = candle.select(scene, glb(scene), 4, 1)
        self.assertEqual([p["material"] for p in parts], list(candle.MATERIALS[:2]))
        self.assertEqual(report["instances"][0]["target_id"], scene.mesh_instances[-1].id)
        self.assertEqual(parts[0]["vertices"][0][:3], (4, 5, 6))
        matrix = list(scene.mesh_instances[-1].transform); matrix[12] += 3
        changed = replace(scene, mesh_instances=scene.mesh_instances[:-1]+(replace(scene.mesh_instances[-1], transform=tuple(matrix)),))
        moved, _ = candle.select(changed, glb(scene), 4, 1)
        self.assertEqual(moved[0]["vertices"][0][:3], (7, 5, 6))
        matrix[0] = -1
        mirrored = replace(scene, mesh_instances=scene.mesh_instances[:-1]+(replace(scene.mesh_instances[-1], transform=tuple(matrix)),))
        reflected, _ = candle.select(mirrored, glb(scene), 4, 1)
        self.assertAlmostEqual(reflected[0]["vertices"][1][1], 6)
        self.assertAlmostEqual(reflected[0]["vertices"][2][0], 6)
        for value, pattern in ((glb(scene, names=candle.MATERIALS[:-1]), "materials"),
                               (glb(scene, shift=1), "bounds")):
            with self.assertRaisesRegex(ValueError, pattern):
                candle.select(scene, value, 4, 1)
        with self.assertRaisesRegex(ValueError, "no submitted"):
            candle.select(scene, glb(scene), 4, 9)
        wrong_resource = replace(scene.static_mesh_resources[-1], name="unrelated")
        with self.assertRaisesRegex(ValueError, "unexpected resource"):
            candle.select(replace(scene, static_mesh_resources=scene.static_mesh_resources[:-1]+(wrong_resource,)), glb(scene), 4, 1)


class CandlePacketTests(unittest.TestCase):
    setUpClass = classmethod(packet_tests.PbrPacketTests.setUpClass.__func__)
    tearDownClass = classmethod(packet_tests.PbrPacketTests.tearDownClass.__func__)
    parse = packet_tests.PbrPacketTests.parse

    def test_replacement_is_reversible_and_excludes_own_light_from_shadows(self):
        scene, lights, config, actors = inputs()
        other_id = f"candidate:light:{scene.source.map}:{scene.source.fingerprints['map']}:lamp"
        lights = replace(lights, lights=lights.lights+(replace(lights.lights[0], id=other_id),))
        config["lights"].append(dict(config["lights"][0], id=other_id))
        actors["lamp"] = {"attached_to": None}
        args = (scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene))
        shadow = {"size": 128, "near_m": .05, "far_m": 15, "bias_m": .015, "softness": .5}
        original, _ = pbr.create_packet(*args, shadows=shadow, actors=actors)
        replacement, info = pbr.create_packet(*args, shadows=shadow, actors=actors, candlestick_glb=glb(scene),
                                               flame_glb=glb(scene))
        self.assertEqual(self.parse(replacement).returncode, 0)
        selected = [d for d in info["draws"] if d["replacement"] == "candlestick_high"]
        self.assertEqual([d["material"] for d in selected], list(candle.MATERIALS[:2]))
        self.assertTrue(all(d["shadow_caster_lights"] == [other_id] for d in selected))
        self.assertEqual(info["candle_flames"]["instances"], 1)
        self.assertEqual(sum(d["reason"] == "replaced_by_generated_candlestick" for d in info["skipped"]), 2)
        restored, _ = pbr.create_packet(*args, shadows=shadow, actors=actors)
        self.assertEqual(original, restored)
        actors["D_384__5"]["casts_shadows"] = False
        _, disabled = pbr.create_packet(*args, shadows=shadow, actors=actors, candlestick_glb=glb(scene))
        self.assertTrue(all(not d["shadow_caster_lights"] for d in disabled["draws"] if d["replacement"] == "candlestick_high"))
        with self.assertRaisesRegex(ValueError, "same GLB"):
            pbr.create_packet(*args, shadows=shadow, actors=actors, candlestick_glb=glb(scene), flame_glb=b"different")
        with self.assertRaisesRegex(ValueError, "remaster materials"):
            pbr.create_packet(scene, lights, json.dumps(config), Resolver(), None, candlestick_glb=glb(scene))


if __name__ == "__main__":
    unittest.main()
