import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import compare_live_light_diagnostics as compare  # noqa: E402
from tests.test_scene_ir import example  # noqa: E402


def fixture():
    scene = example()
    scene["lights"][0]["id"] = "light:SouthQuarter1_int1:" + "a" * 64 + ":Light__15"
    return compare.SceneSnapshot.from_dict(scene), scene


def frame():
    light_id = "light-actor:SouthQuarter1_int1:" + "a" * 64 + ":Light:16"
    raw = {"id": light_id, "generation": 8,
           "field_24_bits": [0, 0, 0], "field_34_bits": [0x3F800000, 0x40000000, 0x40400000],
           "field_68_bits": [0, 0, 0]}
    raw.update({field: 0 for field in compare.RAW_SCALARS})
    detail = {"id": light_id, "generation": 8, "binding": "candidate", "position_evidence": "candidate",
              "position": [1, 2, 3]}
    view = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -4, 0, 0, 1]
    return {"event": "frame", "source": "synthetic", "session_id": "fixture-session", "frame": 7,
            "map": "SouthQuarter1_int1", "map_sha256": "a" * 64, "map_epoch": 1, "device_epoch": 1,
            "continuity": 1, "render_pass": 0, "stage": 1,
            "view": view, "projection": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
            "viewport": {"x": 0, "y": 0, "width": 640, "height": 480, "min_depth": 0, "max_depth": 0.5},
            "light_coverage": "partial", "visual_lights": 1, "light_details_truncated": False,
            "light_details": [detail], "light_raw_available": True,
            "light_raw": {"listed": 1, "skipped": 0, "truncated": False, "items": [raw]}}


class CompareLiveLightDiagnosticsTests(unittest.TestCase):
    def test_candidate_comparison_retains_synthetic_and_offline_provenance(self):
        scene, _ = fixture()
        record = frame()
        report = compare.compare(record, scene, "b" * 64, "c" * 64)
        light = report["lights"][0]
        self.assertEqual(report["observation"]["source"], "synthetic")
        self.assertEqual(report["offline_fixture"]["source"], "offline")
        self.assertEqual(report["camera_position_candidate"], [4, 0, 0])
        self.assertEqual(light["identity"], "candidate_name_match")
        self.assertEqual(light["distance_t3_units"], 0)
        self.assertEqual(light["runtime_position_candidate"], [1, 2, 3])
        self.assertIn("not independent proof", report["limitations"][0])
        self.assertNotIn("field_80_bits", light["raw_words"])

    def test_rejects_mismatch_or_unattested_frame(self):
        scene, _ = fixture()
        for mutate in (
            lambda r: r.update(light_raw_available=False),
            lambda r: r.update(map_sha256="b" * 64),
            lambda r: r["light_raw"]["items"][0]["field_34_bits"].__setitem__(0, 0x40000000),
            lambda r: r["light_raw"]["items"][0].update(generation=9),
            lambda r: r["light_raw"]["items"][0].update(engine_pointer=1),
            lambda r: r["light_raw"]["items"][0].update(id=[]),
            lambda r: r["light_details"][0].update(binding="verified"),
            lambda r: r["viewport"].update(width=0),
        ):
            record = frame()
            mutate(record)
            with self.assertRaises(ValueError):
                compare.compare(record, scene, "b" * 64, "c" * 64)
        bad_scene, _ = fixture()
        object.__setattr__(bad_scene.source, "kind", "runtime")
        with self.assertRaises(ValueError):
            compare.compare(frame(), bad_scene, "b" * 64, "c" * 64)
        bad_scene, _ = fixture()
        object.__setattr__(bad_scene.conventions, "length_unit", "meter")
        with self.assertRaises(ValueError):
            compare.compare(frame(), bad_scene, "b" * 64, "c" * 64)

    def test_ambiguous_fname_is_not_matched(self):
        scene, _ = fixture()
        record = frame()
        other_id = "light-actor:SouthQuarter1_int1:" + "a" * 64 + ":Light__15:0"
        detail = copy.deepcopy(record["light_details"][0]); detail["id"] = other_id
        raw = copy.deepcopy(record["light_raw"]["items"][0]); raw["id"] = other_id
        record["light_details"].append(detail);record["light_raw"]["items"].append(raw)
        record["visual_lights"] = record["light_raw"]["listed"] = 2
        lights = compare.compare(record, scene, "b" * 64, "c" * 64)["lights"]
        self.assertEqual([light["identity"] for light in lights], ["ambiguous", "ambiguous"])
        self.assertTrue(all(light["offline_id_candidate"] is None for light in lights))

    def test_exact_session_frame_and_explicit_synthetic_fixture(self):
        data = json.dumps(frame()).encode() + b"\n"
        with self.assertRaises(ValueError):
            compare.select_frame(data, "fixture-session", 7)
        self.assertEqual(compare.select_frame(data, "fixture-session", 7, True)["source"], "synthetic")
        with self.assertRaises(ValueError):
            compare.select_frame(data + data, "fixture-session", 7, True)
        with self.assertRaises(ValueError):
            compare.select_frame(data, "other-session", 7, True)
        with self.assertRaises(ValueError):
            compare.strict_json(b'{"event":"frame","event":"frame"}')
        live = frame();live["source"] = "live_process";live["session_id"] = "not-a-live-session"
        with self.assertRaises(ValueError):
            compare.select_frame(json.dumps(live).encode(), "not-a-live-session", 7)

    def test_invalid_float_stays_unknown_and_bit_sign_is_checked(self):
        scene, _ = fixture()
        record = frame();record["light_raw"]["items"][0]["field_34_bits"][0] = 0x7FC00000
        record["light_details"][0].update(position=None, position_evidence="unknown")
        light = compare.compare(record, scene, "b" * 64, "c" * 64)["lights"][0]
        self.assertIsNone(light["runtime_position_candidate"])
        self.assertIsNone(light["distance_t3_units"])
        record = frame();record["light_raw"]["items"][0]["field_34_bits"][0] = 0
        record["light_details"][0]["position"][0] = -0.0
        with self.assertRaises(ValueError):
            compare.compare(record, scene, "b" * 64, "c" * 64)

    def test_cli_creates_report_once(self):
        _, scene = fixture()
        with tempfile.TemporaryDirectory(prefix="t3light-compare-") as directory:
            directory = Path(directory);log = directory / "viewer.jsonl";offline = directory / "offline.json"
            report = directory / "report.json"
            log.write_text(json.dumps(frame()) + "\n");offline.write_text(json.dumps(scene))
            cmd = [sys.executable, "-B", str(ROOT / "tools/compare_live_light_diagnostics.py"), str(log), str(offline),
                   "--session", "fixture-session", "--frame", "7", "--synthetic-fixture", "--output", str(report)]
            subprocess.run(cmd, check=True, capture_output=True, text=True)
            result = json.loads(report.read_text())
            self.assertEqual(result["lights"][0]["identity"], "candidate_name_match")
            self.assertNotEqual(subprocess.run(cmd, capture_output=True).returncode, 0)
            self.assertEqual(json.loads(report.read_text()), result)


if __name__ == "__main__":
    unittest.main()
