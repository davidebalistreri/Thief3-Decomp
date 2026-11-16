"""Temporal history is camera-reprojected, bounded and provenance-checked."""
from dataclasses import replace
import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import scene_pbr_packet as pbr
import scene_temporal_resolve as temporal
import scene_temporal_fixture as fixture
import test_scene_pbr_packet as packet_tests
from test_scene_textured_packet import Resolver


def raw_history(packet, color=(.5, .5, .5), jitter=(0, 0), flags=0, changed_position=False, neighbor=False):
    h = struct.unpack_from("<8sIIff32f4f4fIII", packet)
    width, height = h[1:3]
    data = bytearray(temporal.HEADER.pack(b"T3TEMP01", width, height, hashlib.sha256(packet).digest(),
                                          *h[5:37], *h[37:41], h[44], *jitter, flags))
    for y in range(height):
        for x in range(width):
            rgb = (.75, .75, .75) if neighbor and (x, y) == (33, 32) else color
            data.extend(struct.pack("<4e", *rgb, 1))
    for y in range(height):
        for x in range(width):
            # The fixture uses identity view/projection and a camera at z=2.
            px = (x-jitter[0])/32-1
            py = 1-(y-jitter[1])/32
            if changed_position and (x, y) == (32, 32): px += .2
            data.extend(struct.pack("<4e", px, py, -1.5, 1))
    return bytes(data)


