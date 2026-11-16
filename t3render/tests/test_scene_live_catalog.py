"""Offline asset catalogs must not masquerade as runtime frames."""
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
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools"))
import export_live_catalog as catalog
import scene_ir
import test_scene_ir
import test_scene_pbr_bed as bed_tests


def fixture():
    data = test_scene_ir.example()
    source = data["source"]
    map_name, map_hash, bundle_hash = source["map"], source["fingerprints"]["map"], "b" * 64
    source["fingerprints"]["bundle"] = bundle_hash
    actor = f"actor:{map_name}:{map_hash}:StaticMeshActor__1520"
    bed = f"mesh:{map_name}:{map_hash}:{bundle_hash}:innbed1a1"
    data["mesh_instances"][0].update(id=actor, resource_id=bed)
    data["static_mesh_resources"][0]["id"] = bed
    block = data["bsp_blocks"][0]
    block["id"] = f"bsp:{map_name}:{map_hash}:{block['source_index']}:{block['source_offset']}"
    block["geometry"]["sections"][0]["material_id"] = "material:wood"
    return data


class LiveCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("host C++ compiler required")
        cls.temp = tempfile.TemporaryDirectory(prefix="t3render-catalog-fixture-")
        cls.binary = Path(cls.temp.name) / "fixture"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                        str(ROOT / "tests/scene_live_catalog_fixture.cpp"), str(ROOT / "ir/scene_live.cpp"),
                        str(ROOT / "src/scene_live_catalog.cpp"), "-o", str(cls.binary)],
                       check=True, capture_output=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def load(self, data):
        digest = hashlib.sha256(data[:-32]).hexdigest() if len(data) >= 32 else "0" * 64
        return subprocess.run([str(self.binary), digest], input=data, capture_output=True,
                              check=True, timeout=30).stdout.decode().strip()

    def test_round_trip_preserves_offline_assets_and_attributes(self):
        raw = json.dumps(fixture()).encode()
        data, manifest = catalog.export(raw)
        result = self.load(data).split()
        self.assertEqual(result[:10], ["accepted", "SouthQuarter1_int1", "1", "1", "3", "3", "3", "3", "3", "1"])
        self.assertEqual(result[10], hashlib.sha256(raw).hexdigest())
        self.assertEqual(manifest["source"], "offline")
        self.assertEqual(manifest["catalog_sha256"], hashlib.sha256(data).hexdigest())

    def test_integrity_truncation_and_extra_bytes_rejected_atomically(self):
        data, _ = catalog.export(json.dumps(fixture()).encode())
        changed = bytearray(data); changed[12] ^= 1
        self.assertEqual(self.load(bytes(changed)), "rejected")
        self.assertEqual(self.load(data[:-1]), "rejected")
        body = data[:-32] + b"extra"
        self.assertEqual(self.load(body + hashlib.sha256(body).digest()), "rejected")
        self.assertEqual(self.load(b"short"), "rejected")

    def test_high_poly_override_round_trips_without_replacing_the_source(self):
        data = fixture()
        data["conventions"]["meters_per_unit"] = .01905
        scene = scene_ir.SceneSnapshot.from_dict(data)
        high = bed_tests.glb(scene)
        body, manifest = catalog.export(json.dumps(data).encode(), high)
        result = self.load(body).split()
        self.assertEqual(body[:8], b"T3LCAT02")
        self.assertEqual(result[:10], ["accepted", "SouthQuarter1_int1", "1", "1", "3", "3", "3", "3", "3", "1"])
        self.assertEqual(result[11:15], ["override", "18", "18", "6"])
        self.assertEqual(result[15], hashlib.sha256(high).hexdigest())
        self.assertEqual(manifest["bed_override"]["asset_sha256"], result[15])
        self.assertEqual(manifest["bed_override"]["scope"], "viewer_only_when_semantic_bed_is_selected")
        _, _, geometry = catalog._bed_override(scene, scene.static_mesh_resources[0], high)
        # The fixture's glTF triangle is reflected from T3 and uses reversed
        # indices; conversion must restore coordinates, normals and winding.
        for actual, expected in zip(geometry.positions[:3], ((0, 0, 0), (1, 0, 0), (0, 1, 0))):
            for a, b in zip(actual, expected):
                self.assertAlmostEqual(a, b, places=6)
        self.assertEqual(geometry.normals[:3], ((0, 0, 1),) * 3)
        self.assertEqual(geometry.uv0[:3], ((0, 0), (1, 0), (0, 1)))
        self.assertEqual(geometry.indices[:6], (0, 1, 2, 3, 4, 5))
        self.assertEqual((geometry.sections[1].first_index, geometry.sections[1].index_count), (3, 3))

        def rebuild(changed):
            return bytes(changed) + hashlib.sha256(changed).digest()

        raw = bytearray(body[:-32])
        marker = raw.find(b"remaster:mesh:bed_high:")
        self.assertGreater(marker, 0)
        bad = raw.copy();bad[marker + len(b"remaster:mesh:bed_high:")] = ord("0") if bad[marker + len(b"remaster:mesh:bed_high:")] != ord("0") else ord("1")
        self.assertEqual(self.load(rebuild(bad)), "rejected")
        geometry = raw.find(struct.pack("<4I", 18, 18, 6, 3), marker)
        self.assertGreater(geometry, marker)
        bad = raw.copy();struct.pack_into("<f", bad, geometry + 16, 200.0)
        self.assertEqual(self.load(rebuild(bad)), "rejected")
        bad = raw.copy();struct.pack_into("<f", bad, geometry + 16 + 18*12, 100.0)
        self.assertEqual(self.load(rebuild(bad)), "rejected")
        material = raw.find(b"remaster:material:bed_high:BedTrimWood")
        self.assertGreater(material, 0)
        bad = raw.copy();bad[material + len(b"remaster:material:bed_high:")] = ord("X")
        self.assertEqual(self.load(rebuild(bad)), "rejected")

    def test_generated_bed_rejects_wrong_geometry_and_unit_scale(self):
        data = fixture();data["conventions"]["meters_per_unit"] = .01905
        scene = scene_ir.SceneSnapshot.from_dict(data)
        for raw, message in ((bed_tests.glb(scene, shift=1), "bounds"),
                             (bed_tests.glb(scene, names=bed_tests.bed.MATERIALS[:-1]), "materials")):
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                catalog.export(json.dumps(data).encode(), raw)
        data["conventions"]["meters_per_unit"] = None
        with self.assertRaisesRegex(ValueError, "unit scale"):
            catalog.export(json.dumps(data).encode(), bed_tests.glb(scene))

    def test_structurally_invalid_count_and_magic_are_rejected_even_with_new_digest(self):
        data, _ = catalog.export(json.dumps(fixture()).encode())
        for offset, value in ((0, b"BAD!"), (8, struct.pack("<I", 0xffffffff))):
            body = bytearray(data[:-32]); body[offset:offset+4] = value
            self.assertEqual(self.load(bytes(body) + hashlib.sha256(body).digest()), "rejected")

    def test_runtime_source_or_unqualified_assets_are_refused(self):
        data = fixture()
        runtime = copy.deepcopy(data)
        runtime["source"].update(kind="runtime", frame_id=1, camera_synchronized=False)
        with self.assertRaisesRegex(ValueError, "offline"):
            catalog.export(json.dumps(runtime).encode())
        for change in ("actor", "mesh", "bsp", "skin", "coordinates"):
            altered = copy.deepcopy(data)
            if change == "actor": altered["mesh_instances"][0]["id"] = "actor:other"
            elif change == "mesh":
                altered["static_mesh_resources"][0]["id"] = "mesh:wrong"
                altered["mesh_instances"][0]["resource_id"] = "mesh:wrong"
            elif change == "bsp": altered["bsp_blocks"][0]["id"] = "bsp:wrong"
            elif change == "skin": altered["mesh_instances"][0]["material_ids"] = [None]
            else: altered["conventions"]["clip_depth"] = "minus_one_to_one"
            with self.subTest(change=change), self.assertRaises(ValueError):
                catalog.export(json.dumps(altered).encode())


if __name__ == "__main__":
    unittest.main()
