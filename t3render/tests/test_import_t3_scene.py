"""Offline importer contract; no installed game or graphics runtime required."""
import hashlib
import importlib.util
import math
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("import_t3_scene", ROOT / "t3render" / "tools" / "import_t3_scene.py")
importer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(importer)


class FakeBundle:
    def __init__(self, name):
        self.resource = types.SimpleNamespace(name=name)

    def find(self, name, kind):
        return self.resource if name.lower() == self.resource.name.lower() else None

    def split(self, resource):
        return []


def mesh():
    section = importer_section
    data = types.SimpleNamespace(
        positions=[(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (1.0, 1.0, 0.0)],
        normals=[(0.0, 0.0, 1.0)] * 4,
        uv0=[(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)],
        tangents=[(1.0, 0.0, 0.0)] * 4,
        indices=[0, 1, 2, 1, 3, 2],
        sections=[section("First", 0, 3), section("Second", 3, 3)],
    )
    return types.SimpleNamespace(name="Bed", render=data, skins=[
        types.SimpleNamespace(name="Default", materials=["A", "B"]),
        types.SimpleNamespace(name="Alternate", materials=["C", "D"]),
    ])


def importer_section(name, first, count):
    return types.SimpleNamespace(name=name, first_index=first, index_count=count)


def actor(name, **kwargs):
    return {"name": name, "mesh": "Bed", "skin": "Default", "location": [10.0, 20.0, 30.0],
            "rotation": [0, 0, 0], "draw_scale": 1.0, "attached_to": None, "light": None, **kwargs}


class OfflineImporterTest(unittest.TestCase):
    def make(self):
        bed = mesh()
        block = types.SimpleNamespace(offset=120, block_id=3, mesh=bed.render)
        export = {"level": "TestMap", "actors": [
            actor("one", rotation=[0, 16384, 0], draw_scale=2.0,
                  light={"kind": "omni", "color": [0.3, 0.2, 0.1], "on": False}),
            actor("two", skin="Alternate"),
            actor("missing-skin", skin="Unknown"),
            actor("attached", attached_to="Parent"),
        ]}
        fingerprints = {role: c * 64 for role, c in (("map", "a"), ("bundle", "b"), ("actors", "c"))}
        with patch.object(importer, "parse_static_mesh", return_value=bed):
            doc, counts = importer.make_scene(export, [block], [FakeBundle("Bed")], fingerprints)
        return importer.SceneSnapshot.from_dict(doc).to_dict(), counts

    def test_offline_source_reuses_geometry_and_preserves_per_instance_skins(self):
        doc, counts = self.make()
        self.assertEqual(doc["source"]["kind"], "offline")
        self.assertIsNone(doc["source"]["frame_id"])
        self.assertIsNone(doc["source"]["camera_synchronized"])
        self.assertIsNone(doc["camera"])
        self.assertEqual(len(doc["static_mesh_resources"]), 1)
        self.assertEqual(len(doc["mesh_instances"]), 3)
        self.assertEqual(counts["attached_mesh_actors"], 1)
        self.assertEqual(counts["unresolved_skins"], 1)
        self.assertTrue(any("attached -> Parent" in x for x in doc["source"]["limitations"]))
        by_name = {item["id"].rsplit(":", 1)[-1]: item for item in doc["mesh_instances"]}
        material_names = {item["id"]: item["name"] for item in doc["materials"]}
        self.assertEqual([material_names[x] for x in by_name["one"]["material_ids"]], ["A", "B"])
        self.assertEqual([material_names[x] for x in by_name["two"]["material_ids"]], ["C", "D"])
        self.assertEqual(by_name["missing-skin"]["material_ids"], [None, None])
        self.assertTrue(all(s["material_id"] is None for s in doc["static_mesh_resources"][0]["geometry"]["sections"]))
        self.assertEqual(doc["bsp_blocks"][0]["source_offset"], 120)
        self.assertEqual([material_names[s["material_id"]] for s in doc["bsp_blocks"][0]["geometry"]["sections"]],
                         ["First", "Second"])
        self.assertFalse(doc["lights"][0]["enabled"])
        self.assertIsNone(doc["lights"][0]["radius"])
        self.assertIsNone(doc["lights"][0]["intensity"])

    def test_transform_uses_t3_units_row_vectors_and_rotation(self):
        doc, _ = self.make()
        item = next(x for x in doc["mesh_instances"] if x["id"].endswith(":one"))
        point = importer.SceneSnapshot.from_dict(doc)
        self.assertEqual(point.conventions.up_axis, "z")
        self.assertEqual(point.conventions.meters_per_unit, 0.01905)
        self.assertEqual(point.conventions.winding, "unknown")
        self.assertTrue(any("not runtime calibrated" in x for x in doc["source"]["limitations"]))
        matrix = item["transform"]
        world = [sum(v * matrix[r * 4 + c] for r, v in enumerate((1, 0, 0, 1))) for c in range(3)]
        for actual, expected in zip(world, (10, 22, 30)):
            self.assertTrue(math.isclose(actual, expected, abs_tol=1e-9))

    def test_actor_export_provenance_must_match_the_map(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "TestMap.gmp"
            source.write_bytes(b"map bytes")
            export = {"level": "TestMap", "actors": [actor("one")], "actor_count": 1,
                      "source_size": source.stat().st_size, "source_sha1": hashlib.sha1(source.read_bytes()).hexdigest(),
                      "units": "unreal (Z up)"}
            importer.validate_actor_export(export, source)
            source.write_bytes(b"other map")
            with self.assertRaisesRegex(ValueError, "provenance"):
                importer.validate_actor_export(export, source)

    def test_invalid_transform_is_rejected(self):
        for changes in ({"rotation": [0, 0.5, 0]}, {"location": [0, 0, float("nan")]},
                        {"draw_scale": float("inf")}, {"properties": {"DrawScale3D": [1, 2, 3]}}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                importer.actor_transform(actor("one", **changes))

    def test_resource_names_follow_ibt_lowercase_not_unicode_casefold(self):
        sharp, plain = mesh(), mesh()
        sharp.name, plain.name = "Straße", "Strasse"
        sharp.skins = [types.SimpleNamespace(name="Straße", materials=["Straße", "A"]),
                       types.SimpleNamespace(name="Strasse", materials=["Strasse", "B"])]
        plain.skins = [types.SimpleNamespace(name="Default", materials=["Strasse", "A"])]
        resources = {item.name: item for item in (sharp, plain)}

        class Bundle:
            def find(self, name, kind):
                return next((types.SimpleNamespace(name=key) for key in resources if key.lower() == name.lower()), None)

            def split(self, resource):
                return []

        export = {"level": "TestMap", "actors": [actor("one", mesh="Straße", skin="Straße"),
                                                 actor("two", mesh="Strasse", skin="Default")]}
        fingerprints = {role: c * 64 for role, c in (("map", "a"), ("bundle", "b"), ("actors", "c"))}
        with patch.object(importer, "parse_static_mesh", side_effect=lambda resource, _: resources[resource.name]):
            doc, counts = importer.make_scene(export, [], [Bundle()], fingerprints)
        importer.SceneSnapshot.from_dict(doc)
        self.assertEqual(len(doc["static_mesh_resources"]), 2)
        self.assertEqual({m["name"] for m in doc["materials"]}, {"Straße", "Strasse", "A"})
        self.assertNotIn("unresolved_skins", counts)


if __name__ == "__main__":
    unittest.main()
