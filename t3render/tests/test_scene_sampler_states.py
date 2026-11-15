import copy
import hashlib
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
import build_scoped_scene_snapshot as scoped
import scene_ir_to_textured_packet as packet
import scene_legacy_states as legacy
import scene_sampler_fixture as gpu_fixture
from test_scene_pipeline_states import observed_state
from test_scene_textured_packet import Resolver
from test_scoped_scene_snapshot import fixture

HARNESS = r'''
#include "scene_packet.hpp"
#include <iostream>
#include <iterator>
#include <string>
int main() {
    std::string bytes((std::istreambuf_iterator<char>(std::cin)), {});
    t3render::scene::Frame scene;
    if (!t3render::scene::ParsePacket(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), scene)) return 2;
    for (const auto& draw : scene.draws) std::cout << draw.state.mip_lod_bias << " ";
    std::cout << "\n";
}
'''


def stage(texture=True):
    return {"stage": 2, "states": {"13": 1, "14": 1, "16": 2, "17": 2, "18": 2,
                                   "19": struct.unpack("<I", struct.pack("<f", -1))[0], "20": 0, "24": 0},
            "texture": {"type": 3, "width": 2, "height": 2, "levels": 1,
                        "format": struct.unpack("<I", b"DXT1")[0], "id": "0xabcdef"} if texture else None}


class SamplerObservationTests(unittest.TestCase):
    def test_sampler_candidate_is_metadata_only_and_distinguishes_unavailable(self):
        observed = legacy.sampler_candidate({"stages": [stage()]}, 2)
        self.assertEqual(observed["lod_bias"], -1)
        self.assertNotIn("id", observed)
        self.assertEqual(legacy.sampler_candidate({"stages": [stage(False)]}, 2)["status"], "unbound")
        failed = stage(False)
        failed["texture_hr"] = -1
        self.assertEqual(legacy.sampler_candidate({"stages": [failed]}, 2)["status"], "unavailable")
        for change in ("address", "mip", "lod", "texture", "duplicate", "missing"):
            entry = stage()
            if change == "address": entry["states"]["13"] = 3
            elif change == "mip": entry["states"]["18"] = 1
            elif change == "lod": entry["states"]["19"] = 0x7fc00000
            elif change == "texture": entry["texture"]["type"] = 5
            elif change == "missing": del entry["texture"]
            draws = [entry, entry] if change == "duplicate" else [entry]
            with self.subTest(change=change), self.assertRaises(ValueError):
                legacy.sampler_candidate({"stages": draws}, 2)

    def test_metadata_match_is_a_hypothesis_and_mismatch_fails(self):
        texture = {"format": "DXT1", "width": 2, "height": 2, "mips": [b"..."]}
        candidate = legacy.sampler_candidate({"stages": [stage()]}, 2)
        match = packet.match_sampler(candidate, texture)
        self.assertEqual(match, {"status": "metadata_match_not_identity", "stage": 2, "mip_lod_bias": -1.0})
        altered = dict(candidate, width=4)
        with self.assertRaisesRegex(ValueError, "metadata"):
            packet.match_sampler(altered, texture)
        with self.assertRaisesRegex(ValueError, "rounds out of range"):
            packet.match_sampler(dict(candidate, lod_bias=15.9999999), texture)
        self.assertEqual(packet.match_sampler({"status": "unbound", "stage": 2}, texture)["mip_lod_bias"], 0)

    def test_trace_revalidation_preserves_candidates_and_fallbacks(self):
        offline, records = fixture()
        for row in records:
            if row.get("event") == "draw":
                row["render_states"] = observed_state()
                row["stages"] = [stage(row["op_id"] != 5)]
        offline_bytes = offline.to_json().encode()
        digest_offline = hashlib.sha256(offline_bytes).hexdigest()
        raw = "".join(json.dumps(row) + "\n" for row in records).encode()
        scene, _ = scoped.build(offline, records, 3, 4, 1, 4, hashlib.sha256(raw).hexdigest(), digest_offline)
        with tempfile.TemporaryDirectory(prefix="t3render-sampler-trace-") as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_bytes(raw)
            candidates = {}
            render_states = legacy.from_trace(scene, offline, offline_bytes, path, 4, 1,
                                              sampler_stage=2, samplers=candidates)
            class DxtResolver(Resolver):
                def resolve(self, name):
                    return dict(super().resolve(name), format="DXT1")
            body, info = packet.create_packet(scene, DxtResolver(), 4, 1, render_states, candidates)
            self.assertEqual(body[:8], b"T3SCN03\0")
            self.assertEqual([d["sampler"]["status"] for d in info["draws"]],
                             ["metadata_match_not_identity"] * 3 + ["unbound_default"])
            self.assertEqual([d["sampler"]["mip_lod_bias"] for d in info["draws"]], [-1, -1, -1, 0])
            bad = copy.deepcopy(candidates)
            bad[2]["width"] = 3
            with self.assertRaisesRegex(ValueError, "metadata"):
                packet.create_packet(scene, DxtResolver(), 4, 1, render_states, bad)

    def test_fixture_checker_requires_all_three_lods(self):
        image = bytearray(b"P6\n64 64\n255\n" + bytes(64 * 64 * 3))
        header = len(b"P6\n64 64\n255\n")
        for (x, y), color in gpu_fixture.EXPECTED.items():
            offset = header + (y * 64 + x) * 3
            image[offset:offset + 3] = bytes(color)
        self.assertEqual(len(gpu_fixture.verify(image)), 3)
        image[header + (32 * 64 + 32) * 3] = 0
        with self.assertRaisesRegex(ValueError, "wrong pixel"):
            gpu_fixture.verify(image)


class SamplerPacketTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("host C++ compiler required")
        cls.temp = tempfile.TemporaryDirectory(prefix="t3render-sampler-packet-")
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

    def parse(self, data):
        return subprocess.run([str(self.binary)], input=data, capture_output=True, timeout=10)

    def test_packet_preserves_bias_and_rejects_nonfinite_and_out_of_range(self):
        data = gpu_fixture.fixture()
        result = self.parse(data)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b"0 -1 1 \n")
        bias_offset = 160 + 12 + 64 + 16 + 4 + 52
        for bits in (0x7fc00000, 0x7f800000, struct.unpack("<I", struct.pack("<f", 16))[0]):
            bad = bytearray(data)
            struct.pack_into("<I", bad, bias_offset, bits)
            self.assertEqual(self.parse(bad).returncode, 2)
        self.assertEqual(self.parse(data[:-1]).returncode, 2)


if __name__ == "__main__":
    unittest.main()
