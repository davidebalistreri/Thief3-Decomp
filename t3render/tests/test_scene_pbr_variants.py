"""Semantic variant bundles switch in memory without changing unrelated draws."""
import copy
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
import scene_pbr_variants as variants
import test_scene_pbr_bed as bed_tests
import test_scene_pbr_offline_casters as caster_tests
import test_scene_pbr_packet as packet_tests
from test_scene_material_overrides import FakeRemaster
from test_scene_textured_packet import Resolver

HARNESS = r'''
#include "scene_pbr_variants.hpp"
#include <iostream>
#include <iterator>
#include <string>
int main(int argc, char** argv) {
    if (argc != 2) return 3;
    std::string raw((std::istreambuf_iterator<char>(std::cin)), {});
    t3render::pbr::Variants variants;
    if (!variants.Parse(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size())) return 2;
    if (!variants.Select(argv[1], false)) return 4;
    const auto* original = &variants.Current();
    if (!variants.Select(argv[1], true)) return 4;
    const auto* replacement = &variants.Current();
    if (original == replacement || variants.Select("wrong-resource", false) || &variants.Current() != replacement) return 5;
    if (!variants.Select(argv[1], false) || &variants.Current() != original) return 5;
    std::cout << original->draws.size() << " " << replacement->draws.size() << "\n";
}
'''


def inputs():
    scene, lights, config = packet_tests.inputs()
    args = (scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene))
    original, old = pbr.create_packet(*args)
    replacement, new = pbr.create_packet(*args, bed_glb=bed_tests.glb(scene))
    return scene, original, old, replacement, new


class VariantTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("host C++ compiler required")
        cls.temp = tempfile.TemporaryDirectory(prefix="t3render-variants-parser-")
        cls.binary = Path(cls.temp.name)/"parser"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(HERE.parent/"src"),
                        "-x", "c++", "-o", str(cls.binary), "-"], input=HARNESS, text=True,
                       check=True, capture_output=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def parse(self, body, key):
        return subprocess.run([str(self.binary), key], input=body, capture_output=True, timeout=10)

    def test_switches_by_resource_key_and_preserves_both_packets(self):
        scene, original, old, replacement, new = inputs()
        body, report = variants.build(scene, original, old, replacement, new)
        magic, key_size, old_size, new_size = struct.unpack_from("<8sIII", body)
        key = body[20:20+key_size].decode("ascii")
        self.assertEqual(magic, b"T3VAR01\0")
        self.assertEqual(key, report["resource_key"])
        self.assertEqual(body[20+key_size:20+key_size+old_size], original)
        self.assertEqual(body[20+key_size+old_size:], replacement)
        self.assertEqual((old_size, new_size), (len(original), len(replacement)))
        self.assertEqual(self.parse(body, key).returncode, 0)
        self.assertEqual(self.parse(body, "wrong-resource").returncode, 4)
        self.assertEqual(self.parse(body[:-1], key).returncode, 2)
        invalid = bytearray(body)
        struct.pack_into("<I", invalid, 8, 513)
        self.assertEqual(self.parse(bytes(invalid), key).returncode, 2)
        changed = bytearray(body)
        # The replacement camera is inconsistent with the first packet.
        struct.pack_into("<f", changed, 20+key_size+old_size+24, 2)
        self.assertEqual(self.parse(bytes(changed), key).returncode, 2)
        changed = bytearray(body)
        # The replacement lighting is inconsistent with the first packet.
        struct.pack_into("<f", changed, 20+key_size+old_size+196+28, 1)
        self.assertEqual(self.parse(bytes(changed), key).returncode, 2)

    def test_rejects_unrelated_scene_and_metadata_changes(self):
        scene, original, old, replacement, new = inputs()
        changed = copy.deepcopy(new)
        changed["draws"][0]["material"] = "unrelated"
        with self.assertRaisesRegex(ValueError, "unrelated draws"):
            variants.build(scene, original, old, replacement, changed)
        changed = copy.deepcopy(new)
        changed["generated_bed"]["resource_id"] = "asset:other"
        with self.assertRaisesRegex(ValueError, "semantic resource"):
            variants.build(scene, original, old, replacement, changed)
        changed = copy.deepcopy(new)
        changed["lights"][0]["intensity_candela"] = 99
        with self.assertRaisesRegex(ValueError, "lights"):
            variants.build(scene, original, old, replacement, changed)
        binary = bytearray(replacement)
        first_vertex = variants.packet_signatures(replacement)[0][0][1]
        struct.pack_into("<f", binary, first_vertex, 42)
        with self.assertRaisesRegex(ValueError, "unrelated draw geometry"):
            variants.build(scene, original, old, bytes(binary), new)
        binary = bytearray(replacement)
        struct.pack_into("<f", binary, 196+28, 1)
        with self.assertRaisesRegex(ValueError, "camera, lighting"):
            variants.build(scene, original, old, bytes(binary), new)

    def test_switches_bed_casters_without_changing_unrelated_casters(self):
        scene, lights, config, offline, raw, actors = caster_tests.fixture()
        args = (scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene))
        options = {"shadows": caster_tests.SHADOW, "actors": actors, "offline": offline,
                   "offline_raw": raw, "caster_resolve": Resolver().resolve}
        original, old = pbr.create_packet(*args, **options)
        replacement, new = pbr.create_packet(*args, bed_glb=bed_tests.glb(scene), **options)
        target = scene.mesh_instances[0].id
        old_sections = old["shadow_maps"]["offline_candidates"]["sections"]
        new_sections = new["shadow_maps"]["offline_candidates"]["sections"]
        self.assertTrue(any(x["target_id"] == target for x in old_sections))
        self.assertFalse(any(x["target_id"] == target for x in new_sections))
        body, report = variants.build(scene, original, old, replacement, new)
        self.assertEqual(self.parse(body, report["resource_key"]).returncode, 0)
        changed = copy.deepcopy(new)
        changed["shadow_maps"]["offline_candidates"]["sections"][0]["section"] += 1
        with self.assertRaisesRegex(ValueError, "unrelated offline casters"):
            variants.build(scene, original, old, replacement, changed)
        binary = bytearray(replacement); binary[-1] ^= 1
        with self.assertRaisesRegex(ValueError, "unrelated caster geometry"):
            variants.build(scene, original, old, bytes(binary), new)


if __name__ == "__main__":
    unittest.main()
