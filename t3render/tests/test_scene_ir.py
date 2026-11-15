import copy
import json
import math
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ir"))
import scene_ir


IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def example():
    geometry = {"positions": [[0, 0, 0], [1, 0, 0], [0, 1, 0]],
                "normals": [[0, 0, 1]] * 3, "uv0": [[0, 0], [1, 0], [0, 1]],
                "tangents": [[1, 0, 0]] * 3, "indices": [0, 1, 2],
                "sections": [{"name": "Default", "first_index": 0, "index_count": 3, "material_id": None}]}
    world = IDENTITY[:]
    world[12:15] = [3398, 1514, 272]
    return {"schema": "t3.scene", "version": 0,
            "source": {"kind": "offline", "map": "SouthQuarter1_int1", "frame_id": None,
                       "camera_synchronized": None, "fingerprints": {"map": "a" * 64},
                       "limitations": ["offline fixture"]},
            "conventions": {"length_unit": "t3_unit", "meters_per_unit": None, "axes": "t3_xyz",
                            "up_axis": "z", "handedness": "left", "winding": "unknown",
                            "uv_origin": "unknown", "tangent_space": "unknown",
                            "matrix_layout": "row_major", "matrix_vectors": "row", "clip_depth": "unknown"},
            "camera": None, "static_mesh_resources": [{"id": "asset:bed", "name": "INNbed1A1", "geometry": geometry}],
            "mesh_instances": [{"id": "actor:bed", "resource_id": "asset:bed", "transform": world,
                                "skin": "SQ1int", "material_ids": ["material:wood"]}],
            "bsp_blocks": [{"id": "bsp:0", "block_id": 7, "source_index": 0, "source_offset": 128,
                            "geometry": copy.deepcopy(geometry)}],
            "materials": [{"id": "material:wood", "name": "wood"}],
            "lights": [{"id": "light:0", "kind": None, "position": [1, 2, 3], "color": None,
                        "radius": None, "intensity": None, "enabled": None}]}


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def subtract(a, b):
    return tuple(x - y for x, y in zip(a, b))


def face_normal(points, indices):
    a, b, c = (points[i] for i in indices)
    return cross(subtract(b, a), subtract(c, a))


