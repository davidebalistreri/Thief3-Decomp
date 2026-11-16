"""Generated flame geometry stays bounded and tied to a semantic light candidate."""
from dataclasses import replace
import json
from pathlib import Path
import struct
import sys
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import scene_pbr_packet as pbr
import scene_pbr_flames as flames
import scene_pbr_emission_fixture as gpu_fixture
import scene_pbr_variants as variants
import test_scene_pbr_packet as packet_tests
from test_scene_textured_packet import Resolver

SHADOW = {"size": 128, "near_m": .05, "far_m": 15, "bias_m": .015, "softness": .5}


def glb(change=None):
    points = ((0, 0, 0), (.1, 0, 0), (0, .1, 0))
    blob = b"".join(struct.pack("<3f", *v) for v in points)
    blob += struct.pack("<3f", 0, 0, 1)*3 + struct.pack("<3H", 0, 1, 2)
    data = {"asset": {"version": "2.0"}, "buffers": [{"byteLength": len(blob)}],
            "scenes": [{"nodes": [0]}], "scene": 0, "nodes": [{"mesh": 0, "name": "candle"}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1},
                                        "indices": 2, "material": 0}]}],
            "materials": [{"name": "CandleFlame"}],
            "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
                          {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
                          {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}],
            "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                            {"buffer": 0, "byteOffset": 36, "byteLength": 36},
                            {"buffer": 0, "byteOffset": 72, "byteLength": 6}]}
    if change is not None:
        change(data)
    encoded = json.dumps(data, separators=(",", ":")).encode()
    encoded += b" " * (-len(encoded) % 4)
    blob += b"\0" * (-len(blob) % 4)
    size = 12+8+len(encoded)+8+len(blob)
    return struct.pack("<4sII", b"glTF", 2, size) + struct.pack("<II", len(encoded), 0x4e4f534a) + encoded + \
        struct.pack("<II", len(blob), 0x004e4942) + blob


def inputs():
    scene, lights, config = packet_tests.inputs()
    data = scene.to_dict()
    old = data["mesh_instances"][0]["id"]
    name = "D_384__5"
    identity = f"actor:{scene.source.map}:{scene.source.fingerprints['map']}:{name}"
    data["mesh_instances"][0]["id"] = identity
    data["static_mesh_resources"][0]["name"] = "GENxLIGHT2candles"
    for sub in data["submissions"]:
        if sub["target_id"] == old: sub["target_id"] = identity
    scene = pbr.SceneSnapshot.from_dict(data)
    light_id = f"candidate:light:{scene.source.map}:{scene.source.fingerprints['map']}:{name}"
    lights = replace(lights, mesh_instances=scene.mesh_instances, static_mesh_resources=scene.static_mesh_resources,
                     lights=(replace(lights.lights[0], id=light_id),))
    config["lights"][0]["id"] = light_id
    actors = {name: {"mesh": "GENxLIGHT2candles", "casts_shadows": False, "attached_to": None,
                     "light": {"kind": "omni", "on": True}}}
    return scene, lights, config, actors


class FlameGeometryTests(unittest.TestCase):
    def test_reads_generated_triangle_and_rejects_malformed_glb(self):
        points, normals, indices = flames.read_geometry(glb())
        self.assertEqual((len(points), len(normals), indices), (3, 3, [0, 1, 2]))
        changes = (lambda d: d["nodes"][0].update(translation=[1, 0, 0]),
                   lambda d: d["materials"][0].update(name="other"),
                   lambda d: d["accessors"][0].update(sparse={}),
                   lambda d: d["bufferViews"][0].update(byteLength=2),
                   lambda d: d["accessors"][2].update(count=4),
                   lambda d: d["accessors"][1].update(componentType=5123),
                   lambda d: d.update(asset=[]), lambda d: d.update(scenes=[None]),
                   lambda d: d["bufferViews"][0].update(byteStride=13),
                   lambda d: d["meshes"][0]["primitives"][0].update(attributes=[]),
                   lambda d: d["meshes"][0]["primitives"][0]["attributes"].update(JOINTS_0=0))
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):
                flames.read_geometry(glb(change))
        for raw in (b"", glb()[:-1], b"bad!"+glb()[4:]):
            with self.subTest(raw=raw[:4]), self.assertRaises(ValueError):
                flames.read_geometry(raw)

    def test_matches_same_actor_light_and_uses_observed_transform(self):
        scene, light_scene, config, actors = inputs()
        _, _, lights = pbr.load_calibration(json.dumps(config), light_scene)
        result, digest = flames.select(scene, lights, actors, glb(), (8, 3.2, .64), 4, 1)
        self.assertEqual(len(digest), 64)
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["target_id"], scene.mesh_instances[0].id)
        self.assertEqual(result[0]["light_id"], lights[0]["id"])
        self.assertEqual(result[0]["vertices"][0][:3], (4, 5, 6))
        self.assertAlmostEqual(result[0]["vertices"][1][2], 6 + .1/.01905, places=5)
        transform = list(scene.mesh_instances[0].transform)
        transform[0] = -1
        mirrored = replace(scene, mesh_instances=(replace(scene.mesh_instances[0], transform=tuple(transform)),))
        mirrored_result, _ = flames.select(mirrored, lights, actors, glb(), (8, 3.2, .64), 4, 1)
        self.assertAlmostEqual(mirrored_result[0]["vertices"][1][0], 4 - .1/.01905, places=5)
        with mock.patch.object(flames, "MAX_FLAME_TRIANGLES", 0):
            with self.assertRaisesRegex(ValueError, "geometry exceeds bounds"):
                flames.select(scene, lights, actors, glb(), (8, 3.2, .64), 4, 1)
        actors["D_384__5"]["light"]["on"] = False
        result, _ = flames.select(scene, lights, actors, glb(), (8, 3.2, .64), 4, 1)
        self.assertEqual(result, [])
        actors["D_384__5"]["light"]["on"] = True
        actors["D_384__5"]["mesh"] = "other"
        with self.assertRaisesRegex(ValueError, "offline mesh or light state"):
            flames.select(scene, lights, actors, glb(), (8, 3.2, .64), 4, 1)
        with self.assertRaisesRegex(ValueError, "candidate light"):
            flames.select(scene, [], actors, glb(), (8, 3.2, .64), 4, 1)


