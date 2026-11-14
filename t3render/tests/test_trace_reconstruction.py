import copy
from pathlib import Path
import struct
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import offline_mesh_reconstruction as common
import trace_to_reconstruction as reconstruction
from synthetic_d3d8_tokens import packed_declaration, position_shader


class TraceReconstructionTests(unittest.TestCase):
    def setUp(self):
        self.world = common.IDENTITY[:]
        self.world[12:15] = [1, 2, 3]
        viewport = {"x": 0, "y": 0, "width": 1024, "height": 768, "min_z": 0, "max_z": 0.5}
        draw = {"op_id": 8, "min_vertex_index": 10, "viewport": viewport,
                "transforms": {"world": [self.world], "view": common.IDENTITY, "projection": common.IDENTITY},
                "render_states": {"7": 1, "14": 1, "22": 3, "23": 2, "27": 0, "52": 0, "168": 15}}
        transposed = [self.world[col * 4 + row] for row in range(4) for col in range(4)]
        bits = list(struct.unpack("<16I", struct.pack("<16f", *transposed)))
        self.command = {"topology": "triangle_list", "indexed": True, "compatibility": {"d3d8": {"draw": draw}},
                        "vertex_declaration_observed": {"status": "candidate", "words": packed_declaration()},
                        "vertex_program_observed": {"status": "candidate", "words": position_shader()},
                        "vertex_constants_observed": {"status": "sampled", "bits": [0, 0, 0, 0] + bits}}
        vertex = b"".join(struct.pack("<3f", *position) + b"\0" * 20
                          for position in ((0, 0, 0), (1, 0, 0), (0, 1, 0)))
        self.mesh = {"buffered_data_observed": {"vertex": {"status": "captured", "stride_bytes": 32,
                    "vertex_count": 3, "data": vertex.hex()}, "index": {"status": "captured",
                    "index_format": "uint16", "data": struct.pack("<3H", 10, 11, 12).hex()}}}
        self.camera = (common.IDENTITY, common.IDENTITY, (1024, 768, 0, 0.5))

    def test_decodes_index_window_and_applies_world_transform(self):
        points = reconstruction.extract(self.command, self.mesh, self.camera)
        self.assertEqual([p for p, _ in points], [[1, 2, 3], [2, 2, 3], [1, 3, 3]])
        self.assertEqual(len({color for _, color in points}), 1)

    def test_rejects_invalid_buffer_shader_constants_and_render_state(self):
        for change in ("index", "missing", "shader", "constants", "cull"):
            command, mesh = copy.deepcopy(self.command), copy.deepcopy(self.mesh)
            if change == "index": mesh["buffered_data_observed"]["index"]["data"] = struct.pack("<3H", 9, 11, 12).hex()
            if change == "missing": mesh["buffered_data_observed"]["vertex"]["status"] = "unknown"
            if change == "shader": command["vertex_program_observed"]["words"].insert(-1, position_shader()[2])
            if change == "constants": command["vertex_constants_observed"]["bits"][4] = 0
            if change == "cull": command["compatibility"]["d3d8"]["draw"]["render_states"]["22"] = 1
            with self.subTest(change=change), self.assertRaises(ValueError):
                reconstruction.extract(command, mesh, self.camera)

    def test_offline_actor_draw_rejects_ambiguous_section_signatures(self):
        records = [{"event": "frame_end", "frame": 1, "present_hr": 0, "had_reset": False,
                    "concurrent_operations": False},
                   {"event": "draw", "frame": 1, "op_id": 8,
                    "render_states": {"7": 1, "14": 1, "22": 3, "23": 2, "27": 0, "52": 0, "168": 15}},
                   {"event": "draw_result", "frame": 1, "op_id": 8, "hr": 0, "buffer_stable": True}]
        matches = {"actors_within_tolerance": ["bed"],
                   "matches": [{"op_id": 8, "section": 0}, {"op_id": 8, "section": 1}]}
        with mock.patch.object(common.correlation, "correlate", return_value=matches):
            with self.assertRaisesRegex(ValueError, "more than one mesh section"):
                common.create_packet(records, 1, [], "bed", None, [8], [], [])
