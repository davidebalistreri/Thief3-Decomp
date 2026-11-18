"""Offline asset catalogs must not masquerade as runtime frames."""
import copy
import hashlib
import json
import os
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


class TinyTextures:
    def resolve(self, name, kind):
        pixel = (bytes((30, 80, 170, 255)) if kind == "albedo" else
                 bytes((85, 85, 85, 255)) if kind == "height" else bytes((128, 128, 255, 255)))
        return {"width": 2, "height": 2, "mips": [pixel * 4, pixel],
                "source_sha256": hashlib.sha256((name + kind).encode()).hexdigest()}


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


def static_fixture():
    data = fixture()
    source = data['source']
    map_name, map_hash, bundle_hash = source['map'], source['fingerprints']['map'], source['fingerprints']['bundle']
    original = data['static_mesh_resources'][0]
    mesh = copy.deepcopy(original)
    mesh['id'] = f'mesh:{map_name}:{map_hash}:{bundle_hash}:inncrate1'
    mesh['name'] = 'INNcrate1'
    data['static_mesh_resources'].append(mesh)
    for number in (5, 6, 7):
        actor = copy.deepcopy(data['mesh_instances'][0])
        actor['id'] = f'actor:{map_name}:{map_hash}:StaticMeshActor__{number}'
        actor['resource_id'] = mesh['id']
        if number == 7:
            actor['material_ids'] = [None]  # retained as excluded, never guessed
        data['mesh_instances'].append(actor)
    return data


