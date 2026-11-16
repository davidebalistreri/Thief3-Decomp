"""The captured-frame renderer keeps one semantic bed switch and provenance."""
from dataclasses import replace
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import os

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent/"tools"))
import build_scoped_scene_snapshot as scoped
import scene_pbr_camera_sequence as sequence
import scene_pbr_packet as pbr
import scene_pbr_variants as variants
from scene_ir import VisualLight
from test_scene_pbr_bed import glb
from test_scene_pbr_variants import distinct_bed
from test_scene_material_overrides import FakeRemaster
from test_scene_textured_packet import Resolver
from test_scene_trace_session import two_frames, write_trace
from test_scoped_scene_snapshot import candidate_fixture
from test_analyze_scene_bridge import bits


FAKE_RENDERER = r'''#!/usr/bin/env python3
import pathlib,sys
initial=pathlib.Path(sys.argv[3]); selected="original"
def image(path):
    color=b"\x20\x40\x60" if selected=="original" else b"\x80\xa0\xc0"
    pathlib.Path(path).write_bytes(b"P6\n640 480\n255\n"+color*640*480)
    print("sceneir_pbr_dx12=ok",flush=True)
print("variant_gpu_resources=shared",flush=True)
image(initial)
print("variant_stream=ready",flush=True)
for line in sys.stdin:
    line=line.rstrip("\n")
    if line=="quit": break
    command,rest=line.split(" ",1)
    if command in ("frame","scene"):
        packet,output=rest.split("\t",1)
        if not pathlib.Path(packet).is_file(): sys.exit(2)
        print("variant_scene=updated" if command=="scene" else "variant_camera=updated",flush=True)
    else:
        selected=command;output=rest
    image(output)
'''


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


class CameraSequenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-camera-sequence-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        offline, records = two_frames()
        data = offline.to_dict()
        data["mesh_instances"][0]["id"] = f"actor:{offline.source.map}:{offline.source.fingerprints['map']}:StaticMeshActor__1520"
        data["conventions"]["meters_per_unit"] = .01905
        offline = scoped.SceneSnapshot.from_dict(data)
        offline_raw = offline.to_json().encode()
        self.trace = self.root/"trace.jsonl"
        trace_digest = write_trace(self.trace, records)
        self.paths, scenes, packets, stats = [], [], [], []
        for frame in (3, 4):
            frame_records = scoped.builder.read_frame(self.trace, frame, trace_digest)
            scene, _ = scoped.build(offline, frame_records, frame, 4, 1, None, trace_digest, digest(offline_raw))
            light = VisualLight("candidate:light:test", "omni", (1, 2, 3), None, None, None, None)
            light_scene = replace(scene, lights=(light,))
            calibration = json.dumps({"schema": "t3.scene.pbr-preview", "version": 1, "ambient_linear": [.02]*3,
                                      "exposure": 1, "lights": [{"id": light.id, "color_linear": [1, .5, .25],
                                                                "intensity_candela": 10, "radius_m": .1}]})
            packet, info = pbr.create_packet(scene, light_scene, calibration, Resolver(), FakeRemaster(scene))
            scene_raw = scene.to_json().encode()
            manifest = {"schema": "t3.scene.pbr-packet", "version": int(chr(packet[6])),
                        "packet_sha256": digest(packet), "scene_sha256": digest(scene_raw),
                        "source_frame": frame, "source_fingerprints": scene.source.fingerprints,
                        "calibration_sha256": digest(calibration.encode()), "light_source": {"kind": "runtime_candidates"},
                        "render_pass": info["render_pass"], "stage": info["stage"],
                        "textures": info["textures"], "lights": info["lights"]}
            paths = tuple(self.root/f"{frame}.{kind}" for kind in ("scene", "packet", "manifest"))
            paths[0].write_bytes(scene_raw); paths[1].write_bytes(packet); paths[2].write_text(json.dumps(manifest))
            self.paths.append(paths); scenes.append(scene); packets.append(packet); stats.append(info)
        light = VisualLight("candidate:light:test", "omni", (1, 2, 3), None, None, None, None)
        replacement, replacement_stats = pbr.create_packet(scenes[0], replace(scenes[0], lights=(light,)), calibration,
                                                            Resolver(), FakeRemaster(scenes[0]), bed_glb=glb(scenes[0]))
        body, report = variants.build(scenes[0], packets[0], stats[0], replacement, replacement_stats)
        self.bundle = self.root/"variants.bin"; self.bundle.write_bytes(body)
        variant_manifest = {"schema": "t3.scene.pbr-variants", "version": 1, "packet_sha256": digest(body),
                            "scene_sha256": digest(self.paths[0][0].read_bytes()), "source_frame": 3,
                            "source_fingerprints": scenes[0].source.fingerprints,
                            "calibration_sha256": digest(calibration.encode()), "light_source": {"kind": "runtime_candidates"},
                            "original": stats[0], "variant": report, **replacement_stats}
        self.bundle_manifest = self.root/"variants.manifest.json"
        self.bundle_manifest.write_text(json.dumps(variant_manifest))
        self.renderer = self.root/"fake-renderer"
        self.renderer.write_text(FAKE_RENDERER); self.renderer.chmod(0o755)

    def prepare(self, paths=None):
        return sequence.prepare(self.trace, self.bundle, self.bundle_manifest, paths or self.paths)

    def test_validated_sequence_streams_both_bed_variants(self):
        result = sequence.run(self.prepare(), self.root/"output", self.renderer, effects=("filmic",))
        self.assertEqual([x["frame"] for x in result["frames"]], [3, 4])
        self.assertEqual([(x["frame"], x["variant"]) for x in result["images"]],
                         [(3, "original"), (3, "replacement"), (4, "replacement"), (4, "original")])
        self.assertEqual(result["resource_key"], "asset:bed")
        self.assertEqual(result["light_source"], {"kind": "runtime_candidates"})

    def test_changed_geometry_is_rejected_before_launch(self):
        scene, packet, manifest = self.paths[1]
        raw = bytearray(packet.read_bytes()); raw[-1] ^= 1
        changed = self.root/"changed.packet"; changed.write_bytes(raw)
        data = json.loads(manifest.read_text()); data["packet_sha256"] = digest(raw)
        changed_manifest = self.root/"changed.manifest"; changed_manifest.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "changes resident"):
            self.prepare([self.paths[0], (scene, changed, changed_manifest)])
        self.assertFalse((self.root/"output").exists())

    def test_variant_bundle_tampering_and_missing_frame_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "two or three"):
            self.prepare(self.paths[:1])
        altered = bytearray(self.bundle.read_bytes()); altered[-1] ^= 1
        self.bundle.write_bytes(altered)
        data = json.loads(self.bundle_manifest.read_text()); data["packet_sha256"] = digest(altered)
        self.bundle_manifest.write_text(json.dumps(data))
        with self.assertRaises(ValueError):
            self.prepare()

    def test_highball_environment_is_set_only_for_the_child(self):
        engine, bottle = self.root/"engine", self.root/"bottle"
        (engine/"engine/bin").mkdir(parents=True)
        (engine/"frameworks").mkdir()
        for name in ("wine", "wineserver"): (engine/"engine/bin"/name).touch()
        (bottle/"dosdevices").mkdir(parents=True)
        (bottle/"dosdevices/z:").symlink_to("/")
        with mock.patch.dict(os.environ, {"T3RENDER_TEST_CAPTURE": "1", "WINEDLLPATH": "untrusted"}):
            wine, env = sequence.highball_environment(engine, bottle)
            self.assertEqual(wine, (engine/"engine/bin/wine").resolve())
            self.assertEqual(env["WINEPREFIX"], str(bottle.resolve()))
            self.assertNotIn("T3RENDER_TEST_CAPTURE", env)
            self.assertNotIn("WINEDLLPATH", env)
            self.assertEqual(os.environ["WINEDLLPATH"], "untrusted")


class ResidentVisibilityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-visibility-sequence-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        offline, records = candidate_fixture()
        data = offline.to_dict()
        data["mesh_instances"][0]["id"] = f"actor:{offline.source.map}:{offline.source.fingerprints['map']}:StaticMeshActor__1520"
        data["mesh_instances"][1]["transform"][12] += 2
        data["conventions"]["meters_per_unit"] = .01905
        geometry = data["static_mesh_resources"][0]["geometry"]
        for k in range(1, 6):
            geometry["positions"].append([k/8, 0, 0]); geometry["uv0"].append([k/8, 0])
            geometry["normals"].append([0, 0, 1]); geometry["tangents"].append([1, 0, 0])
        geometry["indices"][6:9] = [0, 3, 2]
        bsp = data["bsp_blocks"][0]["geometry"]
        bsp["positions"].append([.5, 0, 0]); bsp["uv0"].append([.5, 0])
        bsp["normals"].append([0, 0, 1]); bsp["tangents"].append([1, 0, 0])
        bsp["indices"][3:6] = [0, 3, 2]
        offline = scoped.SceneSnapshot.from_dict(data)
        observation = next(row for row in records if row["event"] == "scene_bridge_observation")
        node = observation["target"]["mesh_node"]
        node["counts"]["vertices"] = 8
        for section in node["sections"]: section["vertex_count"] = 8
        block = observation["bsp"]["candidates"][0]
        block["counts"]["vertices"] = 4
        for section in block["sections"]: section["vertex_count"] = 4
        entry = observation["entries"][2]
        entry["object_matrix_bits"] = bits(data["mesh_instances"][1]["transform"])
        entry["owner_location_bits"] = bits(data["mesh_instances"][1]["transform"][12:15])
        begin = next(row for row in records if row["event"] == "scene_bridge_submission_begin" and row["ticket"] == 15)
        begin["matrix_bits"] = entry["object_matrix_bits"]
        for row in records:
            if row.get("event") == "draw" and row.get("op_id") in (2, 3): row["num_vertices"] = 4
            if row.get("event") == "draw" and row.get("op_id") in (4, 5, 6):
                row["num_vertices"] = 8
                if row["op_id"] == 6: row["transforms"]["world"][0] = data["mesh_instances"][1]["transform"]
        header = copy.deepcopy(records[0]); header["requested_frames"] = 2
        first = copy.deepcopy(records[1:-1]); first[0]["device"] = "0x1234"
        second = copy.deepcopy(first)
        second = [row for row in second if not (row.get("ticket") == 15 or
                  row.get("event") in ("draw", "draw_result") and row.get("op_id") == 6)]
        next(row for row in second if row["event"] == "scene_bridge_frame").update(submission_started=4, submission_completed=4)
        for row in second:
            row["frame"] = 4
            if row["event"] in ("scene_bridge_observation", "scene_bridge_submission_begin", "scene_bridge_submission_end"):
                row["ticket"] += 100
            if row["event"] == "draw": row["submission_ticket"] += 100
        self.trace = self.root/"synthetic-trace.jsonl"
        trace_digest = write_trace(self.trace, [header, *first, *second, {"event": "capture_end", "frames": 2}])
        scoped.bridge.trace_summary.summarize(self.trace)
        offline_raw = offline.to_json().encode()
        self.paths, self.bundles = [], []
        for frame in (3, 4):
            frame_records = scoped.builder.read_frame(self.trace, frame, trace_digest)
            scene, _ = scoped.build(offline, frame_records, frame, 4, 1, None, trace_digest, digest(offline_raw),
                                    include_asset_candidates=True)
            light = VisualLight("candidate:light:test", "omni", (1, 2, 3), None, None, None, None)
            calibration = json.dumps({"schema": "t3.scene.pbr-preview", "version": 1, "ambient_linear": [.02]*3,
                                      "exposure": 1, "lights": [{"id": light.id, "color_linear": [1, .5, .25],
                                                                "intensity_candela": 10, "radius_m": .1}]})
            args = (scene, replace(scene, lights=(light,)), calibration, Resolver(), FakeRemaster(scene))
            packet, original_stats = pbr.create_packet(*args)
            replacement, replacement_stats = pbr.create_packet(*args, bed_glb=distinct_bed(scene))
            bundle, report = variants.build(scene, packet, original_stats, replacement, replacement_stats)
            scene_raw = scene.to_json().encode()
            manifest = {"schema": "t3.scene.pbr-packet", "version": int(chr(packet[6])),
                        "packet_sha256": digest(packet), "scene_sha256": digest(scene_raw), "source_frame": frame,
                        "source_fingerprints": scene.source.fingerprints, "calibration_sha256": digest(calibration.encode()),
                        "light_source": {"kind": "runtime_candidates"}, **original_stats}
            variant_manifest = {"schema": "t3.scene.pbr-variants", "version": 1, "packet_sha256": digest(bundle),
                                "scene_sha256": digest(scene_raw), "source_frame": frame,
                                "source_fingerprints": scene.source.fingerprints,
                                "calibration_sha256": digest(calibration.encode()), "light_source": {"kind": "runtime_candidates"},
                                "original": original_stats, "variant": report, **replacement_stats}
            paths = tuple(self.root/f"{frame}.{kind}" for kind in ("scene", "packet", "manifest"))
            paths[0].write_bytes(scene_raw); paths[1].write_bytes(packet); paths[2].write_text(json.dumps(manifest))
            pair = (self.root/f"{frame}.bundle", self.root/f"{frame}.bundle-manifest")
            pair[0].write_bytes(bundle); pair[1].write_text(json.dumps(variant_manifest))
            self.paths.append(paths); self.bundles.append(pair)
        self.renderer = self.root/"fake-renderer"
        self.renderer.write_text(FAKE_RENDERER); self.renderer.chmod(0o755)

    def prepare(self):
        return sequence.prepare(self.trace, *self.bundles[0], self.paths, updates=[self.bundles[1]])

    def test_synthetic_visibility_subset_streams_with_semantic_provenance(self):
        plan = self.prepare()
        self.assertGreater(len(plan["frames"][0]["scene"].submissions), len(plan["frames"][1]["scene"].submissions))
        result = sequence.run(plan, self.root/"output", self.renderer)
        self.assertEqual(result["update_mode"], "resident_subsets")
        self.assertEqual([x["frame"] for x in result["frames"]], [3, 4])
        for item, bundle, paths in zip(result["frames"], self.bundles, self.paths):
            self.assertEqual(item["variant_bundle_sha256"], digest(bundle[0].read_bytes()))
            self.assertEqual(item["variant_manifest_sha256"], digest(bundle[1].read_bytes()))
            self.assertEqual(item["packet_manifest_sha256"], digest(paths[2].read_bytes()))

    def test_changed_semantic_identity_is_rejected_before_launch(self):
        data = json.loads(self.bundles[1][1].read_text())
        old = data["original"]["draws"][0]; new = data["draws"][0]
        old["target_id"] = new["target_id"] = "bsp:unrelated"
        self.bundles[1][1].write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "semantically different"):
            self.prepare()
        self.assertFalse((self.root/"output").exists())


if __name__ == "__main__":
    unittest.main()
