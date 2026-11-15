"""Offline shadow candidates must remain bounded and separate from runtime visibility."""
import copy
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import struct
import sys
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import scene_pbr_offline_casters as candidates
import scene_pbr_packet as pbr
import scene_pbr_shadow_fixture as gpu_fixture
from scene_ir import SceneSnapshot, transform_point
import test_scene_pbr_packet as packet_tests
from test_scene_textured_packet import Resolver

SHADOW = {"size": 128, "near_m": .05, "far_m": 15, "bias_m": .015, "softness": .5}


def fixture():
    scene, light_scene, calibration = packet_tests.inputs()
    data = scene.to_dict()
    data["version"] = 0
    del data["submissions"]
    data["source"].update(kind="offline", frame_id=None, camera_synchronized=None,
                          fingerprints={"map": scene.source.fingerprints["map"]}, limitations=[])
    data["camera"] = None
    bed = data["mesh_instances"][0]
    bed["transform"][12] = 100  # Runtime transform must win for unsubmitted bed sections.
    for name in ("Nearby", "Disabled", "Attached", "Far", "Other"):
        actor = copy.deepcopy(bed)
        actor["id"] = f"actor:test:{name}"
        actor["transform"][12] = 100000 if name == "Far" else 4
        data["mesh_instances"].append(actor)
    block = copy.deepcopy(data["bsp_blocks"][0])
    block.update(id="bsp:extra", source_index=1)
    data["bsp_blocks"].append(block)
    offline = SceneSnapshot.from_dict(data)
    raw = offline.to_json().encode()
    fingerprints = {**scene.source.fingerprints, "offline_scene": hashlib.sha256(raw).hexdigest()}
    source = replace(scene.source, fingerprints=fingerprints)
    scene = replace(scene, source=source)
    light_scene = replace(light_scene, source=source)
    light_id = f"candidate:light:{source.map}:{fingerprints['map']}:lamp"
    light_scene = replace(light_scene, lights=(replace(light_scene.lights[0], id=light_id),))
    calibration["lights"][0]["id"] = light_id
    actors = {name: {"base": "StaticMeshActor", "mesh": "INNbed1A1", "casts_shadows": True,
                     "attached_to": None} for name in ("StaticMeshActor__1520", "Nearby", "Disabled", "Attached", "Far", "Other")}
    actors["Disabled"]["casts_shadows"] = False
    actors["Attached"]["attached_to"] = "lamp"
    actors["Other"]["base"] = "Mover"
    actors["lamp"] = {"attached_to": None}
    return scene, light_scene, calibration, offline, raw, actors


def select(scene, lights, offline, raw, actors, resolve=None):
    selected_lights = [{"position": light.position} for light in lights.lights]
    return candidates.select(scene, offline, raw, actors, resolve or Resolver().resolve, selected_lights,
                             pbr.shadow_parameters(SHADOW), [{"lamp"}], 4, 1)


class OfflineCasterSelectionTests(unittest.TestCase):
    def test_deduplicates_submissions_and_preserves_observed_transform(self):
        scene, lights, _, offline, raw, actors = fixture()
        casters, stats = select(scene, lights, offline, raw, actors)
        bed = [c for c in casters if c["target_id"].endswith("StaticMeshActor__1520")]
        self.assertEqual([c["section"] for c in bed], [0])
        self.assertEqual(bed[0]["source"], "offline_section_runtime_transform")
        geo = scene.static_mesh_resources[0].geometry
        expected = transform_point(geo.positions[geo.indices[0]], scene.mesh_instances[0].transform)[:3]
        self.assertEqual(bed[0]["vertices"][0], expected)
        self.assertEqual(stats["counts"]["already_submitted"], 4)
        self.assertEqual(stats["counts"]["disabled_or_attached_actor"], 2)
        self.assertEqual(stats["counts"]["not_static_mesh_actor"], 1)
        self.assertEqual(stats["counts"]["outside_light_range"], 3)
        self.assertTrue(any(c["target_id"] == "bsp:extra" for c in casters))
        self.assertTrue(any(c["target_id"].endswith("Nearby") for c in casters))
        self.assertTrue(all(c["shadow_mask"] == 1 for c in casters))

    def test_skips_hidden_nonopaque_and_missing_materials(self):
        scene, lights, _, offline, _, actors = fixture()
        data = offline.to_dict()
        data["materials"] += [{"id": "material:hidden", "name": "BF"},
                              {"id": "material:transparent", "name": "glass"}]
        data["bsp_blocks"][1]["geometry"]["sections"][0]["material_id"] = "material:hidden"
        data["bsp_blocks"][1]["geometry"]["sections"][1]["material_id"] = "material:transparent"
        data["mesh_instances"][1]["material_ids"][0] = None
        offline = SceneSnapshot.from_dict(data)
        raw = offline.to_json().encode()
        scene = replace(scene, source=replace(scene.source, fingerprints={**scene.source.fingerprints,
                        "offline_scene": hashlib.sha256(raw).hexdigest()}))
        def resolve(name):
            value = Resolver().resolve(name)
            if name == "glass": value["mips"] = [bytes((1, 2, 3, 0))]
            return value
        casters, stats = select(scene, lights, offline, raw, actors, resolve)
        self.assertFalse(any(c["target_id"] == "bsp:extra" for c in casters))
        self.assertEqual(stats["counts"]["hidden_material"], 1)
        self.assertEqual(stats["counts"]["nonopaque_offline_diffuse"], 1)
        self.assertEqual(stats["counts"]["missing_material"], 1)

    def test_rejects_changed_fixture_asset_identity_and_geometry_limits(self):
        scene, lights, _, offline, raw, actors = fixture()
        with self.assertRaisesRegex(ValueError, "fixture does not match"):
            select(scene, lights, offline, raw + b" ", actors)
        changed = replace(offline, conventions=replace(offline.conventions, meters_per_unit=.5))
        with self.assertRaisesRegex(ValueError, "fixture does not match"):
            select(scene, lights, changed, raw, actors)
        actors["Nearby"]["mesh"] = "other"
        with self.assertRaisesRegex(ValueError, "matching mesh"):
            select(scene, lights, offline, raw, actors)
        actors["Nearby"]["mesh"] = "INNbed1A1"
        with mock.patch.object(candidates, "MAX_TRIANGLES", 1):
            with self.assertRaisesRegex(ValueError, "exceeds bounds"):
                select(scene, lights, offline, raw, actors)
        with mock.patch.object(candidates, "MAX_CASTERS", 1):
            with self.assertRaisesRegex(ValueError, "exceeds bounds"):
                select(scene, lights, offline, raw, actors)

    def test_generated_bed_excludes_the_entire_original_offline_caster(self):
        scene, lights, _, offline, raw, actors = fixture()
        anchor = scene.mesh_instances[0].id
        casters, stats = candidates.select(scene, offline, raw, actors, Resolver().resolve,
                                           [{"position": lights.lights[0].position}], pbr.shadow_parameters(SHADOW),
                                           [{"lamp"}], 4, 1, replaced_instances={anchor})
        self.assertFalse(any(item["target_id"] == anchor for item in casters))
        self.assertEqual(stats["counts"]["replaced_by_generated_mesh"], 3)


