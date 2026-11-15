"""Exercise the offline SceneIR preview without the game or GPU."""
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
from test_scene_ir import example, IDENTITY  # noqa: E402
import scene_ir_to_d3d12_packet as preview  # noqa: E402
import capture_to_camera_observation as camera_capture  # noqa: E402
import scene_ir_attach_camera as attach_camera  # noqa: E402


def camera_data():
    return {"schema": "t3.camera.observation", "version": 1, "source": {"kind": "synthetic"},
            "camera": {"source": {"kind": "offline", "frame_id": None, "fingerprint": None},
                       "view": IDENTITY, "projection": IDENTITY,
                       "viewport": {"x": 0, "y": 0, "width": 256, "height": 256,
                                    "min_depth": 0, "max_depth": 1}, "near": None, "far": None}}


class SceneIRPreviewTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("a host C++ compiler is required")
        cls.temp = tempfile.TemporaryDirectory(prefix="t3render-scene-ir-preview-")
        cls.parser = Path(cls.temp.name) / "parser"
        source = ('#define T3RENDER_REPLAY_PARSER_TEST\n#include "d3d12_replay_up.cpp"\n'
                  '#include <iostream>\n#include <iterator>\n'
                  'int main(){std::string s((std::istreambuf_iterator<char>(std::cin)),{}); '
                  'Frame f{}; if(!ParseFrameBytes(s.data(),s.size(),f)) return 2; '
                  'std::cout << f.scene.size() << " " << f.scene[0].vertices[0].x << " " '
                  '<< f.scene[1].vertices[0].x << "\\n";}\n')
        result = subprocess.run([compiler, "-std=c++17", "-I", str(HERE), "-x", "c++", "-o", str(cls.parser), "-"],
                                input=source, capture_output=True, text=True, timeout=60)
        if result.returncode:
            cls.temp.cleanup()
            raise RuntimeError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_offline_scene_geometry_and_transform_reach_existing_dx12_parser(self):
        scene = preview.SceneSnapshot.from_dict(example())
        camera, _ = preview.load_camera(json.dumps(camera_data()))
        packet, info = preview.create_packet(scene, camera, ["actor:bed"], [0])
        self.assertEqual(info["triangles"], 2)
        self.assertEqual([s["kind"] for s in info["sources"]], ["mesh_instance", "bsp_block"])
        self.assertEqual(len(packet.splitlines()[8].split()), 4)  # XYZ plus color, not XYZW
        result = subprocess.run([str(self.parser)], input=packet, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "2 3398 0\n")

    def test_fail_closed_for_ambiguous_selection_conventions_and_camera(self):
        data = example()
        camera, _ = preview.load_camera(json.dumps(camera_data()))
        for instances, blocks in (([], []), (["actor:bed", "actor:bed"], []), (["missing"], []), ([], [4])):
            with self.subTest(instances=instances, blocks=blocks), self.assertRaises(ValueError):
                preview.create_packet(preview.SceneSnapshot.from_dict(data), camera, instances, blocks)
        data["mesh_instances"][0]["transform"][3] = 1
        with self.assertRaisesRegex(ValueError, "affine"):
            preview.create_packet(preview.SceneSnapshot.from_dict(data), camera, ["actor:bed"], [])
        data = example()
        data["conventions"]["clip_depth"] = "minus_one_to_one"
        with self.assertRaisesRegex(ValueError, "clip convention"):
            preview.create_packet(preview.SceneSnapshot.from_dict(data), camera, ["actor:bed"], [])
        observed = camera_data()
        observed["camera"]["source"] = {"kind": "runtime", "frame_id": 7, "fingerprint": "a" * 64}
        observed["source"] = {"kind": "captured_draw", "frame_id": 8, "operation_id": 1,
                              "trace_sha256": "a" * 64}
        with self.assertRaisesRegex(ValueError, "provenance disagree"):
            preview.load_camera(json.dumps(observed))

    def test_explicit_material_exclusion_is_reported(self):
        data = example()
        data["materials"][0]["name"] = "BF"
        scene = preview.SceneSnapshot.from_dict(data)
        camera, _ = preview.load_camera(json.dumps(camera_data()))
        packet, info = preview.create_packet(scene, camera, ["actor:bed"], [0], ["bf"])
        self.assertEqual(info["triangles"], 1)
        self.assertEqual(info["excluded_sections"][0]["material"], "BF")
        self.assertEqual(len(packet.splitlines()[8].split()), 4)

    def test_geometry_without_sections_is_not_silently_dropped(self):
        data = example()
        data["static_mesh_resources"][0]["geometry"]["sections"] = []
        data["mesh_instances"][0]["material_ids"] = []
        camera, _ = preview.load_camera(json.dumps(camera_data()))
        packet, info = preview.create_packet(preview.SceneSnapshot.from_dict(data), camera, ["actor:bed"], [0])
        self.assertEqual(info["triangles"], 2)
        self.assertIsNone(info["sources"][0]["section"])
        self.assertIsNone(info["sources"][0]["material"])
        parsed = subprocess.run([str(self.parser)], input=packet, capture_output=True, text=True, timeout=10)
        self.assertEqual(parsed.returncode, 0, parsed.stderr)
        self.assertEqual(parsed.stdout, "2 3398 0\n")

    def test_material_exclusion_uses_sdk_lowercase_without_unicode_casefold(self):
        data = example()
        sections = data["static_mesh_resources"][0]["geometry"]["sections"]
        sections.append(copy.deepcopy(sections[0]))
        data["mesh_instances"][0]["material_ids"] = ["material:wood", "material:street"]
        data["materials"] = [{"id": "material:wood", "name": "Straße"},
                             {"id": "material:street", "name": "Strasse"}]
        camera, _ = preview.load_camera(json.dumps(camera_data()))
        packet, info = preview.create_packet(preview.SceneSnapshot.from_dict(data), camera,
                                             ["actor:bed"], [], ["strasse"])
        self.assertEqual(info["triangles"], 1)
        self.assertEqual(info["sources"][0]["material"], "Straße")
        self.assertEqual(info["excluded_sections"][0]["material"], "Strasse")

    def test_camera_extraction_keeps_frame_provenance_and_rejects_failed_present(self):
        viewport = {"x": 0, "y": 0, "width": 256, "height": 256, "min_z": 0, "max_z": 0.5}
        draw = {"event": "draw", "frame": 7, "op_id": 8,
                "transforms": {"view": IDENTITY, "projection": IDENTITY}, "viewport": viewport}
        result = {"event": "draw_result", "frame": 7, "op_id": 8, "hr": 0}
        end = {"event": "frame_end", "frame": 7, "present_hr": 0,
               "had_reset": False, "concurrent_operations": False}
        records = [draw, result, end]
        with mock.patch.object(camera_capture.trace, "read_selected", return_value=(records, "a" * 64)):
            observation = camera_capture.extract("unused", 7, 8)
            parsed, source = preview.load_camera(json.dumps(observation))
            self.assertEqual((source["frame_id"], source["operation_id"]), (7, 8))
            self.assertEqual(parsed.source.kind, "runtime")
            self.assertEqual(parsed.viewport.max_depth, 0.5)
            original = example()
            original["source"]["limitations"] = [
                "Offline map export: no runtime visibility, save-game changes, frame or camera are represented."]
            attached = attach_camera.attach(preview.SceneSnapshot.from_dict(original),
                                            json.dumps(observation).encode())
            self.assertEqual(attached.source.kind, "offline")
            self.assertIsNone(attached.source.frame_id)
            self.assertIs(attached.source.camera_synchronized, False)
            self.assertEqual(attached.camera.source.frame_id, 7)
            self.assertEqual(attached.camera.source.fingerprint, "a" * 64)
            self.assertIn("draw operation 8", attached.source.limitations[-1])
            self.assertIn("Offline map export: no runtime visibility, save-game changes or runtime frame are represented.",
                          attached.source.limitations)
            self.assertFalse(any("frame or camera are represented" in text for text in attached.source.limitations))
            packet, info = preview.create_packet(attached, attached.camera, ["actor:bed"], [0])
            self.assertEqual(info["triangles"], 2)
        bad = copy.deepcopy(records)
        bad[-1]["present_hr"] = -1
        with mock.patch.object(camera_capture.trace, "read_selected", return_value=(bad, "a" * 64)):
            with self.assertRaisesRegex(ValueError, "Present"):
                camera_capture.extract("unused", 7, 8)

    def test_cli_uses_embedded_camera_without_claiming_runtime_scene(self):
        scene = attach_camera.attach(preview.SceneSnapshot.from_dict(example()),
                                     json.dumps(camera_data()).encode())
        with tempfile.TemporaryDirectory(prefix="t3render-scene-preview-cli-") as directory:
            root = Path(directory)
            scene_path, output = root / "scene.json", root / "packet.txt"
            scene_path.write_text(scene.to_json(), encoding="utf-8")
            cmd = [sys.executable, str(HERE.parent / "tools" / "scene_ir_to_d3d12_packet.py"),
                   str(scene_path), "--instance", "actor:bed", "--bsp", "0", "--output", str(output)]
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = json.loads(Path(str(output) + ".manifest.json").read_text())
            self.assertEqual(manifest["kind"], "offline_scene_ir_preview_not_runtime_frame")
            self.assertFalse(manifest["camera_synchronized_to_scene"])
            self.assertEqual(manifest["camera_source"]["kind"], "scene_ir_camera")
            self.assertEqual(manifest["clip_depth_assumption"], "zero_to_one")
            replay = subprocess.run([str(self.parser)], input=output.read_text(), capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(replay.returncode, 0, replay.stderr)
            self.assertEqual(replay.stdout, "2 3398 0\n")
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_text().splitlines()[0], "T3R_RECONSTRUCTION_XYZ_V1")

    def test_manifest_collision_removes_only_new_packet(self):
        with tempfile.TemporaryDirectory(prefix="t3render-scene-preview-atomic-") as directory:
            output = Path(directory) / "packet.txt"
            sidecar = Path(str(output) + ".manifest.json")
            sidecar.write_text("existing", encoding="utf-8")
            with self.assertRaises(FileExistsError):
                preview.write_pair(output, "packet", {"value": 1})
            self.assertFalse(output.exists())
            self.assertEqual(sidecar.read_text(), "existing")


if __name__ == "__main__":
    unittest.main()
