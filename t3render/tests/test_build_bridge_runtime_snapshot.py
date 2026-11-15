import copy
from pathlib import Path
import struct
import sys
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import build_bridge_runtime_snapshot as builder
import compare_bridge_fixture as comparison
import test_analyze_scene_bridge as bridge_test
import test_compare_bridge_fixture as comparison_test
import test_scene_ir as ir_test


def fixture():
    geometry = comparison_test.offline().static_mesh_resources[0].geometry
    declaration = [0x20000000, 0x40020000, 0x40040001, 0x40010002, 0x40040003, 0x40040004, 0xffffffff]
    vertex = b"".join(struct.pack("<3fI2fII", *position, 0, *uv, 0, 0)
                      for position, uv in zip(geometry.positions, geometry.uv0))
    index = struct.pack("<3H", *geometry.indices)
    viewport = {"x": 0, "y": 0, "width": 256, "height": 256, "min_z": 0, "max_z": 0.5}
    camera = {"derived_matrix_bits": bridge_test.bits(ir_test.IDENTITY),
              "projection_bits": bridge_test.bits(ir_test.IDENTITY)}
    section = {"first_vertex": 0, "vertex_count": 3, "first_index": 0, "index_count": 3, "triangles": 1}
    draw = {"event": "draw", "op_id": 1, "thread_id": 5, "call": "DrawIndexedPrimitive", "primitive_type": 4,
            "min_vertex_index": 0, "num_vertices": 3, "start_index": 0, "primitive_count": 1,
            "viewport": viewport, "transforms": {"world": [ir_test.IDENTITY], "view": ir_test.IDENTITY,
                                                 "projection": ir_test.IDENTITY},
            "vertex_shader": 17, "vertex_declaration": {"status": "candidate", "generation": 1, "words": declaration},
            "streams": [{"stream": 0, "hr": 0, "buffer": "0xaaa", "stride": 32, "description": {"size": 96}}],
            "indices": {"hr": 0, "buffer": "0xbbb", "base_vertex_index": 0,
                        "description": {"format": 101, "size": 6}},
            "buffer_data": {"source": "application_lock_shadow",
                            "vertex": {"status": "captured", "resource": "0xaaa", "stream": 0,
                                       "first_vertex": 0, "vertex_count": 3, "stride": 32,
                                       "byte_offset": 0, "byte_count": 96, "encoding": "hex", "data": vertex.hex()},
                            "index": {"status": "captured", "resource": "0xbbb", "index_count": 3,
                                      "index_format": 101, "byte_offset": 0, "byte_count": 6,
                                      "encoding": "hex", "data": index.hex()}}}
    result = {"event": "draw_result", "op_id": 1, "hr": 0, "shader_stable": True, "buffer_stable": True}
    header = {"event": "header", "declaration_capture": {"requested": True},
              "buffer_capture": {"requested": True, "encoding": "hex", "component_limit_bytes": 4096,
                                 "frame_limit_bytes": 8192}}
    return geometry, section, draw, result, header, camera


class RuntimeSnapshotEvidenceTests(unittest.TestCase):
    def test_section_verifies_positions_uv_indices_and_capture_state(self):
        geometry, section, draw, result, header, camera = fixture()
        def check():
            return builder.verified_section(geometry, geometry.sections[0], section, draw, result, header, True,
                                            ir_test.IDENTITY, camera, draw["viewport"], 5)
        self.assertTrue(check())
        for component, byte in (("vertex", 0), ("vertex", 16), ("index", 0)):
            part = draw["buffer_data"][component]
            original = part["data"]
            changed = bytearray.fromhex(original)
            changed[byte] ^= 1
            part["data"] = changed.hex()
            self.assertFalse(check(), (component, byte))
            part["data"] = original
        result["buffer_stable"] = False
        self.assertFalse(check())
        result["buffer_stable"] = True
        draw["thread_id"] = 6
        self.assertFalse(check())

    def test_shadow_must_follow_the_same_probe_observation(self):
        geometry, section, draw, result, header, camera = fixture()
        first = {"event": "scene_bridge_observation", "next_d3d_operation": 1, "current_camera": camera}
        other = {"event": "scene_bridge_observation", "next_d3d_operation": 2, "current_camera": camera}
        second_draw = copy.deepcopy(draw)
        second_draw["op_id"] = 2
        second_result = dict(result, op_id=2)
        records = [header, first, draw, result, other, second_draw, second_result]
        proof = builder.find_section_draws(geometry, [section], [0], records, camera, draw, ir_test.IDENTITY)
        self.assertEqual(proof, [{"section": 0, "draw": 1, "matching_draws": 1}])
        draw["buffer_data"]["index"]["data"] = "000000000000"
        with self.assertRaisesRegex(ValueError, "lacks a verified"):
            builder.find_section_draws(geometry, [section], [0], records, camera, draw, ir_test.IDENTITY)

    def test_incomplete_runtime_evidence_fails_closed(self):
        records = comparison_test.observed()
        data = comparison_test.offline().to_dict()
        data["mesh_instances"][0]["id"] = "actor:StaticMeshActor__1520"
        with self.assertRaisesRegex(ValueError, "insufficiently verified"):
            builder.build(comparison.SceneSnapshot.from_dict(data), "a" * 64, records, [], "b" * 64, 3, 2, 4, [0], [0])


if __name__ == "__main__":
    unittest.main()