class OfflineCasterPacketTests(unittest.TestCase):
    setUpClass = classmethod(packet_tests.PbrPacketTests.setUpClass.__func__)
    tearDownClass = classmethod(packet_tests.PbrPacketTests.tearDownClass.__func__)
    parse = packet_tests.PbrPacketTests.parse

    def test_v3_preserves_v2_draws_and_rejects_bad_casters(self):
        scene, lights, config, offline, raw, actors = fixture()
        args = (scene, lights, json.dumps(config), Resolver(), None)
        v2, _ = pbr.create_packet(*args, shadows=SHADOW, actors=actors)
        v3, stats = pbr.create_packet(*args, shadows=SHADOW, actors=actors, offline=offline,
                                      offline_raw=raw, caster_resolve=Resolver().resolve)
        self.assertEqual(v3[:8], pbr.OFFLINE_CASTER_MAGIC)
        self.assertEqual(self.parse(v3).returncode, 0)
        report = stats["shadow_maps"]["offline_candidates"]
        self.assertGreater(report["selected_sections"], 0)
        payload = sum(8 + section["triangles"] * 36 for section in report["sections"])
        self.assertEqual(v2, pbr.SHADOW_MAGIC + v3[8:216] + v3[220:-payload])
        offset = len(v3) - payload
        for pos, value in ((216, struct.pack("<I", 8193)), (offset, struct.pack("<I", 0)),
                           (offset, struct.pack("<I", 2)), (offset+4, struct.pack("<I", 4)),
                           (offset+8, struct.pack("<I", 0x7fc00000))):
            with self.subTest(pos=pos, value=value):
                self.assertEqual(self.parse(v3[:pos] + value + v3[pos+4:]).returncode, 2)
        self.assertEqual(self.parse(v3[:-1]).returncode, 2)
        self.assertEqual(self.parse(v3+b"x").returncode, 2)
        with mock.patch.object(pbr, "MAX_SHADOW_DRAWS", stats["shadow_maps"]["draw_calls"] - 1):
            with self.assertRaisesRegex(ValueError, "work bounds"):
                pbr.create_packet(*args, shadows=SHADOW, actors=actors, offline=offline,
                                  offline_raw=raw, caster_resolve=Resolver().resolve)
        with self.assertRaisesRegex(ValueError, "require shadows"):
            pbr.create_packet(*args, offline=offline, offline_raw=raw, caster_resolve=Resolver().resolve)

    def test_parser_counts_offline_work_across_lights(self):
        raw = gpu_fixture.fixture(4, two_lights=True, offline_caster=True)
        self.assertEqual(self.parse(raw).returncode, 0)
        header = bytearray(raw[:-80])
        struct.pack_into("<I", header, 216, 6000)
        caster = bytearray(raw[-80:])
        struct.pack_into("<I", caster, 0, 3)
        self.assertEqual(self.parse(bytes(header) + bytes(caster)*6000).returncode, 2)


if __name__ == "__main__":
    unittest.main()
