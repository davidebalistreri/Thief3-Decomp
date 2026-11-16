"""Semantic variant bundles switch in memory without changing unrelated draws."""
import copy
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
import scene_pbr_variants as variants
import scene_pbr_variants_fixture as gpu_fixture
import test_scene_pbr_bed as bed_tests
import test_scene_pbr_offline_casters as caster_tests
import test_scene_pbr_packet as packet_tests
from test_scene_material_overrides import FakeRemaster
from test_scene_textured_packet import Resolver
from gltf import GLTFBuilder

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
    t3render::pbr::Frame combined; std::vector<std::uint8_t> draws, casters;
    if (!variants.BuildCombined(combined, draws, casters) ||
        draws.size() != original->draws.size()+replacement->draws.size() ||
        casters.size() != original->offline_casters.size()+replacement->offline_casters.size()) return 6;
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const bool old = i < original->draws.size();
        const auto& source = old ? original->draws[i] : replacement->draws[i-original->draws.size()];
        const auto& merged = combined.draws[i];
        if (draws[i] != (old ? 1 : 2) || merged.vertex_count != source.vertex_count ||
            std::memcmp(combined.vertices.data()+merged.first_vertex,
                        (old ? original : replacement)->vertices.data()+source.first_vertex,
                        source.vertex_count*sizeof(t3render::scene::Vertex))) return 6;
        for (unsigned k = 0; k < 4; ++k) {
            const auto& a = (old ? original : replacement)->textures[source.textures[k]].mips;
            const auto& b = combined.textures[merged.textures[k]].mips;
            if (a.size() != b.size()) return 6;
            for (std::size_t j = 0; j < a.size(); ++j)
                if (a[j].width != b[j].width || a[j].height != b[j].height || a[j].rgba != b[j].rgba) return 6;
        }
    }
    if (!variants.Select(argv[1], false) || &variants.Current() != original) return 5;
    std::uint32_t key_size = 0, old_size = 0;
    std::memcpy(&key_size, raw.data()+8, 4); std::memcpy(&old_size, raw.data()+12, 4);
    std::string update = raw.substr(20+key_size, old_size);
    const float view_value = original->view[12] + .125f;
    const float projection_value = original->projection[0] + .125f;
    const float camera_value = original->camera[0] + .25f;
    std::memcpy(&update[24+12*4], &view_value, 4); std::memcpy(&update[24+16*4], &projection_value, 4);
    std::memcpy(&update[152], &camera_value, 4);
    if (!variants.UpdateCamera(reinterpret_cast<const std::uint8_t*>(update.data()), update.size()) ||
        variants.Current().view[12] != view_value || variants.Current().projection[0] != projection_value ||
        variants.Current().camera[0] != camera_value ||
        !variants.Select(argv[1], true) || variants.Current().view[12] != view_value ||
        variants.Current().projection[0] != projection_value || variants.Current().camera[0] != camera_value ||
        !variants.BuildCombined(combined, draws, casters)) return 7;
    std::string invalid = update; invalid.back() ^= 1;
    if (variants.UpdateCamera(reinterpret_cast<const std::uint8_t*>(invalid.data()), invalid.size()) ||
        variants.Current().view[12] != view_value) return 8;
    invalid = update; invalid[8] ^= 1;
    if (variants.UpdateCamera(reinterpret_cast<const std::uint8_t*>(invalid.data()), invalid.size()) ||
        variants.UpdateCamera(reinterpret_cast<const std::uint8_t*>(update.data()), update.size()-1)) return 8;
    invalid = update; const std::uint32_t nan = 0x7fc00000;
    std::memcpy(&invalid[24], &nan, 4);
    if (variants.UpdateCamera(reinterpret_cast<const std::uint8_t*>(invalid.data()), invalid.size()) ||
        variants.Current().view[12] != view_value || variants.Current().camera[0] != camera_value) return 8;
    std::cout << original->draws.size() << " " << replacement->draws.size() << "\n";
}
'''

UPDATE_HARNESS = r'''
#include "scene_pbr_variants.hpp"
#include <iostream>
#include <iterator>
int main(int argc, char** argv) {
    if (argc != 2) return 3;
    std::string raw((std::istreambuf_iterator<char>(std::cin)), {});
    t3render::scene::Reader reader(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size());
    std::uint32_t a = 0, b = 0;
    if (!reader.u32(a) || !reader.u32(b) || a > reader.remaining() || b != reader.remaining()-a) return 3;
    std::vector<std::uint8_t> first(a), second(b);
    if (!reader.bytes(first.data(), a) || !reader.bytes(second.data(), b) || !reader.end()) return 3;
    t3render::pbr::Variants resident, incoming;
    if (!resident.Parse(first.data(), first.size()) || !incoming.Parse(second.data(), second.size()) ||
        !resident.Select(argv[1], true)) return 3;
    std::vector<std::uint32_t> draws[2], casters[2];
    if (!resident.UpdateScene(second.data(), second.size(), draws, casters)) return 4;
    if (!resident.ReplacementSelected()) return 5;
    t3render::pbr::Frame combined; std::vector<std::uint8_t> draw_tags, caster_tags;
    if (!resident.BuildCombined(combined, draw_tags, caster_tags)) return 5;
    for (unsigned v=0; v<2; ++v) {
        if (!incoming.Select(argv[1], v != 0) || draws[v].size() != incoming.Current().draws.size() ||
            casters[v].size() != incoming.Current().offline_casters.size()) return 5;
        for (std::size_t i=0; i<draws[v].size(); ++i)
            if (draws[v][i] >= combined.draws.size() || draw_tags[draws[v][i]] != v+1 ||
                !t3render::pbr::SameDraw(combined, combined.draws[draws[v][i]], incoming.Current(), incoming.Current().draws[i])) return 5;
        for (std::size_t i=0; i<casters[v].size(); ++i)
            if (casters[v][i] >= combined.offline_casters.size() || caster_tags[casters[v][i]] != v+1 ||
                !t3render::pbr::SameCaster(combined, combined.offline_casters[casters[v][i]],
                                           incoming.Current(), incoming.Current().offline_casters[i])) return 5;
    }
    const auto before = draws[0];
    second.back() ^= 1;
    if (resident.UpdateScene(second.data(), second.size(), draws, casters) || draws[0] != before ||
        !resident.ReplacementSelected()) return 5;
    if (!resident.UpdateScene(first.data(), first.size(), draws, casters)) return 5;
    std::cout << before.size() << " " << draws[0].size() << "\n";
}
'''


def inputs():
    scene, lights, config = packet_tests.inputs()
    args = (scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene))
    original, old = pbr.create_packet(*args)
    replacement, new = pbr.create_packet(*args, bed_glb=bed_tests.glb(scene))
    return scene, original, old, replacement, new


def distinct_bed(scene):
    geometry, scale = scene.static_mesh_resources[0].geometry, scene.conventions.meters_per_unit
    builder = GLTFBuilder()
    positions = [(x*scale, z*scale, y*scale) for x, y, z in geometry.positions]
    normals = [(x, z, y) for x, y, z in geometry.normals]
    attrs = {"POSITION": builder.add_floats(positions, "VEC3"),
             "NORMAL": builder.add_floats(normals, "VEC3"),
             "TEXCOORD_0": builder.add_floats(geometry.uv0, "VEC2")}
    indices = (1, 3, 4, 5, 6, 7)
    primitives = [{"attributes": attrs, "indices": builder.add_indices((0, 2, index)),
                   "material": builder.material(name)} for index, name in zip(indices, pbr.bed.MATERIALS)]
    builder.node("bed", builder.mesh("bed", primitives), root=True)
    with tempfile.TemporaryDirectory(prefix="t3render-distinct-bed-") as directory:
        path = Path(directory)/"bed.glb"; builder.write_glb(path)
        return path.read_bytes()


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
        cls.updater = Path(cls.temp.name)/"updater"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(HERE.parent/"src"),
                        "-x", "c++", "-o", str(cls.updater), "-"], input=UPDATE_HARNESS, text=True,
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

    def test_bed_switch_keeps_spot_direction_in_resident_context(self):
        scene, lights, config = packet_tests.inputs()
        lights = replace(lights, lights=(replace(lights.lights[0], kind="spot"),))
        config["version"] = 2
        config["lights"][0].update(direction_world=[0, 0, 1], inner_degrees=12, outer_degrees=25)
        args = (scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene))
        original, old = pbr.create_packet(*args)
        replacement, new = pbr.create_packet(*args, bed_glb=bed_tests.glb(scene))
        body, report = variants.build(scene, original, old, replacement, new)
        self.assertEqual(self.parse(body, report["resource_key"]).returncode, 0)
        key_size, old_size = struct.unpack_from("<II", body, 8)
        changed = bytearray(body)
        struct.pack_into("<3f", changed, 20+key_size+old_size+200+32, 1, 0, 0)
        self.assertEqual(self.parse(bytes(changed), report["resource_key"]).returncode, 2)

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

    def test_combined_texture_limit_fails_without_partial_state(self):
        def packet(seed):
            raw = packet_tests.gpu_fixture.fixture()
            header = bytearray(raw[:196]); struct.pack_into("<I", header, 184, 64)
            texture_end = 196+32+7*16
            extra = b"".join(struct.pack("<III4B", 1, 1, 1, n, seed, 123, 255) for n in range(57))
            return bytes(header)+raw[196:texture_end]+extra+raw[texture_end:]
        key = b"asset:bed"
        first, second = packet(1), packet(2)
        def bundle(a, b):
            return struct.pack("<8sIII", b"T3VAR01\0", len(key), len(a), len(b))+key+a+b
        self.assertEqual(self.parse(bundle(first, first), key.decode()).returncode, 0)
        self.assertEqual(self.parse(bundle(first, second), key.decode()).returncode, 6)

    def test_synthetic_gpu_bundle_is_accepted(self):
        self.assertEqual(self.parse(gpu_fixture.bundle(), gpu_fixture.KEY).returncode, 0)

    def test_synthetic_gpu_subset_uses_resident_draws(self):
        first, second = gpu_fixture.bundle(), gpu_fixture.subset_bundle()
        raw = struct.pack("<II", len(first), len(second)) + first + second
        result = subprocess.run([str(self.updater), gpu_fixture.KEY], input=raw, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.split(), [b"3", b"4"])

    def test_resident_scene_accepts_verified_subsets_and_rejects_new_geometry(self):
        scene, lights, config = packet_tests.inputs()
        data = scene.to_dict(); geometry = data["static_mesh_resources"][0]["geometry"]
        for k in range(1, 6):
            geometry["positions"].append([k/8, 0, 0]); geometry["uv0"].append([k/8, 0])
            geometry["normals"].append([0, 0, 1]); geometry["tangents"].append([1, 0, 0])
        geometry["indices"][6:9] = [0, 3, 2]
        scene = pbr.SceneSnapshot.from_dict(data)
        lights = replace(lights, static_mesh_resources=scene.static_mesh_resources)
        args = (scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene))
        original, old = pbr.create_packet(*args)
        replacement, new = pbr.create_packet(*args, bed_glb=distinct_bed(scene))
        base, report = variants.build(scene, original, old, replacement, new)
        data = scene.to_dict()
        removed = next(s for s in data["submissions"] if s["target_kind"] == "bsp_block")
        data["submissions"].remove(removed)
        data["submissions"].reverse()
        subset = pbr.SceneSnapshot.from_dict(data)
        light = packet_tests.inputs()[1].lights[0]
        lights = replace(subset, lights=(light,))
        config = packet_tests.inputs()[2]
        args = (subset, lights, json.dumps(config), Resolver(), FakeRemaster(subset))
        partial_original, partial_old = pbr.create_packet(*args)
        partial_replacement, partial_new = pbr.create_packet(*args, bed_glb=distinct_bed(subset))
        partial, _ = variants.build(subset, partial_original, partial_old, partial_replacement, partial_new)
        def update(first, second):
            raw = struct.pack("<II", len(first), len(second))+first+second
            return subprocess.run([str(self.updater), report["resource_key"]], input=raw, capture_output=True, timeout=10)
        result = update(base, partial)
        self.assertEqual(result.returncode, 0, result.stderr)
        reduced, full = map(int, result.stdout.split())
        self.assertLess(reduced, full)
        changed = bytearray(partial)
        _, key_size, original_size, _ = struct.unpack_from("<8sIII", changed)
        replacement_offset = 20+key_size+original_size
        vertex = variants.packet_signatures(bytes(changed[replacement_offset:]))[0][-1][1]
        value = struct.unpack_from("<f", changed, replacement_offset+vertex)[0]
        struct.pack_into("<f", changed, replacement_offset+vertex, value+.125)
        self.assertEqual(update(base, bytes(changed)).returncode, 4)
        ambiguous_scene, original, old, replacement, new = inputs()
        ambiguous, _ = variants.build(ambiguous_scene, original, old, replacement, new)
        self.assertEqual(update(ambiguous, ambiguous).returncode, 4)


if __name__ == "__main__":
    unittest.main()
