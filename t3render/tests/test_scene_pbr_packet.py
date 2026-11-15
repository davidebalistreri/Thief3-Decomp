from dataclasses import replace
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import scene_pbr_packet as pbr
import scene_pbr_fixture as gpu_fixture
from scene_ir import VisualLight
from test_scene_material_overrides import FakeRemaster, scene_fixture
from test_scene_textured_packet import Resolver

HARNESS = r'''
#include "scene_pbr_packet.hpp"
#include <iostream>
#include <iterator>
#include <string>
int main() {
    std::string bytes((std::istreambuf_iterator<char>(std::cin)), {});
    t3render::pbr::Frame frame;
    if (!t3render::pbr::ParsePacket(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), frame)) return 2;
    std::cout << frame.width << " " << frame.height << " " << frame.lights.size() << " " << frame.textures.size()
              << " " << frame.draws.size() << " " << frame.vertices.size() << " " << frame.draws[0].textures[0] << "\n";
}
'''


def inputs():
    scene = scene_fixture()
    light = VisualLight("candidate:light:test", "omni", (1, 2, 3), None, None, None, None)
    light_scene = replace(scene, lights=(light,))
    config = {"schema": "t3.scene.pbr-preview", "version": 1, "ambient_linear": [.02, .03, .04],
              "exposure": 1, "lights": [{"id": light.id, "color_linear": [1, .5, .25],
                                         "intensity_candela": 10, "radius_m": .1}]}
    return scene, light_scene, config


