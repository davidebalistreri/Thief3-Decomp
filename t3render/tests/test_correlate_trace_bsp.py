import copy
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import correlate_trace_bsp as bsp


class BspCorrelationTests(unittest.TestCase):
    def setUp(self):
        self.section = SimpleNamespace(name="floor", triangles=1, first_vertex=0, vertex_count=3,
                                       first_index=0, index_count=3)
        self.block = SimpleNamespace(offset=16, block_id=7, mesh=SimpleNamespace(
            indices=[0, 1, 2], sections=[self.section], bbox_min=(0, 0, 0), bbox_max=(1, 1, 0)))
        self.vertices = bytes(range(96))
        self.source = bytes(16 + 0x50) + self.vertices
        index = struct.pack("<3H", 0, 1, 2)
        self.draw = {"event": "draw", "frame": 7, "op_id": 5, "call": "DrawIndexedPrimitive",
                     "primitive_type": 4, "primitive_count": 1, "num_vertices": 3,
                     "min_vertex_index": 0, "start_index": 0,
                     "transforms": {"world": [bsp.IDENTITY], "view": [1], "projection": [2]},
                     "viewport": {"width": 4},
                     "streams": [{"stream": 0, "hr": 0, "buffer": "0xabc", "stride": 32}],
                     "indices": {"hr": 0, "buffer": "0xdef", "base_vertex_index": 4},
                     "buffer_data": {"source": "application_lock_shadow",
                        "vertex": {"status": "captured", "encoding": "hex", "resource": "0xabc",
                                   "first_vertex": 4, "vertex_count": 3, "stream": 0, "stride": 32,
                                   "byte_offset": 128, "byte_count": 96, "data": self.vertices.hex()},
                        "index": {"status": "captured", "encoding": "hex", "resource": "0xdef",
                                  "index_format": 101, "index_count": 3,
                                  "byte_offset": 0, "byte_count": 6, "data": index.hex()}}}
        self.result = {"event": "draw_result", "frame": 7, "op_id": 5, "hr": 0, "buffer_stable": True}
        self.end = {"event": "frame_end", "frame": 7, "present_hr": 0,
                    "concurrent_operations": False, "had_reset": False}

    def comparison(self, draw=None, result=None, end=None):
        return bsp.compare(self.block, self.section, self.source, draw or self.draw,
                           self.result if result is None else result, self.end if end is None else end)

    def test_exact_payload_and_changed_vertex_are_distinct(self):
        self.assertEqual(self.comparison(), {"vertex_bytes": "identical", "index_bytes": "identical"})
        changed = copy.deepcopy(self.draw)
        changed["buffer_data"]["vertex"]["data"] = "ff" + self.vertices[1:].hex()
        self.assertEqual(self.comparison(changed), {"vertex_bytes": "different", "index_bytes": "identical"})

    def test_unsafe_or_malformed_observation_is_unavailable(self):
        for change in ("unstable", "reset", "wrong_binding", "incomplete"):
            draw, result, end = copy.deepcopy(self.draw), copy.deepcopy(self.result), copy.deepcopy(self.end)
            if change == "unstable":
                result["buffer_stable"] = False
            elif change == "reset":
                end["had_reset"] = True
            elif change == "wrong_binding":
                draw["streams"][0]["buffer"] = "0x123"
            else:
                draw["buffer_data"]["index"]["data"] = "00"
            self.assertEqual(self.comparison(draw, result, end),
                             {"vertex_bytes": "unavailable", "index_bytes": "unavailable"}, change)

    def test_reference_camera_and_world_filter_candidates(self):
        self.assertTrue(bsp.candidate(self.draw, self.draw, self.section))
        reference = copy.deepcopy(self.draw)
        reference["transforms"]["view"] = [3]
        self.assertFalse(bsp.candidate(self.draw, reference, self.section))
        moved = copy.deepcopy(self.draw)
        moved["transforms"]["world"] = [[0] * 16]
        self.assertFalse(bsp.candidate(moved, self.draw, self.section))


if __name__ == "__main__":
    unittest.main()