class FlamePacketTests(unittest.TestCase):
    setUpClass = classmethod(packet_tests.PbrPacketTests.setUpClass.__func__)
    tearDownClass = classmethod(packet_tests.PbrPacketTests.tearDownClass.__func__)
    parse = packet_tests.PbrPacketTests.parse

    def test_spot_and_candle_emission_share_a_packet(self):
        scene, lights, config, actors = inputs()
        spot_id = f"candidate:light:{scene.source.map}:{scene.source.fingerprints['map']}:spot"
        spot = replace(lights.lights[0], id=spot_id, kind="spot")
        lights = replace(lights, lights=lights.lights+(spot,))
        config["version"] = 2
        config["lights"].append({"id": spot_id, "color_linear": [1, .5, .25], "intensity_candela": 5,
                                 "radius_m": .1, "direction_world": [0, 0, 1],
                                 "inner_degrees": 12, "outer_degrees": 25})
        actors["spot"] = {"attached_to": None}
        packet, info = pbr.create_packet(scene, lights, json.dumps(config), Resolver(), None,
                                          shadows=SHADOW, actors=actors, flame_glb=glb())
        self.assertEqual(packet[:8], pbr.SPOT_MAGIC)
        self.assertEqual(struct.unpack_from("<I", packet, 196)[0], 3)
        self.assertEqual(self.parse(packet).returncode, 0)
        self.assertEqual(info["candle_flames"]["instances"], 1)
        self.assertEqual(len(variants.packet_signatures(packet)[0]), len(info["draws"]))

    def test_v4_packet_has_emissive_draw_and_rejects_bad_radiance(self):
        scene, lights, config, actors = inputs()
        args = (scene, lights, json.dumps(config), Resolver(), None)
        raw, info = pbr.create_packet(*args, shadows=SHADOW, actors=actors, flame_glb=glb())
        self.assertEqual(raw[:8], pbr.EMISSION_MAGIC)
        self.assertEqual(self.parse(raw).returncode, 0)
        self.assertEqual(info["candle_flames"]["instances"], 1)
        self.assertEqual(info["draws"][-1]["material"], "CandleFlame")
        self.assertEqual(info["draws"][-1]["shadow_caster_lights"], [])
        tex_count, _, light_count = struct.unpack_from("<III", raw, 184)
        offset = 220 + light_count*32
        for _ in range(tex_count):
            width, height, levels = struct.unpack_from("<III", raw, offset)
            offset += 12
            for _ in range(levels):
                offset += width*height*4
                width, height = max(1, width//2), max(1, height//2)
        for bad in (struct.pack("<f", -1), struct.pack("<f", 1001), struct.pack("<I", 0x7fc00000)):
            self.assertEqual(self.parse(raw[:offset+24]+bad+raw[offset+28:]).returncode, 2)
        actors["D_384__5"]["light"]["on"] = False
        off, stats = pbr.create_packet(*args, shadows=SHADOW, actors=actors, flame_glb=glb())
        self.assertEqual(self.parse(off).returncode, 0)
        self.assertEqual(stats["candle_flames"]["instances"], 0)
        with self.assertRaisesRegex(ValueError, "require shadow"):
            pbr.create_packet(*args, flame_glb=glb())
        with self.assertRaisesRegex(ValueError, "flame radiance"):
            pbr.create_packet(*args, shadows=SHADOW, actors=actors, flame_glb=glb(), flame_radiance=1001)

    def test_emission_gpu_fixture_is_accepted_and_off_control_is_required(self):
        for state in (False, True):
            self.assertEqual(self.parse(gpu_fixture.fixture(state)).returncode, 0)
        header = b"P6\n64 64\n255\n"
        base = bytearray(header+bytes(64*64*3))
        source = len(header)+(32*64+16)*3
        base[source:source+3] = bytes((150, 80, 30))
        bounce = bytearray(base)
        receiver = len(header)+(32*64+35)*3
        bounce[receiver:receiver+3] = bytes((30, 1, 0))
        off = bytearray(base)
        off[source:source+3] = bytes((0, 0, 0))
        gpu_fixture.verify(base, bounce, off)
        gpu_fixture.verify_off(off, off)
        with self.assertRaisesRegex(ValueError, "did not add HDR"):
            gpu_fixture.verify(base, bounce, base)
        with self.assertRaisesRegex(ValueError, "disabled emission"):
            gpu_fixture.verify_off(off, bounce)


if __name__ == "__main__":
    unittest.main()