class SceneIrTests(unittest.TestCase):
    def test_round_trip_preserves_offline_provenance_and_semantics(self):
        data = example()
        snapshot = scene_ir.SceneSnapshot.from_dict(data)
        self.assertIsInstance(snapshot.static_mesh_resources[0], scene_ir.StaticMeshResource)
        self.assertIsInstance(snapshot.mesh_instances[0], scene_ir.MeshInstance)
        self.assertIsInstance(snapshot.bsp_blocks[0], scene_ir.BspBlock)
        self.assertIsInstance(snapshot.materials[0], scene_ir.MaterialRef)
        self.assertIsInstance(snapshot.lights[0], scene_ir.VisualLight)
        self.assertEqual(scene_ir.SceneSnapshot.from_json(snapshot.to_json()).to_dict(), data)
        self.assertEqual(snapshot.source.kind, "offline")
        self.assertIsNone(snapshot.source.frame_id)
        self.assertIsNone(snapshot.camera)
        self.assertIsNone(snapshot.lights[0].enabled)

    def test_unit_scale_must_be_explicit_and_does_not_change_source_coordinates(self):
        data = example()
        snapshot = scene_ir.SceneSnapshot.from_dict(data)
        with self.assertRaisesRegex(ValueError, "unit scale is unknown"):
            scene_ir.distance_in_meters(16, snapshot.conventions)
        data["conventions"]["meters_per_unit"] = 0.01905  # documented asset estimate, not runtime proof
        snapshot = scene_ir.SceneSnapshot.from_dict(data)
        self.assertAlmostEqual(scene_ir.distance_in_meters(16, snapshot.conventions), 0.3048)
        self.assertEqual(snapshot.mesh_instances[0].transform[12:15], (3398, 1514, 272))
        data["conventions"]["meters_per_unit"] = 0
        with self.assertRaises(ValueError):
            scene_ir.SceneSnapshot.from_dict(data)

    def test_axis_swap_flips_handedness_and_winding_restores_normal_alignment(self):
        data = example()
        source = data["static_mesh_resources"][0]["geometry"]
        original = face_normal(source["positions"], source["indices"])
        self.assertEqual(original, (0, 0, 1))
        swapped = [scene_ir.t3_to_y_up(point) for point in source["positions"]]
        swapped_normal = scene_ir.t3_to_y_up(original)
        self.assertEqual(face_normal(swapped, source["indices"]), tuple(-v for v in swapped_normal))
        dot = lambda a, b: sum(x * y for x, y in zip(a, b))
        self.assertLess(dot(face_normal(swapped, source["indices"]), swapped_normal), 0)
        reversed_indices = scene_ir.reverse_triangles(source["indices"])
        self.assertEqual(face_normal(swapped, reversed_indices), swapped_normal)
        self.assertGreater(dot(face_normal(swapped, reversed_indices), swapped_normal), 0)
        self.assertEqual(scene_ir.SceneSnapshot.from_dict(data).conventions.winding, "unknown")
        data["conventions"]["winding"] = "normal_aligned"
        self.assertEqual(scene_ir.SceneSnapshot.from_dict(data).conventions.winding, "normal_aligned")
        data["conventions"]["winding"] = "ccw"
        with self.assertRaises(ValueError):
            scene_ir.SceneSnapshot.from_dict(data)
        with self.assertRaises(ValueError):
            scene_ir.reverse_triangles([0, 1])

    def test_matrix_is_row_major_with_row_vectors_and_no_transpose(self):
        matrix = IDENTITY[:]
        matrix[12:15] = [7, 8, 9]
        self.assertEqual(scene_ir.transform_point([1, 2, 3], matrix), (8, 10, 12, 1))
        matrix[0], matrix[1], matrix[4], matrix[5] = 0, 1, -1, 0
        self.assertEqual(scene_ir.transform_point([1, 2, 3], matrix), (5, 9, 12, 1))
        data = example()
        data["conventions"]["matrix_vectors"] = "column"
        with self.assertRaises(ValueError):
            scene_ir.SceneSnapshot.from_dict(data)

    def test_uv_and_tangents_remain_unmodified_without_assuming_tangent_handedness(self):
        data = example()
        geometry = data["static_mesh_resources"][0]["geometry"]
        geometry["uv0"][0] = [0.25, 0.75]
        geometry["tangents"][0] = [0, 1, 0]
        snapshot = scene_ir.SceneSnapshot.from_json(scene_ir.SceneSnapshot.from_dict(data).to_json())
        self.assertEqual(snapshot.conventions.uv_origin, "unknown")
        self.assertEqual(snapshot.conventions.tangent_space, "unknown")
        self.assertEqual(snapshot.static_mesh_resources[0].geometry.uv0[0], (0.25, 0.75))
        self.assertEqual(snapshot.static_mesh_resources[0].geometry.tangents[0], (0, 1, 0))
        data["conventions"]["tangent_space"] = "left_handed"
        with self.assertRaises(ValueError):
            scene_ir.SceneSnapshot.from_dict(data)

    def test_clip_depth_is_declared_not_guessed_or_converted(self):
        near, far = 1, 10
        projection = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, far / (far - near), 1,
                      0, 0, -near * far / (far - near), 0]
        for z, expected in ((near, 0), (far, 1)):
            _, _, clip_z, clip_w = scene_ir.transform_point([0, 0, z], projection)
            self.assertAlmostEqual(clip_z / clip_w, expected)
        symmetric = projection[:]
        symmetric[10] = (far + near) / (far - near)
        symmetric[14] = -2 * near * far / (far - near)
        for z, expected in ((near, -1), (far, 1)):
            _, _, clip_z, clip_w = scene_ir.transform_point([0, 0, z], symmetric)
            self.assertAlmostEqual(clip_z / clip_w, expected)
        data = example()
        data["source"].update(kind="runtime", frame_id=42, camera_synchronized=True)
        data["source"]["fingerprints"]["trace"] = "b" * 64
        data["conventions"]["clip_depth"] = "zero_to_one"
        data["camera"] = {"source": {"kind": "runtime", "frame_id": 42, "fingerprint": "b" * 64},
                          "view": IDENTITY, "projection": projection,
                          "viewport": {"x": 0, "y": 0, "width": 1024, "height": 768,
                                       "min_depth": 0, "max_depth": 0.5}, "near": near, "far": far}
        result = scene_ir.SceneSnapshot.from_json(scene_ir.SceneSnapshot.from_dict(data).to_json())
        self.assertIsInstance(result.camera, scene_ir.Camera)
        self.assertEqual(result.camera.projection, tuple(projection))
        self.assertEqual(result.camera.viewport.max_depth, 0.5)
        self.assertEqual(result.conventions.clip_depth, "zero_to_one")

    def test_runtime_camera_on_offline_scene_retains_independent_provenance(self):
        data = example()
        data["camera"] = {"source": {"kind": "runtime", "frame_id": 2661, "fingerprint": "c" * 64},
                          "view": IDENTITY, "projection": IDENTITY, "viewport": None, "near": None, "far": None}
        result = scene_ir.SceneSnapshot.from_json(scene_ir.SceneSnapshot.from_dict(data).to_json())
        self.assertEqual(result.source.kind, "offline")
        self.assertIsNone(result.source.frame_id)
        self.assertIsNone(result.source.camera_synchronized)
        self.assertEqual(result.camera.source.frame_id, 2661)
        self.assertEqual(result.camera.source.fingerprint, "c" * 64)
        data["source"]["camera_synchronized"] = True
        with self.assertRaises(ValueError):
            scene_ir.SceneSnapshot.from_dict(data)
        data["source"].update(kind="runtime", frame_id=2662)
        with self.assertRaises(ValueError):
            scene_ir.SceneSnapshot.from_dict(data)
        data["source"].update(frame_id=2661)
        with self.assertRaises(ValueError):
            scene_ir.SceneSnapshot.from_dict(data)
        data["source"].update(frame_id=2661, fingerprints={"trace": "d" * 64})
        with self.assertRaises(ValueError):
            scene_ir.SceneSnapshot.from_dict(data)
        data["source"]["fingerprints"]["trace"] = "c" * 64
        self.assertTrue(scene_ir.SceneSnapshot.from_dict(data).source.camera_synchronized)
        data["camera"]["source"]["fingerprint"] = None
        with self.assertRaises(ValueError):
            scene_ir.SceneSnapshot.from_dict(data)

    def test_unknown_fields_pointers_d3d_types_and_nonfinite_values_are_rejected(self):
        for path, value in (("actor_ptr", 123), ("uobject", "0x12345678"), ("d3d_device", "IDirect3DDevice8")):
            data = example()
            data["mesh_instances"][0][path] = value
            with self.subTest(path=path), self.assertRaises(ValueError):
                scene_ir.SceneSnapshot.from_dict(data)
        data = example()
        data["mesh_instances"][0]["id"] = "0x12345678"
        with self.assertRaises(ValueError):
            scene_ir.SceneSnapshot.from_dict(data)
        data = example()
        data["static_mesh_resources"][0]["geometry"]["positions"][0][0] = math.inf
        with self.assertRaises(ValueError):
            scene_ir.SceneSnapshot.from_dict(data)
        with self.assertRaises(ValueError):
            scene_ir.SceneSnapshot.from_json('{"schema":"t3.scene", "schema":"t3.scene"}')

    def test_invalid_indices_sections_and_references_are_rejected(self):
        for change in ("bad_index", "bad_section", "missing_mesh", "missing_material", "bad_attribute"):
            data = example()
            geo = data["static_mesh_resources"][0]["geometry"]
            if change == "bad_index": geo["indices"][2] = 4
            if change == "bad_section": geo["sections"][0]["index_count"] = 6
            if change == "missing_mesh": data["mesh_instances"][0]["resource_id"] = "missing"
            if change == "missing_material": data["mesh_instances"][0]["material_ids"] = ["missing"]
            if change == "bad_attribute": geo["uv0"].pop()
            with self.subTest(change=change), self.assertRaises(ValueError):
                scene_ir.SceneSnapshot.from_dict(data)

    def test_sections_can_start_at_an_arbitrary_index_buffer_offset(self):
        data = example()
        geometry = data["static_mesh_resources"][0]["geometry"]
        geometry["indices"] = [65535, 0, 1, 2]
        geometry["sections"][0]["first_index"] = 1
        snapshot = scene_ir.SceneSnapshot.from_dict(data)
        self.assertEqual(snapshot.static_mesh_resources[0].geometry.indices, (65535, 0, 1, 2))

    def test_offline_is_never_a_runtime_frame(self):
        data = example()
        data["source"]["frame_id"] = 2661
        with self.assertRaises(ValueError):
            scene_ir.SceneSnapshot.from_dict(data)
        data["source"].update(kind="runtime", frame_id=None)
        with self.assertRaises(ValueError):
            scene_ir.SceneSnapshot.from_dict(data)
        data["source"].update(frame_id=2661, camera_synchronized=True)
        with self.assertRaises(ValueError):
            scene_ir.SceneSnapshot.from_dict(data)


if __name__ == "__main__":
    unittest.main()
