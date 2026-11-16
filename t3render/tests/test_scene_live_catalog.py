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
import test_scene_ir


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
