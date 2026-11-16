"""Synthetic two-frame evidence; these records are not runtime captures."""
import copy
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from test_scoped_scene_snapshot import builder, fixture
from test_scene_textured_packet import Resolver
import scene_pbr_packet as pbr
import build_scoped_scene_sequence as sequence
import scene_temporal_resolve as temporal
import scene_trace_session as session
from scene_ir import VisualLight


def two_frames():
    scene, records = fixture()
    header = copy.deepcopy(records[0])
    header["requested_frames"] = 2
    first = copy.deepcopy(records[1:-1])
    first[0]["device"] = "0x1234"
    second = copy.deepcopy(first)
    for row in second:
        row["frame"] = 4
        if row["event"] in ("scene_bridge_observation", "scene_bridge_submission_begin", "scene_bridge_submission_end"):
            row["ticket"] += 100
        if row["event"] == "draw":
            row["submission_ticket"] += 100
    return scene, [header, *first, *second, {"event": "capture_end", "frames": 2}]


def write_trace(path, records):
    path.write_text("".join(json.dumps(row) + "\n" for row in records))
    return hashlib.sha256(path.read_bytes()).hexdigest()


def with_digest(scene, digest):
    return replace(scene, source=replace(scene.source, fingerprints=dict(scene.source.fingerprints, trace=digest)),
                   camera=replace(scene.camera, source=replace(scene.camera.source, fingerprint=digest)))


class TraceSessionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="t3render-session-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "trace.jsonl"
        scene, self.records = two_frames()
        digest = write_trace(self.path, self.records)
        builder.bridge.trace_summary.summarize(self.path)
        frames = [builder.builder.read_frame(self.path, frame, digest) for frame in (3, 4)]
        self.scenes = [builder.build(scene, frame_records, frame, 4, 1, None, digest, "c" * 64)[0]
                       for frame, frame_records in zip((3, 4), frames)]

    def validate(self, records=None, scenes=None):
        scenes = scenes or self.scenes
        if records is not None:
            digest = write_trace(self.path, records)
            scenes = [with_digest(scene, digest) for scene in scenes]
        return session.validate(self.path, *scenes, 4, 1)

    def test_consecutive_frames_verify_against_finalized_trace(self):
        result = self.validate()
        self.assertEqual((result["previous_frame"], result["current_frame"]), (3, 4))
        self.assertNotIn("0x1234", json.dumps(result))

    def test_missing_continuity_is_rejected(self):
        for change in ("reset", "device", "missing_device", "unsynchronized", "missing_end", "gap", "absent_target"):
            records = copy.deepcopy(self.records)
            second = next(row for row in records if row.get("event") == "frame_begin" and row.get("frame") == 4)
            if change == "reset": records.insert(records.index(second), {"event": "reset", "frame": 4})
            elif change == "device": second["device"] = "0x5678"
            elif change == "missing_device": del second["device"]
            elif change == "unsynchronized":
                next(row for row in records if row.get("event") == "scene_bridge_frame" and row.get("frame") == 4)["status"] = "unknown"
            elif change == "missing_end": records.pop()
            elif change == "gap":
                for row in records:
                    if row.get("frame") == 4: row["frame"] = 5
            else:
                target = next(row for row in records if row.get("event") == "scene_bridge_observation" and
                              row.get("frame") == 4)["target"]
                target.update(list_occurrences=0, entry=None)
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.validate(records)

    def test_scene_camera_transform_and_draw_are_rechecked(self):
        for change in ("camera", "transform", "draw", "section", "mesh_name", "bsp_bounds", "map"):
            data = self.scenes[1].to_dict()
            if change == "camera": data["camera"]["view"][12] += 1
            elif change == "transform": data["mesh_instances"][0]["transform"][12] += 1
            elif change == "draw": data["submissions"][0]["draw_id"] = 99
            elif change == "section": data["submissions"][0]["section"] = 1
            elif change == "mesh_name": data["static_mesh_resources"][0]["name"] = "different"
            elif change == "bsp_bounds": data["bsp_blocks"][0]["geometry"]["positions"][1][0] = 2
            else: data["source"]["map"] = "OtherMap"
            altered = builder.SceneSnapshot.from_dict(data)
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.validate(scenes=[self.scenes[0], altered])

    def test_other_trace_or_nonconsecutive_scenes_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "fingerprint"):
            self.validate(scenes=[self.scenes[0], with_digest(self.scenes[1], "0" * 64)])
        source = replace(self.scenes[1].source, frame_id=5)
        camera = replace(self.scenes[1].camera, source=replace(self.scenes[1].camera.source, frame_id=5))
        with self.assertRaisesRegex(ValueError, "consecutive"):
            self.validate(scenes=[self.scenes[0], replace(self.scenes[1], source=source, camera=camera)])

    def test_temporal_cli_requires_and_checks_the_actual_trace(self):
        root = Path(self.directory.name)
        calibration = json.dumps({"schema": "t3.scene.pbr-preview", "version": 1,
                                  "ambient_linear": [.02, .03, .04], "exposure": 1,
                                  "lights": [{"id": "candidate:light:test", "color_linear": [1, .5, .25],
                                              "intensity_candela": 10, "radius_m": .1}]})
        paths = []
        for index, original in enumerate(self.scenes):
            scene = replace(original, conventions=replace(original.conventions, meters_per_unit=.01905))
            raw_scene = scene.to_json().encode()
            light = VisualLight("candidate:light:test", "omni", (1, 2, 3), None, None, None, None)
            packet, info = pbr.create_packet(scene, replace(scene, lights=(light,)), calibration, Resolver(), None)
            header = struct.unpack_from("<8sIIff32f4f4fIII", packet)
            raw_history = temporal.HEADER.pack(b"T3TEMP01", header[1], header[2], hashlib.sha256(packet).digest(),
                                               *header[5:37], *header[37:41], header[44], 0, 0, 0)
            raw_history += bytes(header[1] * header[2] * 16)
            manifest = {"schema": "t3.scene.pbr-packet", "version": int(chr(packet[6])),
                        "packet_sha256": hashlib.sha256(packet).hexdigest(),
                        "scene_sha256": hashlib.sha256(raw_scene).hexdigest(), "source_frame": scene.source.frame_id,
                        "source_fingerprints": scene.source.fingerprints, "calibration_sha256": "a" * 64,
                        "render_pass": info["render_pass"], "stage": info["stage"],
                        "textures": info["textures"], "lights": info["lights"]}
            scene_path, packet_path, history_path, manifest_path = (root / f"{index}.{suffix}" for suffix in
                                                                    ("scene", "packet", "history", "manifest"))
            scene_path.write_bytes(raw_scene)
            packet_path.write_bytes(packet)
            history_path.write_bytes(raw_history)
            manifest_path.write_text(json.dumps(manifest))
            paths.append((scene_path, packet_path, history_path, manifest_path))
        old, new = paths
        args = [sys.executable, str(Path(temporal.__file__)), str(old[2]), str(new[2]),
                "--previous-packet", str(old[1]), "--current-packet", str(new[1]),
                "--previous-scene", str(old[0]), "--current-scene", str(new[0]),
                "--previous-manifest", str(old[3]), "--current-manifest", str(new[3]), "--validate-only"]
        missing = subprocess.run(args, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("--trace", missing.stderr)
        result = subprocess.run(args + ["--trace", str(self.path)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["trace_session"]["current_frame"], 4)
        self.path.write_text(self.path.read_text() + " ")
        tampered = subprocess.run(args + ["--trace", str(self.path)], capture_output=True, text=True, timeout=10)
        self.assertNotEqual(tampered.returncode, 0)

    def test_sequence_cli_builds_all_frames_without_overwriting(self):
        root = Path(self.directory.name)
        scene, _ = two_frames()
        reference = root / "offline.json"
        reference.write_text(scene.to_json())
        output = root / "sequence"
        args = [sys.executable, str(Path(sequence.__file__)), str(self.path), "--scene", str(reference),
                "--pass", "4", "--stage", "1", "--output-dir", str(output)]
        result = subprocess.run(args, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads(result.stdout)
        self.assertEqual([item["frame"] for item in manifest["frames"]], [3, 4])
        self.assertEqual(len(list(output.iterdir())), 5)
        snapshot = builder.SceneSnapshot.from_json((output / "frame-4.scene.json").read_bytes())
        self.assertEqual(snapshot.source.frame_id, 4)
        before = (output / "sequence.manifest.json").read_bytes()
        duplicate = subprocess.run(args, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(duplicate.returncode, 0)
        self.assertEqual((output / "sequence.manifest.json").read_bytes(), before)
        changed = copy.deepcopy(self.records)
        next(row for row in changed if row.get("event") == "frame_begin" and row.get("frame") == 4)["device"] = "0x7777"
        write_trace(self.path, changed)
        invalid_output = root / "invalid-sequence"
        invalid = subprocess.run(args[:-1] + [str(invalid_output)], capture_output=True, text=True, timeout=10)
        self.assertNotEqual(invalid.returncode, 0)
        self.assertFalse(invalid_output.exists())


if __name__ == "__main__":
    unittest.main()