class TemporalResolveTests(unittest.TestCase):
    def setUp(self):
        self.packet = packet_tests.gpu_fixture.fixture()
        self.raw = raw_history(self.packet)
        self.frame = temporal.parse(self.raw, self.packet)

    def test_identity_and_zero_weight_preserve_history(self):
        result, motion, stats = temporal.resolve(self.frame, self.frame)
        self.assertEqual(result, self.raw)
        self.assertEqual(stats["counts"].get("reprojected"), 4096)
        self.assertEqual(stats["max_reprojection_pixels"], 0)
        brighter = temporal.parse(raw_history(self.packet, color=(1, 1, 1)), self.packet)
        result, _, _ = temporal.resolve(brighter, self.frame, weight=0)
        self.assertEqual(result, self.raw)

    def test_clamps_history_and_rejects_disoccluded_positions(self):
        current = temporal.parse(raw_history(self.packet, neighbor=True), self.packet)
        previous = temporal.parse(raw_history(self.packet, color=(1, 1, 1)), self.packet)
        result, motion, stats = temporal.resolve(previous, current)
        value = struct.unpack_from("<4e", result, temporal.HEADER.size+(32*64+32)*8)
        self.assertAlmostEqual(value[0], .7, delta=.001)
        self.assertEqual(stats["counts"].get("reprojected"), 4096)
        previous = temporal.parse(raw_history(self.packet, color=(1, 1, 1), changed_position=True), self.packet)
        result, motion, stats = temporal.resolve(previous, current)
        value = struct.unpack_from("<4e", result, temporal.HEADER.size+(32*64+32)*8)
        self.assertEqual(value[0], .5)
        self.assertEqual(stats["counts"].get("disoccluded"), 1)
        self.assertEqual(struct.unpack_from("<ffI", motion, temporal.MOTION_HEADER.size+(32*64+32)*12)[2], 2)

    def test_camera_translation_reprojects_one_pixel(self):
        shifted = bytearray(self.packet)
        struct.pack_into("<f", shifted, 24+12*4, -1/32)
        struct.pack_into("<f", shifted, 152, 1/32)
        shifted = bytes(shifted)
        current = temporal.parse(raw_history(shifted), shifted)
        _, motion, stats = temporal.resolve(self.frame, current)
        self.assertEqual(stats["counts"].get("outside"), 64)
        self.assertAlmostEqual(stats["max_reprojection_pixels"], 1, places=5)
        self.assertEqual(struct.unpack_from("<ffI", motion, temporal.MOTION_HEADER.size+(32*64+32)*12), (1, 0, 3))

    def test_supersampled_fixture_checks_resolution_quality(self):
        samples = [raw_history(self.packet, color=(value, value, value), jitter=(jitter, 0))
                   for value, jitter in ((.2, -.375), (.4, -.125), (.6, .125), (.8, .375))]
        reference = fixture.reference(samples, self.packet)
        pixels = reference[len(b"P6\n64 64\n255\n"):]
        self.assertTrue(all(abs(x-156) <= 1 for x in pixels))
        with self.assertRaisesRegex(ValueError, "four specified"):
            fixture.reference(samples[:-1]+samples[:1], self.packet)
        header = b"P6\n64 64\n255\n"
        ideal = header+bytes([100, 100, 100])*64*64
        noisy = header+bytes([110, 110, 110])*64*64
        improved = header+bytes([101, 101, 101])*64*64
        fixture.verify(ideal, noisy, improved)
        with self.assertRaisesRegex(ValueError, "did not improve"):
            fixture.verify(ideal, noisy, noisy)

    def test_hdr_upscale_interpolates_and_preserves_depth_edges(self):
        header = list(temporal.HEADER.unpack_from(self.raw))
        header[1:3] = [2, 2]
        colors = ((0, 0, 0, 1), (1, 0, 0, 1), (0, 1, 0, 1), (0, 0, 1, 1))
        def small(positions):
            return temporal.HEADER.pack(*header) + b"".join(struct.pack("<4e", *x) for x in colors) + \
                b"".join(struct.pack("<4e", *x) for x in positions)
        positions = [(0, 0, 0, 1)]*4
        output = temporal.image(small(positions), 2)
        self.assertTrue(output.startswith(b"P6\n4 4\n255\n"))
        offset = len(b"P6\n4 4\n255\n") + (1*4+1)*3
        def mapped(v):
            v = v/(1+v)
            return round(255*(12.92*v if v <= .0031308 else 1.055*v**(1/2.4)-.055))
        self.assertEqual(tuple(output[offset:offset+3]), (mapped(.1875), mapped(.1875), mapped(.0625)))
        split = [(0, 0, 0, 1), (10, 0, 0, 1), (0, 0, 0, 1), (10, 0, 0, 1)]
        edge = temporal.image(small(split), 2)
        offset = len(b"P6\n4 4\n255\n") + 3
        self.assertEqual(tuple(edge[offset:offset+3]), (0, 0, 0))
        for invalid in (0, 3, True):
            with self.assertRaisesRegex(ValueError, "upscale"):
                temporal.image(small(positions), invalid)

    def test_filmic_temporal_display_uses_the_recorded_curve(self):
        ordinary = raw_history(self.packet, color=(1, 1, 1))
        filmic = raw_history(self.packet, color=(1, 1, 1), flags=16)
        plain, mapped = temporal.image(ordinary), temporal.image(filmic)
        offset = len(b"P6\n64 64\n255\n")
        self.assertEqual(plain[offset], 188)
        self.assertTrue(230 <= mapped[offset] <= 233)
        with self.assertRaisesRegex(ValueError, "effects"):
            temporal.compatible(temporal.parse(ordinary, self.packet), temporal.parse(filmic, self.packet))

    def test_invalid_headers_pixels_and_settings_fail_closed(self):
        for raw in (self.raw[:-1], b"badmagic"+self.raw[8:],
                    self.raw[:16]+bytes(32)+self.raw[48:],
                    self.raw[:temporal.HEADER.size-4]+struct.pack("<I", 32)+self.raw[temporal.HEADER.size:]):
            with self.subTest(raw=raw[:8]), self.assertRaises(ValueError):
                temporal.parse(raw, self.packet)
        for change in ((temporal.HEADER.size, struct.pack("<H", 0x7c00)),
                       (temporal.HEADER.size+64*64*8+6, struct.pack("<e", .5))):
            offset, value = change
            raw = self.raw[:offset]+value+self.raw[offset+len(value):]
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, "pixels"):
                temporal.resolve(temporal.parse(raw, self.packet), self.frame)
        for weight in (-1, 1, True, float("nan")):
            with self.subTest(weight=weight), self.assertRaises(ValueError):
                temporal.resolve(self.frame, self.frame, weight)
        with self.assertRaisesRegex(ValueError, "effects"):
            temporal.compatible(self.frame, temporal.parse(raw_history(self.packet, flags=1), self.packet))

    def test_runtime_provenance_and_frame_order_are_required(self):
        scene, lights, calibration = packet_tests.inputs()
        def capture(scene, lights, metallic=False):
            packet, info = pbr.create_packet(scene, lights, json.dumps(calibration), Resolver(), None, metallic=metallic)
            source = scene.to_json().encode()
            manifest = {"schema": "t3.scene.pbr-packet", "version": int(chr(packet[6])), "packet_sha256": hashlib.sha256(packet).hexdigest(),
                        "scene_sha256": hashlib.sha256(source).hexdigest(), "source_frame": scene.source.frame_id,
                        "source_fingerprints": scene.source.fingerprints, "calibration_sha256": "a"*64,
                        "render_pass": info["render_pass"], "stage": info["stage"],
                        "textures": info["textures"], "lights": info["lights"]}
            if metallic:
                manifest["metallic_mode"] = info["metallic_mode"]
            h = struct.unpack_from("<8sIIff32f4f4fIII", packet)
            header = temporal.HEADER.pack(b"T3TEMP01", h[1], h[2], hashlib.sha256(packet).digest(),
                                          *h[5:37], *h[37:41], h[44], 0, 0, 0)
            raw = header+bytes(h[1]*h[2]*16)
            item = temporal.parse(raw, packet)
            return item, temporal.validate_runtime(item, source, json.dumps(manifest).encode()), source, manifest
        old, old_runtime, source, manifest = capture(scene, lights)
        camera = replace(scene.camera, source=replace(scene.camera.source, frame_id=4))
        changed = replace(scene, source=replace(scene.source, frame_id=4), camera=camera)
        newer_lights = replace(lights, source=changed.source, camera=camera)
        new, new_runtime, _, _ = capture(changed, newer_lights)
        temporal.compatible(old, new, old_runtime, new_runtime)
        fingerprints = dict(changed.source.fingerprints, trace="c"*64)
        other_source = replace(changed.source, fingerprints=fingerprints)
        other_camera = replace(camera, source=replace(camera.source, fingerprint=fingerprints["trace"]))
        other_scene = replace(changed, source=other_source, camera=other_camera)
        other_lights = replace(newer_lights, source=other_source, camera=other_camera)
        other_frame, other_runtime, _, _ = capture(other_scene, other_lights)
        with self.assertRaisesRegex(ValueError, "capture"):
            temporal.compatible(old, other_frame, old_runtime, other_runtime)
        altered_source = replace(changed.source, fingerprints=dict(changed.source.fingerprints, actors="d"*64))
        altered_scene = replace(changed, source=altered_source)
        altered_lights = replace(newer_lights, source=altered_source)
        altered_frame, altered_runtime, _, _ = capture(altered_scene, altered_lights)
        with self.assertRaisesRegex(ValueError, "assets"):
            temporal.compatible(old, altered_frame, old_runtime, altered_runtime)
        old_metal, old_metal_runtime, old_metal_source, old_metal_manifest = capture(scene, lights, True)
        new_metal, new_metal_runtime, _, _ = capture(changed, newer_lights, True)
        temporal.compatible(old_metal, new_metal, old_metal_runtime, new_metal_runtime)
        with self.assertRaisesRegex(ValueError, "material"):
            temporal.compatible(old, new_metal, old_runtime, new_metal_runtime)
        altered = copy.deepcopy(new_metal_runtime[1]); altered["metallic_mode"]["candlestick_metal_factor"] = .5
        with self.assertRaisesRegex(ValueError, "material"):
            temporal.compatible(old_metal, new_metal, old_metal_runtime, (new_metal_runtime[0], altered))
        for value in (None, {}, {"candlestick_metal_factor": 2, "other_materials_factor": 0},
                      {"candlestick_metal_factor": True, "other_materials_factor": 0}):
            altered = copy.deepcopy(old_metal_manifest); altered["metallic_mode"] = value
            with self.subTest(metallic_mode=value), self.assertRaisesRegex(ValueError, "provenance"):
                temporal.validate_runtime(old_metal, old_metal_source, json.dumps(altered).encode())
        for key in temporal.GEOMETRY_OVERRIDES:
            previous_manifest = copy.deepcopy(old_runtime[1])
            current_manifest = copy.deepcopy(new_runtime[1])
            previous_manifest[key] = {"source_sha256": "b"*64}
            current_manifest[key] = {"source_sha256": "b"*64}
            temporal.compatible(old, new, (old_runtime[0], previous_manifest), (new_runtime[0], current_manifest))
            current_manifest[key]["source_sha256"] = "c"*64
            with self.subTest(override=key, change="asset"), self.assertRaisesRegex(ValueError, "geometry"):
                temporal.compatible(old, new, (old_runtime[0], previous_manifest), (new_runtime[0], current_manifest))
            del current_manifest[key]
            with self.subTest(override=key, change="disabled"), self.assertRaisesRegex(ValueError, "geometry"):
                temporal.compatible(old, new, (old_runtime[0], previous_manifest), (new_runtime[0], current_manifest))
            for invalid in ({}, {"source_sha256": "x"*64}, []):
                bad = copy.deepcopy(manifest); bad[key] = invalid
                with self.subTest(override=key, invalid=invalid), self.assertRaisesRegex(ValueError, "provenance"):
                    temporal.validate_runtime(old, source, json.dumps(bad).encode())
        changed_mode = copy.deepcopy(new_runtime[1]); changed_mode["bedroom_prop_materials"] = []
        with self.assertRaisesRegex(ValueError, "material"):
            temporal.compatible(old, new, old_runtime, (new_runtime[0], changed_mode))
        bad = copy.deepcopy(manifest); bad["bedroom_prop_materials"] = None
        with self.assertRaisesRegex(ValueError, "provenance"):
            temporal.validate_runtime(old, source, json.dumps(bad).encode())
        wrong_lights = copy.deepcopy(new_runtime[1])
        wrong_lights["lights"][0]["position"][0] += 1
        with self.assertRaisesRegex(ValueError, "lighting settings"):
            temporal.compatible(old, new, old_runtime, (new_runtime[0], wrong_lights))
        with self.assertRaisesRegex(ValueError, "frame|order"):
            temporal.compatible(new, old, new_runtime, old_runtime)
        with self.assertRaisesRegex(ValueError, "provenance"):
            temporal.validate_runtime(old, source+b" ", json.dumps(manifest).encode())
        manifest["packet_sha256"] = "0"*64
        with self.assertRaisesRegex(ValueError, "provenance"):
            temporal.validate_runtime(old, source, json.dumps(manifest).encode())

    def test_validation_only_never_creates_a_resolved_artifact(self):
        with tempfile.TemporaryDirectory(prefix="t3render-temporal-validate-") as directory:
            root = Path(directory)
            packet, history = root/"packet.bin", root/"history.bin"
            packet.write_bytes(self.packet); history.write_bytes(self.raw)
            args = [sys.executable, str(HERE.parent/"tools"/"scene_temporal_resolve.py"), str(history), str(history),
                    "--previous-packet", str(packet), "--current-packet", str(packet), "--synthetic", "--validate-only"]
            result = subprocess.run(args, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)["source"], "synthetic")
            self.assertEqual({p.name for p in root.iterdir()}, {"packet.bin", "history.bin"})
            invalid = subprocess.run(args + ["--weight", "nan"], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(invalid.returncode, 0)
            self.assertIn("history weight", invalid.stderr)


if __name__ == "__main__":
    unittest.main()
