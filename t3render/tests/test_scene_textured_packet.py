"""SceneIR texture resolution and the portable D3D12 packet boundary."""
from pathlib import Path
import shutil
import struct
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
                    raw[:196] + struct.pack("<I", 0x7fc00000) + raw[200:]):
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
            texture.mips[1].level = 2
            with self.assertRaisesRegex(ValueError, "mip chain"):
                packet.TextureResolver([first, second]).resolve("stone")


if __name__ == "__main__":
    unittest.main()