def general_fixture():
    data = static_fixture()
    bed_id = data['static_mesh_resources'][0]['id']
    data['static_mesh_resources'] = [item for item in data['static_mesh_resources'] if item['id'] != bed_id]
    data['mesh_instances'] = [item for item in data['mesh_instances'] if item['resource_id'] != bed_id]
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
                        str(ROOT / "ir/scene_live_window.cpp"), str(ROOT / "src/scene_live_catalog.cpp"),
                        str(ROOT / "src/scene_live_binding.cpp"), str(ROOT / "src/scene_live_driver.cpp"),
                        "-o", str(cls.binary)],
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

    def test_material_library_round_trips_with_and_without_high_poly_bed(self):
        data = fixture();data["conventions"]["meters_per_unit"] = .01905
        scene = scene_ir.SceneSnapshot.from_dict(data)
        high = bed_tests.glb(scene)
        for replacement in (None, high):
            body, manifest = catalog.export(json.dumps(data).encode(), replacement, TinyTextures())
            with self.subTest(has_bed=bool(replacement)):
                self.assertEqual(body[:8], b"T3LCAT03")
                result = self.load(body).split()
                self.assertEqual(result[:10], ["accepted", "SouthQuarter1_int1", "1", "1", "3", "3", "3", "3", "3", "1"])
                self.assertEqual(result[-4:], ["bedroom_materials", "2", "2", "2"])
                self.assertEqual(manifest["source"], "offline")
                self.assertEqual(manifest["bedroom_materials"]["scope"], "viewer_only_when_same_frame_bed_anchor_is_selected")
                self.assertEqual("override" in result, bool(replacement))

        raw = bytearray(body[:-32])
        marker = raw.find(b"remaster:material:stone_wall_v2")
        self.assertGreater(marker, 0)
        texture = marker + len(b"remaster:material:stone_wall_v2")
        def reject(changed):
            self.assertEqual(self.load(bytes(changed) + hashlib.sha256(changed).digest()), "rejected")
        bad = raw.copy();bad[texture + 4 + 64 + 12 + 3] = 0  # albedo alpha
        reject(bad)
        bad = raw.copy();struct.pack_into("<I", bad, texture + 4 + 64, 3)  # non-power-of-two width
        reject(bad)
        bad = raw.copy();bad[texture + 4] = ord("z")  # invalid source fingerprint
        reject(bad)

    def test_material_export_rejects_nonopaque_and_mismatched_mips(self):
        class InvalidTextures(TinyTextures):
            def __init__(self, invalid): self.invalid = invalid
            def resolve(self, name, kind):
                item = super().resolve(name, kind)
                if name == "stone_wall_v2" and kind == "albedo":
                    if self.invalid == "alpha": item["mips"][0] = bytes((0, 0, 0, 0))*4
                    else: item["mips"].pop()
                return item
        data = fixture();data["conventions"]["meters_per_unit"] = .01905
        for invalid in ("alpha", "count"):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                catalog.export(json.dumps(data).encode(), None, InvalidTextures(invalid))
        data["materials"].append({"id": "remaster:material:stone_wall_v2", "name": "collision"})
        with self.assertRaisesRegex(ValueError, "collide"):
            catalog.export(json.dumps(data).encode(), None, TinyTextures())
        data = fixture()
        with self.assertRaisesRegex(ValueError, "unit scale"):
            catalog.export(json.dumps(data).encode(), None, TinyTextures())

    def test_wall_height_round_trips_and_rejects_wrong_dimensions(self):
        data = static_fixture();data["conventions"]["meters_per_unit"] = .01905
        raw = json.dumps(data).encode()
        digest = hashlib.sha256(b"stone_wall_v2height").hexdigest()
        for static in (False, True):
            body, manifest = catalog.export(raw, bedroom_materials=TinyTextures(), static_meshes=static, wall_height=True)
            with self.subTest(static=static):
                self.assertEqual(body[:8], b"T3LCAT06")
                result = self.load(body).split()
                at = result.index("wall_height")
                self.assertEqual(result[at:at+5], ["wall_height", "2", "2", "85", digest])
                self.assertEqual(manifest["bedroom_materials"]["wall_height_sha256"], digest)
                self.assertEqual("static_mesh_candidates" in result, static)
                changed = bytearray(body[:-32]);offset = changed.find(digest.encode())
                self.assertGreater(offset, 0)
                struct.pack_into("<I", changed, offset+64, 4)
                self.assertEqual(self.load(bytes(changed) + hashlib.sha256(changed).digest()), "rejected")
        with self.assertRaisesRegex(ValueError, "requires bedroom materials"):
            catalog.export(raw, wall_height=True)

    def test_structurally_invalid_count_and_magic_are_rejected_even_with_new_digest(self):
        data, _ = catalog.export(json.dumps(fixture()).encode())
        for offset, value in ((0, b"BAD!"), (8, struct.pack("<I", 0xffffffff))):
            body = bytearray(data[:-32]); body[offset:offset+4] = value
            self.assertEqual(self.load(bytes(body) + hashlib.sha256(body).digest()), "rejected")

    def test_static_mesh_candidates_round_trip_and_keep_source_offline(self):
        data = static_fixture();data['conventions']['meters_per_unit'] = .01905
        scene = scene_ir.SceneSnapshot.from_dict(data)
        for replacement, textures in ((None, None), (bed_tests.glb(scene), None),
                                      (None, TinyTextures()), (bed_tests.glb(scene), TinyTextures())):
            raw, manifest = catalog.export(json.dumps(data).encode(), replacement, textures, True)
            with self.subTest(bed=bool(replacement), textures=bool(textures)):
                self.assertEqual(raw[:8], b'T3LCAT04')
                self.assertEqual(self.load(raw).split()[-3:], ['static_mesh_candidates', '3', '2'])
                candidates = manifest['static_mesh_candidates']
                self.assertEqual((candidates['actors'], candidates['extra_resources']), (3, 1))
                self.assertEqual(len(candidates['excluded_actors']), 1)
                self.assertTrue(candidates['excluded_actors'][0].endswith('StaticMeshActor__7'))
                self.assertEqual(candidates['scope'], 'offline_candidates_without_transforms_or_visibility')
                self.assertEqual(manifest['source'], 'offline')

    def test_static_mesh_candidates_reject_bad_identity_and_materials(self):
        data = static_fixture()
        packed, _ = catalog.export(json.dumps(data).encode(), static_meshes=True)
        body = bytearray(packed[:-32])
        def reject(changed):
            self.assertEqual(self.load(bytes(changed) + hashlib.sha256(changed).digest()), 'rejected')
        # Rewrite a second actor ID to duplicate the first without changing length.
        second = body.rfind(b'StaticMeshActor__6')
        self.assertGreater(second, 0)
        bad = body.copy();bad[second + len(b'StaticMeshActor__')] = ord('5');reject(bad)
        # A candidate skin must reference a material in this catalog.
        skin = body.rfind(b'material:wood')
        self.assertGreater(skin, 0)
        bad = body.copy();bad[skin + len(b'material:')] = ord('x');reject(bad)
        # Remove the target's candidate record while leaving the bed header intact.
        target = body.rfind(b'StaticMeshActor__1520')
        self.assertGreater(target, 0)
        bad = body.copy();bad[target + len(b'StaticMeshActor__152')] = ord('9');reject(bad)
        # A same-name mesh from another source makes resource naming ambiguous.
        collision = copy.deepcopy(data['static_mesh_resources'][0])
        collision['id'] = collision['id'].replace('b'*64, 'c'*64)
        data['static_mesh_resources'].append(collision)
        with self.assertRaisesRegex(ValueError, 'target bed'):
            catalog.export(json.dumps(data).encode(), static_meshes=True)

    def test_general_map_round_trip_without_bed_and_with_number_1520(self):
        data = general_fixture()
        data['mesh_instances'][0]['id'] = data['mesh_instances'][0]['id'].replace('__5', '__1520')
        raw, manifest = catalog.export(json.dumps(data).encode(), general_map=True)
        self.assertEqual(raw[:8], b'T3LCAT05')
        self.assertEqual(manifest['source'], 'offline')
        self.assertIsNone(manifest['bed_actor_id'])
        self.assertEqual(manifest['static_mesh_candidates']['actors'], 2)
        self.assertEqual(manifest['static_mesh_candidates']['extra_resources'], 1)
        self.assertEqual(len(manifest['static_mesh_candidates']['excluded_actors']), 1)
        result = self.load(raw).split()
        self.assertEqual(result[:10], ['accepted', 'SouthQuarter1_int1', '1', '1', '0', '0', '0', '0', '0', '0'])
        self.assertEqual(result[-4:], ['static_mesh_candidates', '2', '1', 'bedless_driver_ok'])

    def test_general_map_cli_writes_offline_catalog_and_manifest(self):
        with tempfile.TemporaryDirectory(prefix='t3-general-catalog-') as directory:
            root = Path(directory);source = root / 'offline.json';output = root / 'catalog.bin'
            source.write_text(json.dumps(general_fixture()))
            subprocess.run([sys.executable, '-B', str(ROOT / 'tools/export_live_catalog.py'), str(source),
                            '--general-map', '--output', str(output)], check=True, capture_output=True, timeout=30)
            self.assertEqual(output.read_bytes()[:8], b'T3LCAT05')
            self.assertEqual(json.loads(Path(str(output)+'.manifest.json').read_text())['source'], 'offline')
            self.assertEqual(self.load(output.read_bytes()).split()[-1], 'bedless_driver_ok')

    def test_general_map_allows_bsp_only_without_bundle_fingerprints(self):
        data = general_fixture();data['mesh_instances'] = [];data['static_mesh_resources'] = []
        data['source']['fingerprints'].pop('bundle', None);data['source']['fingerprints'].pop('kernel', None)
        raw, manifest = catalog.export(json.dumps(data).encode(), general_map=True)
        self.assertEqual(manifest['static_mesh_candidates']['actors'], 0)
        self.assertEqual(manifest['static_mesh_candidates']['extra_resources'], 0)
        self.assertEqual(self.load(raw).split()[-1], 'bedless_driver_ok')
        with self.assertRaisesRegex(ValueError, 'bedroom-specific'):
            catalog.export(json.dumps(data).encode(), bed_high=b'not a GLB', general_map=True)
        with self.assertRaisesRegex(ValueError, 'bedroom-specific'):
            catalog.export(json.dumps(data).encode(), bedroom_materials=TinyTextures(), general_map=True)

    def test_general_map_allows_mesh_only_but_rejects_empty_geometry(self):
        data = general_fixture();data['bsp_blocks'] = []
        raw, manifest = catalog.export(json.dumps(data).encode(), general_map=True)
        self.assertEqual(manifest['bsp_blocks'], 0)
        self.assertEqual(manifest['static_mesh_candidates']['actors'], 2)
        self.assertEqual(self.load(raw).split()[-4:], ['static_mesh_candidates', '2', '1', 'bedless_driver_ok'])
        data['mesh_instances'] = [];data['static_mesh_resources'] = []
        with self.assertRaisesRegex(ValueError, 'BSP or an eligible'):
            catalog.export(json.dumps(data).encode(), general_map=True)
        empty = (b'T3LCAT05' + catalog._text('empty') + catalog._text('a'*64) + catalog._text('b'*64) +
                 struct.pack('<5I', 0, 0, 0, 0, 0) + struct.pack('<2I', 1, 0) +
                 catalog._text('mat:one') + catalog._text('one') + struct.pack('<3I', 0, 0, 0))
        self.assertEqual(self.load(empty + hashlib.sha256(empty).digest()), 'rejected')

    def test_general_map_rejects_corrupt_identity_fingerprint_and_unused_resources(self):
        raw, _ = catalog.export(json.dumps(general_fixture()).encode(), general_map=True)
        body = bytearray(raw[:-32])
        def reject(changed):
            self.assertEqual(self.load(bytes(changed) + hashlib.sha256(changed).digest()), 'rejected')
        actor = body.find(b'StaticMeshActor__6');self.assertGreater(actor, 0)
        bad = body.copy();bad[actor + len(b'StaticMeshActor__')] = ord('5');reject(bad)
        fingerprint = body.find(b'b'*64);self.assertGreater(fingerprint, 0)
        bad = body.copy();bad[fingerprint] = ord('c');reject(bad)
        skin = body.rfind(b'material:wood');self.assertGreater(skin, 0)
        bad = body.copy();bad[skin + len(b'material:')] = ord('x');reject(bad)
        first_actor = body.find(b'actor:SouthQuarter1_int1:');self.assertGreater(first_actor, 8)
        count_offset = first_actor-8
        self.assertEqual(struct.unpack_from('<I', body, count_offset)[0], 2)
        bad = body[:count_offset+4];struct.pack_into('<I', bad, count_offset, 0);reject(bad)

    @unittest.skipUnless(os.environ.get('T3RENDER_OFFLINE_SCENE'), 'local offline SceneIR not supplied')
    def test_general_catalog_from_offline_bedroom_subset(self):
        data = json.loads(Path(os.environ['T3RENDER_OFFLINE_SCENE']).read_text())
        data['mesh_instances'] = [item for item in data['mesh_instances'] if not item['id'].endswith(':StaticMeshActor__1520')]
        body, manifest = catalog.export(json.dumps(data).encode(), general_map=True)
        self.assertEqual(manifest['source'], 'offline')
        self.assertEqual(manifest['bsp_blocks'], 45)
        self.assertGreater(manifest['static_mesh_candidates']['actors'], 0)
        self.assertIsNone(manifest['bed_actor_id'])
        self.assertEqual(self.load(body).split()[-1], 'bedless_driver_ok')

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
