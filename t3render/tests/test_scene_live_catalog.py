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
from unittest import mock

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools"))
import export_live_catalog as catalog
import scene_ir
import test_scene_ir
import test_scene_pbr_bed as bed_tests
import ibt


class TinyTextures:
    def resolve(self, name, kind):
        pixel = (bytes((30, 80, 170, 255)) if kind == "albedo" else
                 bytes((85, 85, 85, 255)) if kind == "height" else bytes((128, 128, 255, 255)))
        return {"width": 2, "height": 2, "mips": [pixel * 4, pixel],
                "source_sha256": hashlib.sha256((name + kind).encode()).hexdigest()}


class TinyOriginals:
    def resolve(self, name):
        pixel = bytes((40, 90, 140, 255))
        return {"name": name.lower(), "bundle": "fixture.ibt", "width": 2, "height": 1,
                "mips": [pixel * 2, pixel]}


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

    def test_original_texture_export_can_read_verified_backup(self):
        with tempfile.TemporaryDirectory(prefix='t3render-original-bundle-') as temp:
            root = Path(temp);maps = root/'game/Content/T3/Maps';maps.mkdir(parents=True)
            installed = maps/'SouthQuarter1_int1.ibt';installed.write_bytes(b'installed')
            kernel = maps/'Kernel_GFXALL.ibt';kernel.write_bytes(b'kernel')
            backup = root/'SouthQuarter1_int1.ibt';backup.write_bytes(b'original backup')
            data = fixture();data['source']['fingerprints']['bundle'] = hashlib.sha256(backup.read_bytes()).hexdigest()
            data['source']['fingerprints']['kernel'] = hashlib.sha256(kernel.read_bytes()).hexdigest()
            scene = root/'scene.json';scene.write_text(json.dumps(data))
            resolver = object()
            with mock.patch.object(ibt, 'IBT', side_effect=lambda path: Path(path)) as open_bundle, \
                 mock.patch.object(catalog.packet, 'TextureResolver', return_value=resolver), \
                 mock.patch.object(catalog, 'export', return_value=(b'catalog', {'bsp_blocks': 1})) as export, \
                 mock.patch.object(catalog.packet, 'write_pair'):
                catalog.main([str(scene), '--output', str(root/'catalog.bin'),
                              '--bedroom-originals-game-dir', str(root/'game'), '--bedroom-originals-bundle', str(backup)])
            self.assertEqual([call.args[0] for call in open_bundle.call_args_list], [backup, kernel])
            self.assertIs(export.call_args.args[8], resolver)
            self.assertEqual(installed.read_bytes(), b'installed')

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

    def test_bedroom_crate_is_explicit_and_old_catalogs_reject_it(self):
        data = static_fixture()
        source = data['source']
        map_name, map_hash, bundle_hash = source['map'], source['fingerprints']['map'], source['fingerprints']['bundle']
        mesh = copy.deepcopy(data['static_mesh_resources'][0])
        mesh['id'] = f'mesh:{map_name}:{map_hash}:{bundle_hash}:genxgoodscrateb'
        mesh['name'] = 'GENxGOODScrateB'
        data['static_mesh_resources'].append(mesh)
        actor = copy.deepcopy(data['mesh_instances'][0])
        actor.update(id=f'actor:{map_name}:{map_hash}:D_1224__21', resource_id=mesh['id'])
        data['mesh_instances'].append(actor)
        raw, manifest = catalog.export(json.dumps(data).encode(), static_meshes=True, bedroom_props=True)
        self.assertEqual(raw[:8], b'T3LCAT07')
        self.assertEqual(manifest['version'], 7)
        self.assertEqual(self.load(raw).split()[-3:], ['static_mesh_candidates', '4', '3'])
        def reject(body):
            self.assertEqual(self.load(bytes(body) + hashlib.sha256(body).digest()), 'rejected')
        old = bytearray(raw[:-32]);old[:8] = b'T3LCAT04';reject(old)
        wrong = bytearray(raw[:-32]);where = wrong.rfind(b'D_1224__21')
        self.assertGreater(where, 0);wrong[where + len(b'D_1224__2')] = ord('2');reject(wrong)
        cabinet_mesh = copy.deepcopy(mesh)
        cabinet_mesh['id'] = f'mesh:{map_name}:{map_hash}:{bundle_hash}:seainfurncabinetc1'
        cabinet_mesh['name'] = 'SEAinFURNcabinetC1'
        data['static_mesh_resources'].append(cabinet_mesh)
        cabinet_actor = copy.deepcopy(actor)
        cabinet_actor.update(id=f'actor:{map_name}:{map_hash}:D_443__1', resource_id=cabinet_mesh['id'])
        data['mesh_instances'].append(cabinet_actor)
        body, _ = catalog.export(json.dumps(data).encode(), static_meshes=True, bedroom_props=True)
        self.assertEqual(self.load(body).split()[-3:], ['static_mesh_candidates', '5', '4'])
        wrong = bytearray(body[:-32]);where = wrong.rfind(b'D_443__1')
        self.assertGreater(where, 0);wrong[where + len(b'D_443__')] = ord('2');reject(wrong)
        for suffix, name in (('D_125__3', 'GENiFOODbreadA1'), ('D_548__0', 'GENxGOODSarchtarg')):
            prop_mesh = copy.deepcopy(mesh)
            prop_mesh['id'] = f'mesh:{map_name}:{map_hash}:{bundle_hash}:{name.lower()}'
            prop_mesh['name'] = name
            data['static_mesh_resources'].append(prop_mesh)
            prop_actor = copy.deepcopy(actor)
            prop_actor.update(id=f'actor:{map_name}:{map_hash}:{suffix}', resource_id=prop_mesh['id'])
            data['mesh_instances'].append(prop_actor)
        body, _ = catalog.export(json.dumps(data).encode(), static_meshes=True, bedroom_props=True)
        self.assertEqual(self.load(body).split()[-3:], ['static_mesh_candidates', '7', '6'])
        for suffix in (b'D_125__3', b'D_548__0'):
            wrong = bytearray(body[:-32]);where = wrong.rfind(suffix)
            self.assertGreater(where, 0);wrong[where + len(suffix)-1] = ord('9');reject(wrong)
        data['mesh_instances'].pop();data['mesh_instances'].pop()
        data['mesh_instances'].pop()
        with self.assertRaisesRegex(ValueError, 'require static meshes'):
            catalog.export(json.dumps(data).encode(), bedroom_props=True)
        data['mesh_instances'].pop()
        with self.assertRaisesRegex(ValueError, 'bedroom crate'):
            catalog.export(json.dumps(data).encode(), static_meshes=True, bedroom_props=True)

    def test_door_crates_are_bounded_candidates(self):
        data = static_fixture()
        source = data['source']
        map_name, map_hash, bundle_hash = source['map'], source['fingerprints']['map'], source['fingerprints']['bundle']
        mesh = copy.deepcopy(data['static_mesh_resources'][0])
        mesh['id'] = f'mesh:{map_name}:{map_hash}:{bundle_hash}:genxgoodscrateb'
        mesh['name'] = 'GENxGOODScrateB'
        data['static_mesh_resources'].append(mesh)
        for suffix in (0, 21, 22, 23, 24):
            actor = copy.deepcopy(data['mesh_instances'][0])
            actor.update(id=f'actor:{map_name}:{map_hash}:D_1224__{suffix}', resource_id=mesh['id'])
            data['mesh_instances'].append(actor)
        raw, manifest = catalog.export(json.dumps(data).encode(), static_meshes=True, bedroom_props=True)
        self.assertEqual(self.load(raw).split()[-3:], ['static_mesh_candidates', '7', '3'])
        self.assertNotIn(b'D_1224__24', raw)
        wrong = bytearray(raw[:-32]);where = wrong.rfind(b'D_1224__23')
        self.assertGreater(where, 0);wrong[where + len(b'D_1224__2')] = ord('4')
        self.assertEqual(self.load(bytes(wrong) + hashlib.sha256(wrong).digest()), 'rejected')

    def test_bed_textiles_are_optional_and_bound_to_the_generated_bed(self):
        data = static_fixture();data['conventions']['meters_per_unit'] = .01905
        source = data['source'];map_name = source['map'];map_hash = source['fingerprints']['map']
        mesh = copy.deepcopy(data['static_mesh_resources'][0])
        mesh['id'] = f"mesh:{map_name}:{map_hash}:{source['fingerprints']['bundle']}:genxgoodscrateb"
        mesh['name'] = 'GENxGOODScrateB';data['static_mesh_resources'].append(mesh)
        actor = copy.deepcopy(data['mesh_instances'][0])
        actor.update(id=f'actor:{map_name}:{map_hash}:D_1224__21', resource_id=mesh['id'])
        data['mesh_instances'].append(actor)
        raw = json.dumps(data).encode();high = bed_tests.glb(scene_ir.SceneSnapshot.from_dict(data))
        packed, manifest = catalog.export(raw, high, TinyTextures(), True, bedroom_props=True, bed_textiles=True)
        self.assertEqual((packed[:8], manifest['version']), (b'T3LCAT08', 8))
        self.assertIn('bed_textiles 2', self.load(packed))
        self.assertEqual(set(manifest['bedroom_materials']['sources_sha256']),
                         {'stone_wall_v2', 'wood_floor_v2', 'bed_wool_v2', 'pillow_linen'})
        for kwargs in ({'bedroom_props': True}, {'bedroom_props': True, 'bedroom_materials': TinyTextures()},
                       {'bed_high': high, 'bedroom_materials': TinyTextures()}):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(ValueError, 'bed textiles require'):
                catalog.export(raw, static_meshes=True, bed_textiles=True, **kwargs)
        def reject(body):
            self.assertEqual(self.load(bytes(body) + hashlib.sha256(body).digest()), 'rejected')
        bad = bytearray(packed[:-32]);bad[:8] = b'T3LCAT07';reject(bad)
        bad = bytearray(packed[:-32]);marker = bad.rfind(b'remaster:material:bed_high:textured:PillowLinen')
        self.assertGreater(marker, 0);bad[marker + len(b'remaster:material:bed_high:textured:')] = ord('X');reject(bad)

    def test_bedroom_originals_round_trip_and_reject_invalid_assets(self):
        data = static_fixture();data['conventions']['meters_per_unit'] = .01905
        source = data['source'];map_name = source['map'];map_hash = source['fingerprints']['map']
        mesh = copy.deepcopy(data['static_mesh_resources'][0])
        mesh['id'] = f"mesh:{map_name}:{map_hash}:{source['fingerprints']['bundle']}:genxgoodscrateb"
        mesh['name'] = 'GENxGOODScrateB';data['static_mesh_resources'].append(mesh)
        actor = copy.deepcopy(data['mesh_instances'][0])
        actor.update(id=f'actor:{map_name}:{map_hash}:D_1224__21', resource_id=mesh['id'])
        data['mesh_instances'].append(actor)
        for index, name in enumerate(catalog.BEDROOM_ORIGINAL_NAMES, start=10):
            identifier = f'material:original:{name}'
            data['materials'].append({'id': identifier, 'name': name})
            block = copy.deepcopy(data['bsp_blocks'][0])
            block['source_index'] = index;block['source_offset'] = index * 10
            block['id'] = f'bsp:{map_name}:{map_hash}:{index}:{index*10}'
            block['geometry']['sections'][0]['material_id'] = identifier
            data['bsp_blocks'].append(block)
        raw = json.dumps(data).encode();high = bed_tests.glb(scene_ir.SceneSnapshot.from_dict(data))
        packed, manifest = catalog.export(raw, high, TinyTextures(), True, bedroom_props=True,
                                          bed_textiles=True, bedroom_originals=TinyOriginals())
        self.assertEqual((packed[:8], manifest['version']), (b'T3LCAT09', 9))
        self.assertTrue(self.load(packed).startswith('accepted '))
        self.assertEqual(set(manifest['bedroom_original_albedo']), set(catalog.BEDROOM_ORIGINAL_NAMES))
        packed10, manifest10 = catalog.export(raw, high, TinyTextures(), True, bedroom_props=True,
                                              bed_textiles=True, bedroom_originals=TinyOriginals(), candlestick_metal=True)
        self.assertEqual((packed10[:8], manifest10['version']), (b'T3LCAT10', 10))
        self.assertTrue(self.load(packed10).startswith('accepted '))
        self.assertEqual(manifest10['candlestick_metal_albedo_sha256'],
                         hashlib.sha256(b'candlestick_metal_v2albedo').hexdigest())
        self.assertEqual(manifest10['candlestick_wax_albedo']['appearance'], 'original_luminance_tinted_opaque_preview')
        readable, readable_manifest = catalog.export(raw, high, TinyTextures(), True, bedroom_props=True,
            bed_textiles=True, bedroom_originals=TinyOriginals(), candlestick_metal=True, readable_bed_wool=True)
        self.assertTrue(self.load(readable).startswith('accepted '))
        self.assertNotEqual(readable, packed10)
        self.assertEqual(readable_manifest['bedroom_materials']['bed_wool_albedo_linear_gain'], 5)
        with self.assertRaisesRegex(ValueError, 'candlestick metal requires'):
            catalog.export(raw, high, TinyTextures(), True, bedroom_props=True, bed_textiles=True, candlestick_metal=True)
        with self.assertRaisesRegex(ValueError, 'readable bed wool requires'):
            catalog.export(raw, readable_bed_wool=True)
        def reject(body):
            self.assertEqual(self.load(bytes(body) + hashlib.sha256(body).digest()), 'rejected')
        bad = bytearray(packed[:-32]);bad[:8] = b'T3LCAT08';reject(bad)
        bad = bytearray(packed10[:-32]);bad[:8] = b'T3LCAT09';reject(bad)
        bad = bytearray(packed10[:-32]);marker = bad.find(b'remaster:material:candlestick_metal_v2')
        self.assertGreater(marker, 0);bad[marker+len(b'remaster:material:')] = ord('X');reject(bad)
        bad = bytearray(packed[:-32]);marker = bad.rfind(b'material:original:GENtarget_D')
        self.assertGreater(marker, 0)
        bad[marker + len(b'material:original:')] = ord('X');reject(bad)
        bad = bytearray(packed[:-32]);marker = bad.rfind(b'material:original:GENtarget_D')
        pixel = marker + len(b'material:original:GENtarget_D') + 4 + 64 + 12
        bad[pixel+3] = 0;reject(bad)
        class Transparent(TinyOriginals):
            def resolve(self, name):
                value = super().resolve(name);value['mips'][0] = b'\0' * 8;return value
        with self.assertRaisesRegex(ValueError, 'nonopaque'):
            catalog.export(raw, high, TinyTextures(), True, bedroom_props=True,
                           bed_textiles=True, bedroom_originals=Transparent())

    def test_candle_wax_preserves_surface_variation_with_opaque_warm_tint(self):
        class Wax:
            def resolve(self, name):
                if name != 'GENcandlewax_ON':
                    raise ValueError('unexpected material')
                return {'name': 'gencandlewax_d', 'bundle': 'fixture.ibt', 'width': 2, 'height': 1,
                        'mips': [bytes((80, 100, 70, 0, 160, 180, 140, 200)), bytes((120, 140, 100, 100))]}
        data, info = catalog._candlestick_wax(Wax())
        first, second, mip1 = data[-12:-8], data[-8:-4], data[-4:]
        self.assertEqual((first[3], second[3], mip1[3]), (255, 255, 255))
        self.assertTrue(first[0] >= first[1] > first[2] and second[0] >= second[1] > second[2])
        self.assertTrue(all(second[i] > first[i] for i in range(3)))
        self.assertEqual(info['appearance'], 'original_luminance_tinted_opaque_preview')

    def test_wool_preview_lifts_dark_rgb_without_changing_source_or_alpha(self):
        source = bytes((20, 30, 50, 255, 0, 0, 0, 255))
        result = catalog._readable_wool_mips([source])[0]
        self.assertTrue(all(result[i] > source[i] for i in range(3)))
        self.assertEqual(result[3:], source[3:])
        self.assertEqual(source, bytes((20, 30, 50, 255, 0, 0, 0, 255)))

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
