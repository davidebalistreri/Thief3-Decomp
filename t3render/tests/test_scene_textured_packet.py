"""SceneIR texture resolution and the portable D3D12 packet boundary."""
from pathlib import Path
import shutil
import struct
import math
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import build_scoped_scene_snapshot as builder
import scene_ir_to_textured_packet as packet
import scene_texture_fixture as gpu_fixture
import scene_ambient_fixture as ambient_fixture
import scene_direct_fixture as direct_fixture
import scene_spot_fixture as spot_fixture
import scene_projector_fixture as projector_fixture
from t3texture import Mip, Texture
from test_scoped_scene_snapshot import fixture

HARNESS = r'''
#include "scene_packet.hpp"
#include <iostream>
#include <iterator>
#include <string>
int main() {
    std::string input((std::istreambuf_iterator<char>(std::cin)), {});
    t3render::scene::Frame frame;
    if (!t3render::scene::ParsePacket(reinterpret_cast<const std::uint8_t*>(input.data()), input.size(), frame)) return 2;
    std::cout << frame.width << " " << frame.height << " " << frame.textures.size() << " " << frame.draws.size()
              << " " << frame.vertices.size() << " " << unsigned(frame.textures[0].mips[0].rgba[0])
              << " " << frame.vertices[0].x << " " << frame.vertices[0].u << "\n";
}
'''


class Resolver:
    def resolve(self, name):
        return {"name": name, "bundle": "fixture.ibt", "format": "RGBA", "width": 2, "height": 2,
                "mips": [bytes([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255])]}


def scoped_scene():
    scene, records = fixture()
    return builder.build(scene, records, 3, 4, 1, 4, "b" * 64, "c" * 64)[0]


class TexturedScenePacketTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("host C++ compiler required")
        cls.temp = tempfile.TemporaryDirectory(prefix="t3render-scene-packet-")
        cls.binary = Path(cls.temp.name) / "parser"
        result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I",
                                 str(HERE.parent / "src"), "-x", "c++", "-o", str(cls.binary), "-"],
                                input=HARNESS, text=True, capture_output=True, timeout=60)
        if result.returncode:
            cls.temp.cleanup()
            raise RuntimeError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def parse(self, raw):
        return subprocess.run([str(self.binary)], input=raw, capture_output=True, timeout=10)

    def test_scoped_geometry_uv_and_order_reach_backend(self):
        scene = scoped_scene()
        raw, stats = packet.create_packet(scene, Resolver())
        self.assertEqual([d["draw"] for d in stats["draws"]], [2, 3, 4, 5])
        self.assertEqual(stats["triangles"], 4)
        self.assertEqual(len(stats["textures"]), 1)
        result = self.parse(raw)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"640 480 1 4 12 255 0 0\n")

    def test_backend_rejects_truncation_bad_counts_nonfinite_or_trailing_data(self):
        raw, _ = packet.create_packet(scoped_scene(), Resolver())
        for bad in (raw[:-1], raw + b"x", b"wrong123" + raw[8:],
                    raw[:8] + struct.pack("<I", 0) + raw[12:],
                    raw[:152] + struct.pack("<I", 65) + raw[156:],
                    raw[:160] + struct.pack("<I", 2049) + raw[164:],
                    raw[:188] + struct.pack("<I", 9) + raw[192:],
                    raw[:244] + struct.pack("<I", 0x7fc00000) + raw[248:]):
            with self.subTest(length=len(bad)):
                self.assertEqual(self.parse(bad).returncode, 2)

    def test_missing_uv_and_unscoped_or_ambiguous_scenes_fail(self):
        scene = scoped_scene()
        data = scene.to_dict()
        data["static_mesh_resources"][0]["geometry"]["uv0"] = None
        with self.assertRaisesRegex(ValueError, "UV0"):
            packet.create_packet(packet.SceneSnapshot.from_dict(data), Resolver())
        data = scene.to_dict()
        data["submissions"].append(dict(data["submissions"][0], stage=2, draw_id=999))
        with self.assertRaisesRegex(ValueError, "unambiguous"):
            packet.create_packet(packet.SceneSnapshot.from_dict(data), Resolver())
        data = scene.to_dict()
        data["version"] = 0
        del data["submissions"]
        with self.assertRaisesRegex(ValueError, "SceneIR v1"):
            packet.create_packet(packet.SceneSnapshot.from_dict(data), Resolver())

    def test_composed_stages_share_a_packet_and_keep_trace_order(self):
        base = scoped_scene()
        data = base.to_dict()
        for item, draw in zip(data["submissions"], (1, 7, 9, 11)):
            item.update(stage=3, draw_id=draw)
        light_stage = packet.SceneSnapshot.from_dict(data)
        states = {s.draw_id: dict(packet.legacy_states.DEFAULT) for s in (*base.submissions, *light_stage.submissions)}
        for item in light_stage.submissions:
            states[item.draw_id].update(depth_compare=3, depth_write=0, blend_enable=1, blend_src=1, blend_dst=1)
        raw, stats = packet.create_packet(base, Resolver(), draw_states=states, additional_scenes=[light_stage])
        self.assertEqual([d["draw"] for d in stats["draws"]], [1, 2, 3, 4, 5, 7, 9, 11])
        self.assertEqual([d["stage"] for d in stats["draws"]], [3, 1, 1, 1, 1, 3, 3, 3])
        self.assertEqual(stats["draws"][0]["state"]["depth_compare"], 3)
        self.assertEqual(self.parse(raw).stdout, b"640 480 1 8 24 255 0 0\n")
        with self.assertRaisesRegex(ValueError, "per-draw states"):
            packet.create_packet(base, Resolver(), additional_scenes=[light_stage])
        with self.assertRaisesRegex(ValueError, "duplicate draw"):
            packet.create_packet(base, Resolver(), draw_states=states,
                                 additional_scenes=[packet.SceneSnapshot.from_dict(dict(data, submissions=[
                                     dict(data["submissions"][0], draw_id=2)]))])

    def test_composition_rejects_mismatched_camera_frame_or_shared_geometry(self):
        base = scoped_scene()
        for change in ("camera", "frame", "geometry", "pass", "stage"):
            data = base.to_dict()
            for item in data["submissions"]:
                item.update(stage=3, draw_id=item["draw_id"] + 100)
            if change == "camera":
                data["camera"]["view"][12] += 1
            elif change == "frame":
                data["source"]["frame_id"] += 1
                data["camera"]["source"]["frame_id"] += 1
            elif change == "geometry":
                data["static_mesh_resources"][0]["geometry"]["positions"][0][0] += 1
            elif change == "pass":
                for item in data["submissions"]:
                    item["render_pass"] += 1
            else:
                for item in data["submissions"]:
                    item["stage"] = 1
            with self.subTest(change=change), self.assertRaises(ValueError):
                packet.create_packet(base, Resolver(), draw_states={},
                                     additional_scenes=[packet.SceneSnapshot.from_dict(data)])

    def test_ambient_candidates_roundtrip_and_invalid_values_are_rejected(self):
        scene = scoped_scene()
        states = {s.draw_id: dict(packet.legacy_states.DEFAULT) for s in scene.submissions}
        raw, info = packet.create_packet(scene, Resolver(), draw_states=states,
                                         ambient_candidates={2: [.125, .25, .375, 1]})
        self.assertEqual(raw[:8], b"T3SCN04\0")
        self.assertEqual(struct.unpack_from("<I4f", raw, 244), (1, .125, .25, .375, 1))
        self.assertEqual(info["draws"][0]["ambient_candidate"], [.125, .25, .375, 1])
        self.assertEqual(self.parse(raw).returncode, 0)
        for bad in (raw[:244] + struct.pack("<I", 2) + raw[248:],
                    raw[:248] + struct.pack("<I", 0x7fc00000) + raw[252:], raw[:-1]):
            self.assertEqual(self.parse(bad).returncode, 2)
        for candidate in ({}, {2: None}, {2: [-1, 0, 0, 1]}, {999: [0, 0, 0, 1]}):
            with self.subTest(candidate=candidate), self.assertRaises(ValueError):
                packet.create_packet(scene, Resolver(), draw_states=states, ambient_candidates=candidate)

    def test_ambient_gpu_fixture_detects_wrong_modulation(self):
        self.assertEqual(self.parse(ambient_fixture.fixture()).returncode, 0)
        header = b"P6\n64 64\n255\n"
        image = bytearray(header + bytes(64 * 64 * 3))
        for (x, y), color in ambient_fixture.EXPECTED.items():
            offset = len(header) + (y * 64 + x) * 3
            image[offset:offset + 3] = bytes(color)
        self.assertEqual(ambient_fixture.verify(image), 2)
        image[len(header) + (32 * 64 + 16) * 3] = 0
        with self.assertRaisesRegex(ValueError, "wrong pixel"):
            ambient_fixture.verify(image)

    def test_normal_transform_preserves_perpendicularity_under_nonuniform_scale(self):
        matrix = [0, 2, 0, 0, -3, 0, 0, 0, 0, 0, 4, 0, 5, 6, 7, 1]
        normal = packet.transform_normal((1, 1, 0), packet.normal_matrix(matrix))
        self.assertAlmostEqual(normal[0], -2 / math.sqrt(13))
        self.assertAlmostEqual(normal[1], 3 / math.sqrt(13))
        self.assertAlmostEqual(normal[0] * 3 + normal[1] * 2, 0)
        mirror = [-2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1]
        flipped = packet.transform_normal((1, 1, 0), packet.normal_matrix(mirror))
        self.assertAlmostEqual(flipped[0], -3 / math.sqrt(13))
        self.assertAlmostEqual(flipped[1], 2 / math.sqrt(13))
        matrix[10] = 0
        with self.assertRaisesRegex(ValueError, "singular"):
            packet.normal_matrix(matrix)

    def test_direct_packet_roundtrip_rejects_missing_normals_or_invalid_light(self):
        scene = scoped_scene()
        states = {s.draw_id: dict(packet.legacy_states.DEFAULT) for s in scene.submissions}
        states[2].update(depth_write=0, blend_enable=1, blend_src=1, blend_dst=1, color_mask=7)
        light = {"mode": "point_approximation", "position": [1, 2, 3], "inverse_radius_candidate": .1,
                 "offline_color": [1, .8, .6], "offline_energy": .5, "gain": 1}
        raw, info = packet.create_packet(scene, Resolver(), draw_states=states, direct_candidates={2: light})
        self.assertEqual(raw[:8], b"T3SCN05\0")
        self.assertEqual(info["draws"][0]["direct_candidate"]["mode"], "point_approximation")
        self.assertEqual(self.parse(raw).returncode, 0)
        data = scene.to_dict()
        data["bsp_blocks"][0]["geometry"]["normals"] = None
        with self.assertRaisesRegex(ValueError, "normals"):
            packet.create_packet(packet.SceneSnapshot.from_dict(data), Resolver(), draw_states=states,
                                 direct_candidates={2: light})
        for invalid in (dict(light, inverse_radius_candidate=0), dict(light, inverse_radius_candidate=1e-60),
                        dict(light, offline_color=[1, 2, 3]),
                        dict(light, position=[float("nan"), 0, 0]), {"mode": "unsupported"}):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                packet.create_packet(scene, Resolver(), draw_states=states, direct_candidates={2: invalid})
        states[2]["depth_write"] = 1
        with self.assertRaisesRegex(ValueError, "additive RGB"):
            packet.create_packet(scene, Resolver(), draw_states=states, direct_candidates={2: light})

    def test_direct_gpu_fixture_checks_point_and_skipped_spot(self):
        raw = direct_fixture.fixture()
        self.assertEqual(self.parse(raw).returncode, 0)
        # Second draw starts after one texture and the first six-vertex draw.
        for bad in (raw[:532] + struct.pack("<I", 4) + raw[536:],
                    raw[:564] + bytes(4) + raw[568:],
                    raw[:604] + bytes(12) + raw[616:]):
            self.assertEqual(self.parse(bad).returncode, 2)
        header = b"P6\n64 64\n255\n"
        image = bytearray(header + bytes(64 * 64 * 3))
        for (x, y), color in direct_fixture.EXPECTED.items():
            offset = len(header) + (y * 64 + x) * 3
            image[offset:offset + 3] = bytes(color)
        self.assertEqual(direct_fixture.verify(image), 2)
        image[len(header) + (32 * 64 + 16) * 3] = 0
        with self.assertRaisesRegex(ValueError, "wrong pixel"):
            direct_fixture.verify(image)

    def test_spot_packet_and_fixture_validate_projector_and_normals(self):
        scene = scoped_scene()
        states = {s.draw_id: dict(packet.legacy_states.DEFAULT) for s in scene.submissions}
        states[2].update(depth_write=0, blend_enable=1, blend_src=1, blend_dst=1, color_mask=7)
        light = {"mode": "spot_approximation", "position": [1, 2, 3], "inverse_radius_candidate": .1,
                 "offline_color": [1, .8, .6], "offline_energy": .5, "gain": 1,
                 "projector_rows": [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]]}
        raw, _ = packet.create_packet(scene, Resolver(), draw_states=states, direct_candidates={2: light})
        self.assertEqual(raw[:8], b"T3SCN06\0")
        self.assertEqual(self.parse(raw).returncode, 0)
        light["projector_rows"][2] = [0, 0, 0, 0]
        with self.assertRaisesRegex(ValueError, "projector"):
            packet.create_packet(scene, Resolver(), draw_states=states, direct_candidates={2: light})
        raw = spot_fixture.fixture()
        self.assertEqual(self.parse(raw).returncode, 0)
        for bad in (raw[:580] + struct.pack("<I", 5) + raw[584:],
                    raw[:612] + bytes(4) + raw[616:],
                    raw[:664] + bytes(12) + raw[676:],
                    raw[:700] + bytes(12) + raw[712:]):
            self.assertEqual(self.parse(bad).returncode, 2)
        header = b"P6\n64 64\n255\n"
        image = bytearray(header + bytes(64 * 64 * 3))
        for (x, y), color in spot_fixture.EXPECTED.items():
            offset = len(header) + (y * 64 + x) * 3
            image[offset:offset + 3] = bytes(color)
        self.assertEqual(spot_fixture.verify(image), 2)
        image[len(header) + (32 * 64 + 32) * 3] = 0
        with self.assertRaisesRegex(ValueError, "wrong pixel"):
            spot_fixture.verify(image)

    def test_projector_packet_accepts_only_matching_texture_metadata(self):
        class ProjectorResolver(Resolver):
            def resolve_projector(self, name):
                return {"name": name, "bundle": "fixture.ibt", "format": "A8R8G8B8", "width": 2, "height": 2,
                        "mips": [bytes([255, 255, 255, 255] * 4)]}
        scene = scoped_scene()
        states = {s.draw_id: dict(packet.legacy_states.DEFAULT) for s in scene.submissions}
        states[2].update(depth_write=0, blend_enable=1, blend_src=1, blend_dst=1, color_mask=7)
        light = {"mode": "spot_approximation", "position": [1, 2, 3], "inverse_radius_candidate": .1,
                 "offline_color": [1, .8, .6], "offline_energy": .5, "gain": 1,
                 "projector_rows": [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]],
                 "projector_candidate": {"name": "window", "width": 2, "height": 2, "levels": 1, "format": 21}}
        raw, info = packet.create_packet(scene, ProjectorResolver(), draw_states=states, direct_candidates={2: light})
        self.assertEqual(raw[:8], b"T3SCN07\0")
        self.assertEqual(len(info["textures"]), 2)
        self.assertEqual(self.parse(raw).returncode, 0)
        light["projector_candidate"]["width"] = 4
        with self.assertRaisesRegex(ValueError, "metadata"):
            packet.create_packet(scene, ProjectorResolver(), draw_states=states, direct_candidates={2: light})
        light["projector_candidate"]["width"] = True
        with self.assertRaisesRegex(ValueError, "invalid projector"):
            packet.create_packet(scene, ProjectorResolver(), draw_states=states, direct_candidates={2: light})
        raw = projector_fixture.fixture()
        self.assertEqual(self.parse(raw).returncode, 0)
        # The second draw's projector index must be within the texture table.
        self.assertEqual(self.parse(raw[:704] + struct.pack("<I", 2) + raw[708:]).returncode, 2)
        header = b"P6\n64 64\n255\n"
        image = bytearray(header + bytes(64 * 64 * 3))
        for (x, y), color in projector_fixture.EXPECTED.items():
            offset = len(header) + (y * 64 + x) * 3
            image[offset:offset + 3] = bytes(color)
        self.assertEqual(projector_fixture.verify(image), 3)
        image[len(header) + (32 * 64 + 24) * 3] = 0
        with self.assertRaisesRegex(ValueError, "wrong pixel"):
            projector_fixture.verify(image)

    def test_exclusive_write_does_not_overwrite_or_leave_first_file(self):
        with tempfile.TemporaryDirectory(prefix="t3render-textured-output-") as directory:
            output = Path(directory) / "scene.bin"
            sidecar = Path(str(output) + ".manifest.json")
            sidecar.write_text("existing")
            with self.assertRaises(FileExistsError):
                packet.write_pair(output, b"new", {})
            self.assertFalse(output.exists())
            self.assertEqual(sidecar.read_text(), "existing")

    def test_gpu_fixture_and_checker_reject_wrong_results(self):
        self.assertEqual(self.parse(gpu_fixture.fixture()).returncode, 0)
        image = bytearray(b"P6\n64 64\n255\n" + bytes(64 * 64 * 3))
        header = len(b"P6\n64 64\n255\n")
        for (x, y), color in gpu_fixture.EXPECTED.items():
            offset = header + (y * 64 + x) * 3
            image[offset:offset + 3] = bytes(color)
        self.assertEqual(len(gpu_fixture.verify(image)), 6)
        image[header + (32 * 64 + 32) * 3] = 0
        with self.assertRaisesRegex(ValueError, "wrong pixel"):
            gpu_fixture.verify(image)

    def test_matlib_binding_and_mip_validation_use_bundles_in_order(self):
        class Bundle:
            def __init__(self, name, resources):
                self.path = Path(name)
                self.resources = resources
            def split(self, resource):
                return resource.payload
        mat = SimpleNamespace(name="Stone", type=packet.TYPE_MATERIAL,
                              payload=SimpleNamespace(texture=lambda stage: "Diffuse" if stage == 0 else None))
        texture = Texture("Diffuse", "A8R8G8B8", 2, 1, 0, 0,
                          [Mip(2, 1, bytes((3, 2, 1, 255, 6, 5, 4, 255)), 0),
                           Mip(1, 1, bytes((9, 8, 7, 255)), 1)])
        tex = SimpleNamespace(name="Diffuse", type=packet.TYPE_TEXTURE, payload=texture)
        first, second = Bundle("local.ibt", [mat]), Bundle("kernel.ibt", [tex])
        with mock.patch.object(packet, "parse_material", side_effect=lambda resource, payload: payload), \
             mock.patch.object(packet, "parse_texture", side_effect=lambda resource, payload: payload):
            resolver = packet.TextureResolver([first, second])
            resolved = resolver.resolve("sToNe")
            self.assertEqual(resolved["bundle"], "kernel.ibt")
            self.assertEqual(resolved["mips"], [bytes((1, 2, 3, 255, 4, 5, 6, 255)), bytes((7, 8, 9, 255))])
            alias = SimpleNamespace(name="Alias", type=packet.TYPE_MATERIAL, payload=mat.payload)
            first.resources.append(alias)
            self.assertIs(resolver.resolve("Alias"), resolved)
            self.assertEqual(resolver.decoded_bytes, 12)
            with mock.patch.object(packet, "MAX_BYTES", 11):
                with self.assertRaisesRegex(ValueError, "decoded textures"):
                    packet.TextureResolver([first, second]).resolve("stone")
            first.resources.append(mat)
            with self.assertRaisesRegex(ValueError, "ambiguous"):
                packet.TextureResolver([first, second]).resolve("stone")
            first.resources.pop()
            duplicate_bundle = Bundle("local.ibt", [tex, tex])
            duplicate_resolver = packet.TextureResolver([duplicate_bundle])
            self.assertEqual(duplicate_resolver.resolve_projector("diffuse")["name"], "Diffuse")
            altered = SimpleNamespace(name="Diffuse", type=packet.TYPE_TEXTURE, payload="different")
            duplicate_bundle.resources.append(altered)
            with self.assertRaisesRegex(ValueError, "ambiguous projector"):
                packet.TextureResolver([duplicate_bundle]).resolve_projector("Diffuse")
            texture.mips[1].level = 2
            with self.assertRaisesRegex(ValueError, "mip chain"):
                packet.TextureResolver([first, second]).resolve("stone")


if __name__ == "__main__":
    unittest.main()
