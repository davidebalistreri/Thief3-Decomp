"""Metallic packets preserve old defaults and validate bounded per-draw factors."""
import json
from pathlib import Path
import struct
import sys
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import scene_pbr_metallic_fixture as fixture
import scene_pbr_packet as pbr
import test_scene_pbr_candlestick as candles
import test_scene_pbr_packet as packet_tests
from test_scene_material_overrides import FakeRemaster
from test_scene_textured_packet import Resolver


class MetallicPacketTests(unittest.TestCase):
    setUpClass = classmethod(packet_tests.PbrPacketTests.setUpClass.__func__)
    tearDownClass = classmethod(packet_tests.PbrPacketTests.tearDownClass.__func__)
    parse = packet_tests.PbrPacketTests.parse

    def test_bounded_v5_and_invalid_factors_fail_closed(self):
        raw = fixture.fixture()
        self.assertEqual(self.parse(raw).returncode, 0)
        flags = 196
        first_factor = 196+4+32+7*16+20+12
        for offset, value in ((flags, struct.pack("<I", 2)),
                              (first_factor, struct.pack("<f", -1)),
                              (first_factor, struct.pack("<f", 1.01)),
                              (first_factor, struct.pack("<I", 0x7fc00000))):
            with self.subTest(offset=offset, value=value):
                self.assertEqual(self.parse(raw[:offset]+value+raw[offset+len(value):]).returncode, 2)
        self.assertEqual(self.parse(raw[:-1]).returncode, 2)

    def test_candlestick_metal_is_selected_in_shadowed_and_unshadowed_packets(self):
        scene, lights, config, actors = candles.inputs()
        args = (scene, lights, json.dumps(config), Resolver(), FakeRemaster(scene))
        glb = candles.glb(scene)
        shadow = {"size": 128, "near_m": .05, "far_m": 15, "bias_m": .015, "softness": .5}
        original, _ = pbr.create_packet(*args, candlestick_glb=glb)
        for options in ({}, {"shadows": shadow, "actors": actors},
                        {"shadows": shadow, "actors": actors, "flame_glb": glb}):
            with self.subTest(options=options):
                packet, info = pbr.create_packet(*args, candlestick_glb=glb, metallic=True, **options)
                self.assertEqual(packet[:8], pbr.METALLIC_MAGIC)
                self.assertEqual(self.parse(packet).returncode, 0)
                metals = [d for d in info["draws"] if d["metallic"] != 0]
                self.assertEqual([(d["material"], d["metallic"]) for d in metals], [("METAL1Goodness_D", .8)])
                self.assertEqual(info["metallic_mode"]["candlestick_metal_factor"], .8)
        restored, _ = pbr.create_packet(*args, candlestick_glb=glb)
        self.assertEqual(original, restored)
        with self.assertRaisesRegex(ValueError, "metallic mode"):
            pbr.create_packet(*args, metallic=1)


if __name__ == "__main__":
    unittest.main()