def positions_by_draw(body):
    texture_count, draw_count, light_count = struct.unpack_from("<III", body, 184)
    offset = 196+light_count*32
    for _ in range(texture_count):
        width, height, levels = struct.unpack_from("<III", body, offset)
        offset += 12
        for _ in range(levels):
            offset += width*height*4
            width, height = max(1, width//2), max(1, height//2)
    result = []
    for _ in range(draw_count):
        count = struct.unpack_from("<I", body, offset+16)[0]
        offset += 20
        result.append([struct.unpack_from("<3f", body, offset+n*48) for n in range(count)])
        offset += count*48
    return result


class PbrPacketTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("host C++ compiler required")
        cls.temp = tempfile.TemporaryDirectory(prefix="t3render-pbr-packet-")
        cls.binary = Path(cls.temp.name) / "parser"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(HERE.parent / "src"),
                        "-x", "c++", "-o", str(cls.binary), "-"], input=HARNESS, text=True,
                       check=True, capture_output=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def parse(self, raw):
        return subprocess.run([str(self.binary)], input=raw, capture_output=True, timeout=10)

    def test_semantic_materials_lights_and_geometry_reach_independent_parser(self):
        scene, lights, calibration = inputs()
        body, info = pbr.create_packet(scene, lights, json.dumps(calibration), Resolver(), FakeRemaster(scene))
        self.assertEqual(body[:8], pbr.MAGIC)
        self.assertEqual(self.parse(body).returncode, 0)
        self.assertEqual(info["triangles"], 6)
        self.assertEqual(len(info["draws"]), 5)
        self.assertEqual([(d["replacement"], d["triangle_range"]) for d in info["draws"][:3]],
                         [("wood_floor_v2", [0, 1]), (None, [1, 3]), ("stone_wall_v2", [0, 1])])
        self.assertEqual(info["draws"][0]["roughness"], "wood_floor_v2_roughness")
        self.assertEqual(info["draws"][0]["ao"], "wood_floor_v2_ao")
        self.assertEqual(info["draws"][1]["roughness"], "roughness_0_8")
        self.assertEqual(info["lights"][0]["position"], (1, 2, 3))
        self.assertEqual(info["lights"][0]["intensity_candela"], 10)

    def test_compatibility_data_is_not_needed_and_translucency_is_explicitly_skipped(self):
        class TransparentResolver(Resolver):
            def resolve(self, name):
                value = super().resolve(name)
                if name.lower() == "hsefloorb_d":
                    value["mips"] = [bytes([255, 255, 255, 0]*4)]
                return value
        scene, lights, calibration = inputs()
        body, info = pbr.create_packet(scene, lights, json.dumps(calibration), TransparentResolver(), None)
        self.assertEqual(self.parse(body).returncode, 0)
        self.assertEqual(len(info["skipped"]), 3)
        self.assertTrue(all(x["reason"] == "nonopaque_diffuse_texture" for x in info["skipped"]))
        self.assertEqual(info["triangles"], 1)

    def test_mirrored_instance_preserves_front_faces(self):
        scene, lights, calibration = inputs()
        original, _ = pbr.create_packet(scene, lights, json.dumps(calibration), Resolver(), None)
        transform = list(scene.mesh_instances[0].transform)
        transform[0] = -1
        mirrored = replace(scene, mesh_instances=(replace(scene.mesh_instances[0], transform=tuple(transform)),))
        light_scene = replace(lights, mesh_instances=mirrored.mesh_instances)
        changed, info = pbr.create_packet(mirrored, light_scene, json.dumps(calibration), Resolver(), None)
        self.assertEqual(self.parse(changed).returncode, 0)
        self.assertTrue(info["draws"][-1]["reversed_winding"])
        before, after = positions_by_draw(original)[-1], positions_by_draw(changed)[-1]
        expected = [(2*transform[12]-v[0], v[1], v[2]) for v in (before[0], before[2], before[1])]
        self.assertEqual(after, expected)

    def test_calibration_rejects_ambiguous_or_unbounded_inputs(self):
        _, lights, config = inputs()
        bad_configs = []
        for key, value in (("exposure", 0), ("ambient_linear", [float("nan"), 0, 0]), ("version", True)):
            item = dict(config, **{key: value}); bad_configs.append(item)
        bad_configs.extend((dict(config, lights=[]), dict(config, lights=[config["lights"][0]]*2),
                            dict(config, lights=[dict(config["lights"][0], id="missing")]),
                            dict(config, lights=[dict(config["lights"][0], radius_m=0)]),
                            dict(config, lights=[dict(config["lights"][0], intensity_candela=-1)])))
        for value in bad_configs:
            with self.subTest(value=value), self.assertRaises(ValueError):
                pbr.load_calibration(json.dumps(value), lights)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            pbr.load_calibration('{"schema":"x","schema":"y"}', lights)
        with self.assertRaisesRegex(ValueError, "point lights"):
            pbr.load_calibration(json.dumps(config), replace(lights, lights=(replace(lights.lights[0], kind="spot"),)))

    def test_scene_light_frame_and_units_are_required(self):
        scene, lights, calibration = inputs()
        for changed in (replace(lights, source=replace(lights.source, frame_id=4)),
                        replace(lights, camera=replace(lights.camera, view=tuple([2]+list(lights.camera.view[1:])))),
                        replace(lights, source=replace(lights.source, fingerprints={"map": "d"*64}))):
            with self.subTest(changed=changed), self.assertRaisesRegex(ValueError, "same|share"):
                pbr.create_packet(scene, changed, json.dumps(calibration), Resolver(), None)
        changed = replace(scene, conventions=replace(scene.conventions, meters_per_unit=None))
        with self.assertRaisesRegex(ValueError, "unit scale"):
            pbr.create_packet(changed, replace(lights, conventions=changed.conventions), json.dumps(calibration), Resolver(), None)

    def test_parser_rejects_bad_counts_invalid_values_and_trailing_bytes(self):
        scene, lights, calibration = inputs()
        body, _ = pbr.create_packet(scene, lights, json.dumps(calibration), Resolver(), FakeRemaster(scene))
        mutations = ((0, b"bad!"), (8, struct.pack("<I", 0)), (152, struct.pack("<I", 0x7fc00000)),
                     (164, struct.pack("<f", 0)), (184, struct.pack("<I", 65)),
                     (188, struct.pack("<I", 0)), (192, struct.pack("<I", 17)),
                     (208, struct.pack("<f", 0)), (224, struct.pack("<f", -1)))
        for offset, value in mutations:
            with self.subTest(offset=offset):
                self.assertEqual(self.parse(body[:offset] + value + body[offset+len(value):]).returncode, 2)
        self.assertEqual(self.parse(body[:-1]).returncode, 2)
        self.assertEqual(self.parse(body+b"x").returncode, 2)
        texture_count, _, light_count = struct.unpack_from("<III", body, 184)
        offset = 196+light_count*32
        for _ in range(texture_count):
            width, height, levels = struct.unpack_from("<III", body, offset)
            offset += 12
            for _ in range(levels):
                offset += width*height*4
                width, height = max(1, width//2), max(1, height//2)
        for relative, value in ((0, struct.pack("<I", texture_count)), (16, struct.pack("<I", 4)),
                                (20+12, struct.pack("<I", 0x7fc00000)),
                                (20+20, struct.pack("<3f", 0, 0, 0)),
                                (20+44, struct.pack("<f", .5))):
            with self.subTest(vertex_offset=relative):
                bad = body[:offset+relative]+value+body[offset+relative+len(value):]
                self.assertEqual(self.parse(bad).returncode, 2)

    def test_gpu_fixture_validates_hdr_material_response_and_extremes(self):
        self.assertEqual(self.parse(gpu_fixture.fixture()).returncode, 0)
        self.assertEqual(self.parse(gpu_fixture.fixture(extreme=True)).returncode, 0)
        self.assertEqual(self.parse(gpu_fixture.fixture(unit_scale=.5)).returncode, 0)
        self.assertEqual(self.parse(gpu_fixture.fixture_srgb_filter()).returncode, 0)
        header = b"P6\n64 64\n255\n"
        image = bytearray(header+bytes(64*64*3))
        for x, y, roughness, ao, normal in gpu_fixture.TILES:
            offset = len(header)+(y*64+x)*3
            image[offset:offset+3] = bytes(gpu_fixture.expected(x,y,roughness,ao,normal)[0])
        self.assertEqual(len(gpu_fixture.verify(image)), 4)
        image[len(header)+(16*64+16)*3] = 0
        with self.assertRaisesRegex(ValueError, "wrong PBR pixel"):
            gpu_fixture.verify(image)
        image[len(header)+(16*64+16)*3:len(header)+(16*64+16)*3+3] = bytes([255]*3)
        self.assertEqual(gpu_fixture.verify_extreme(image), (255, 255, 255))
        image[len(header)+(16*64+16)*3] = 0
        with self.assertRaisesRegex(ValueError, "finite highlight"):
            gpu_fixture.verify_extreme(image)
        for x, y, roughness, ao, normal in gpu_fixture.TILES:
            offset = len(header)+(y*64+x)*3
            image[offset:offset+3] = bytes(gpu_fixture.expected(x,y,roughness,ao,normal,.5)[0])
        self.assertEqual(len(gpu_fixture.verify(image,.5)), 4)
        center = len(header)+(32*64+32)*3
        image[center:center+3] = bytes([156]*3)
        self.assertEqual(gpu_fixture.verify_srgb_filter(image), (156, 156, 156))
        image[center:center+3] = bytes([116]*3)
        with self.assertRaisesRegex(ValueError, "linear light"):
            gpu_fixture.verify_srgb_filter(image)


if __name__ == "__main__":
    unittest.main()
