"""Run the real capture writer against small POSIX/D3D test doubles.

This validates host-side behavior, not the Windows ABI, DLL hook, or TDS.
"""
import importlib.util
import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("trace_summary", ROOT / "tools" / "trace_summary.py")
trace_summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace_summary)


class CaptureHostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++")
        if not compiler or os.name != "posix":
            raise unittest.SkipTest("POSIX and clang++ are required for the capture host test")
        cls.build = tempfile.TemporaryDirectory(prefix="t3render-host-test-")
        cls.binary = Path(cls.build.name) / "capture_fake"
        result = subprocess.run([
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I", str(ROOT / "tests" / "fakes"), "-I", str(ROOT / "src"),
            str(ROOT / "src" / "capture.cpp"), str(ROOT / "tests" / "capture_fake.cpp"),
            "-o", str(cls.binary),
        ], text=True, capture_output=True)
        if result.returncode:
            cls.build.cleanup()
            raise RuntimeError("host capture compilation failed:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-trace-test-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "trace.jsonl"

    def run_capture(self, scenario="frame", *, frame=None, key=None, extra=None, path=True):
        env = {k: v for k, v in os.environ.items() if not k.startswith(("T3RENDER_", "T3_TEST_"))}
        if path:
            env["T3RENDER_CAPTURE_PATH"] = str(self.path)
        if frame is not None:
            env["T3RENDER_CAPTURE_FRAME"] = frame
        if key is not None:
            env["T3RENDER_CAPTURE_KEY"] = key
        if extra:
            env.update(extra)
        result = subprocess.run([str(self.binary), scenario], env=env, text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("state=unchanged", result.stdout)
        return result.stdout

    def records(self):
        return [json.loads(line) for line in self.path.read_text(encoding="utf-8").splitlines()]

    def test_bridge_same_present_interval_same_thread_is_recorded(self):
        self.run_capture("bridge_good", frame="1", extra={"T3RENDER_SCENE_BRIDGE": "1"})
        records = self.records()
        self.assertEqual(records[0]["scene_bridge"]["install_status"], "installed")
        self.assertEqual([r["event"] for r in records], ["header", "frame_begin", "scene_bridge_observation",
                         "draw", "draw_result", "scene_bridge_frame", "frame_end", "capture_end"])
        observation = records[2]
        self.assertEqual(observation["frame"], 1)
        self.assertEqual(observation["pass"], 2)
        self.assertEqual(observation["entries"][0]["owner_name"], 'synthetic"actor')
        frame = records[-3]
        self.assertEqual(frame["status"], "synchronized")
        self.assertTrue(frame["same_thread"])
        self.assertEqual((frame["started"], frame["completed"], frame["pending"]), (1, 1, 0))
        self.assertEqual(trace_summary.summarize(self.path)["frame_count"], 1)

    def test_bridge_and_screenshot_share_one_frame_and_present(self):
        self.run_capture("bridge_image_good", frame="1", extra={"T3RENDER_SCENE_BRIDGE": "1",
                                                                 "T3RENDER_CAPTURE_SCREENSHOTS": "1"})
        records = self.records()
        self.assertEqual([r["event"] for r in records], ["header", "frame_begin", "scene_bridge_observation",
                         "draw", "draw_result", "frame_image", "scene_bridge_frame", "frame_end", "capture_end"])
        self.assertTrue(all(r.get("frame") == 1 for r in records[1:-1]))
        self.assertEqual(records[-3]["status"], "synchronized")
        self.assertEqual(trace_summary.summarize(self.path)["frame_count"], 1)

    def test_bridge_ambiguous_windows_never_claim_synchronization(self):
        for scenario, frame in (("bridge_overlap_present", "1"), ("bridge_unfinished", "1"),
                                ("bridge_overlap_draw", "1"), ("bridge_thread_change", "1"),
                                ("bridge_birth_change", "1"), ("bridge_missing_pre", "0"),
                                ("bridge_present_draw", "1"), ("bridge_reset_pending", "1")):
            with self.subTest(scenario=scenario):
                self.path.unlink(missing_ok=True)
                self.run_capture(scenario, frame=frame, extra={"T3RENDER_SCENE_BRIDGE": "1"})
                event = next(r for r in self.records() if r["event"] == "scene_bridge_frame")
                self.assertEqual(event["status"], "unknown")
                self.assertEqual(trace_summary.summarize(self.path)["frame_count"], 1)

    def test_bridge_replacement_device_requires_its_own_previous_present(self):
        self.run_capture("bridge_device_replace", frame="1", extra={"T3RENDER_SCENE_BRIDGE": "1"})
        event = next(r for r in self.records() if r["event"] == "scene_bridge_frame")
        self.assertEqual(event["status"], "synchronized")
        self.assertTrue(event["previous_present_observed"])
        self.assertEqual(trace_summary.summarize(self.path)["frame_count"], 1)

    def test_bridge_capacity_limit_marks_frame_unknown(self):
        self.run_capture("bridge_capacity", frame="1", extra={"T3RENDER_SCENE_BRIDGE": "1"})
        records = self.records()
        self.assertEqual(records[0]["scene_bridge"]["probe_limit"], 16)
        self.assertEqual(sum(r["event"] == "scene_bridge_observation" for r in records), 16)
        frame = next(r for r in records if r["event"] == "scene_bridge_frame")
        self.assertEqual((frame["status"], frame["started"], frame["completed"], frame["dropped"]),
                         ("unknown", 16, 16, 1))
        self.assertEqual(trace_summary.summarize(self.path)["frame_count"], 1)

    def test_bridge_install_failure_is_visible(self):
        self.run_capture("bridge_install_failure", frame="1", extra={"T3RENDER_SCENE_BRIDGE": "1"})
        records = self.records()
        self.assertEqual(records[0]["scene_bridge"]["install_status"], "prologue_mismatch")
        self.assertEqual(next(r for r in records if r["event"] == "scene_bridge_frame")["status"], "not_installed")
        self.assertEqual(trace_summary.summarize(self.path)["frame_count"], 1)

    def test_bridge_reentrant_driver_query_is_dropped_without_deadlock(self):
        self.run_capture("bridge_good", frame="1", extra={"T3RENDER_SCENE_BRIDGE": "1",
                                                            "T3_TEST_BRIDGE_REENTER": "1"})
        event = next(r for r in self.records() if r["event"] == "scene_bridge_frame")
        self.assertEqual(event["status"], "unknown")
        self.assertTrue(event["operation_overlap"])
        self.assertGreater(event["dropped"], 0)
        self.assertEqual(trace_summary.summarize(self.path)["frame_count"], 1)

    def test_nested_probe_marks_only_its_present_interval_unknown(self):
        for scenario, expected in (("bridge_reentered", "unknown"), ("bridge_prior_reentry", "synchronized")):
            with self.subTest(scenario=scenario):
                self.path.unlink(missing_ok=True)
                self.run_capture(scenario, frame="1", extra={"T3RENDER_SCENE_BRIDGE": "1"})
                records = self.records()
                self.assertEqual(records[0]["scene_bridge"]["reentry_detection"], "saturating_counter_v1")
                event = next(r for r in records if r["event"] == "scene_bridge_frame")
                self.assertEqual(event["status"], expected)
                self.assertEqual(event["probe_reentered"], scenario == "bridge_reentered")
                self.assertEqual(event["probe_overlap"], scenario == "bridge_reentered")
                self.assertEqual(trace_summary.summarize(self.path)["frame_count"], 1)

    def test_bridge_summary_rejects_inconsistent_synchronization_claims(self):
        self.run_capture("bridge_good", frame="1", extra={"T3RENDER_SCENE_BRIDGE": "1"})
        original = self.records()
        for mutation in ("different_draw_thread", "pending_reset", "failed_present", "wrong_frame", "scan_limit",
                         "unreported_reentry", "missing_reentry"):
            with self.subTest(mutation=mutation):
                records = copy.deepcopy(original)
                draw = next(r for r in records if r["event"] == "draw")
                observation = next(r for r in records if r["event"] == "scene_bridge_observation")
                bridge = next(r for r in records if r["event"] == "scene_bridge_frame")
                end = next(r for r in records if r["event"] == "frame_end")
                if mutation == "different_draw_thread": draw["thread_id"] += 1
                elif mutation == "pending_reset": bridge["reset_pending"] = True
                elif mutation == "failed_present": end["present_hr"] = -1
                elif mutation == "wrong_frame": observation["frame"] += 1
                elif mutation == "scan_limit": observation["target"]["objects_scanned"] = 65537
                elif mutation == "unreported_reentry": bridge["probe_reentered"] = True
                elif mutation == "missing_reentry": del bridge["probe_reentered"]
                self.path.write_text("".join(json.dumps(r) + "\n" for r in records), encoding="utf-8")
                with self.assertRaises(trace_summary.TraceError):
                    trace_summary.summarize(self.path)

    def test_image_is_associated_once_with_each_selected_frame_before_present(self):
        self.run_capture("frame_image", frame="1", extra={"T3RENDER_CAPTURE_SCREENSHOTS": "1",
                                                            "T3RENDER_CAPTURE_FRAMES": "2"})
        records = self.records()
        images = [record for record in records if record["event"] == "frame_image"]
        self.assertEqual([record["frame"] for record in images], [1, 2])
        self.assertTrue(all(record["status"] == "captured" for record in images))
        for image in images:
            index = records.index(image)
            self.assertEqual(records[index + 1]["event"], "frame_end")
            self.assertEqual(records[index + 1]["frame"], image["frame"])
        self.assertEqual(trace_summary.summarize(self.path)["frame_count"], 2)

    def test_frame_trigger_records_actual_snapshots_and_balances_references(self):
        output = self.run_capture(frame="1")
        self.assertIn("configured=1", output)
        summary = trace_summary.summarize(self.path)
        self.assertEqual(summary["draw_count"], 4)
        self.assertEqual(summary["calls"], {
            "DrawIndexedPrimitive": 1, "DrawPrimitive": 1,
            "DrawIndexedPrimitiveUP": 1, "DrawPrimitiveUP": 1,
        })
        self.assertEqual(summary["frames"][0]["frame"], 1)
        self.assertEqual(summary["frames"][0]["present_hr"], -7)
        events = self.records()
        self.assertEqual([e["event"] for e in events],
                         ["header", "frame_begin", "draw", "draw_result", "draw", "draw_result",
                          "draw", "draw_result", "draw", "draw_result", "reset", "frame_end", "capture_end"])
        self.assertFalse(events[0]["replayable"])
        self.assertEqual(events[0]["operation_results"], 1)
        self.assertEqual(events[1]["presentation"]["width"], 1280)
        draw = events[2]
        self.assertEqual(draw["viewport"]["height"], 720)
        self.assertEqual(draw["transforms"]["view"][12], 12)
        self.assertIsNone(draw["transforms"]["world"][0])
        self.assertEqual(draw["render_states"], {"7": 42})
        self.assertEqual(draw["material"]["power"], 16)
        self.assertEqual(draw["lights"][0]["position"], [1, 2, 3])
        self.assertEqual(draw["stages"][0]["texture"]["width"], 64)
        self.assertEqual(draw["streams"][0]["stride"], 32)
        self.assertEqual(draw["indices"]["base_vertex_index"], 3)
        self.assertEqual(draw["vertex_shader"], 444)
        self.assertEqual(draw["pixel_shader"], 555)
        self.assertTrue(events[-2]["had_reset"])
        self.assertEqual(events[-2]["draws"], 4)

    def test_experimental_facade_is_explicitly_marked_partial(self):
        self.run_capture("experimental_header", frame="1")
        header = self.records()[0]
        self.assertFalse(header["replayable"])
        self.assertEqual(header["experimental_facade"], {"enabled": True, "identity_verified": False,
                                                         "concurrency_verified": False})

    def test_repeated_unlock_experiment_is_explicit_in_header(self):
        self.run_capture("experimental_repeated_header", frame="1")
        header = self.records()[0]
        self.assertFalse(header["replayable"])
        self.assertTrue(header["experimental_facade"]["repeated_unlock_resource_discard"])

    def test_post_repeat_recovery_is_explicit_in_header(self):
        self.run_capture("experimental_recovery_header", frame="1")
        header = self.records()[0]
        self.assertTrue(header["experimental_facade"]["repeated_unlock_resource_discard"])
        self.assertTrue(header["experimental_facade"]["repeated_unlock_post_write_recovery"])
        self.assertFalse(header["replayable"])

    def test_reentrant_device_operation_abandons_partial_frame(self):
        for scenario in ("shader_abandon", "shader_abandon_all"):
            with self.subTest(scenario=scenario):
                self.run_capture(scenario, frame="1", extra={"T3RENDER_CAPTURE_DECLARATIONS": "1"})
                self.assertEqual([item["event"] for item in self.records()], ["header", "frame_begin", "draw"])
                self.path.unlink()

    def test_pre_reset_history_requires_opt_in_and_valid_framing(self):
        self.run_capture("shader_history", frame="1", extra={"T3RENDER_CAPTURE_DECLARATIONS": "1"})
        records = self.records()
        self.assertNotIn("declaration_history", records[0])
        self.assertTrue(all("vertex_declaration_pre_reset" not in e for e in records if e["event"] == "draw"))
        self.path.unlink()
        self.run_capture("shader_history", frame="1", extra={"T3RENDER_CAPTURE_DECLARATIONS": "1",
                                                               "T3RENDER_CAPTURE_DECLARATION_HISTORY": "1"})
        records = self.records()
        self.assertEqual(records[0]["declaration_history"], {"requested": True, "words_per_observation": 1024,
                         "words_per_frame": 262144, "words_per_device": 8192})
        draws = [e for e in records if e["event"] == "draw"]
        self.assertEqual(["vertex_declaration_pre_reset" in e for e in draws], [True, True, False, False, False])
        self.assertEqual(draws[0]["vertex_declaration_pre_reset"], {
            "status": "hypothesis", "source": "application_memory_before_reset", "generation": 9,
            "reset_epoch": 3, "reset_hr": 0, "rebind_epoch": 4, "words": [0x20000000, 0xffffffff]})
        self.assertEqual(draws[0]["vertex_declaration"]["status"], "unavailable")
        self.assertIsNone(draws[0]["vertex_declaration"]["words"])
        results = {e["op_id"]: e for e in records if e["event"] == "draw_result"}
        self.assertFalse(results[draws[1]["op_id"]]["shader_stable"])
        converted = subprocess.run([sys.executable, "-B", str(ROOT / "tools" / "trace_to_ir.py"), str(self.path)],
                                   text=True, capture_output=True)
        self.assertEqual(converted.returncode, 0, converted.stderr)
        ir = json.loads(converted.stdout)
        self.assertFalse(ir["replayable"])
        commands = ir["frames"][0]["commands"]
        self.assertEqual(commands[0]["vertex_declaration_pre_reset"]["status"], "hypothesis")
        self.assertEqual(commands[0]["vertex_declaration_observed"]["status"], "unavailable")
        self.assertNotIn("vertex_declaration_pre_reset", commands[1])

    def test_pre_reset_history_frame_budget_resets_between_frames(self):
        self.run_capture("shader_history_budget", frame="1", extra={"T3RENDER_CAPTURE_DECLARATIONS": "1",
            "T3RENDER_CAPTURE_DECLARATION_HISTORY": "1", "T3RENDER_CAPTURE_FRAMES": "2"})
        draws = [e for e in self.records() if e["event"] == "draw"]
        self.assertEqual(len(draws), 258)
        self.assertTrue(all("vertex_declaration_pre_reset" in e for e in draws[:256]))
        self.assertNotIn("vertex_declaration_pre_reset", draws[256])
        self.assertIn("vertex_declaration_pre_reset", draws[257])

    def test_pre_reset_program_is_a_separate_paired_hypothesis(self):
        self.run_capture("shader_history_program", frame="1", extra={"T3RENDER_CAPTURE_DECLARATIONS": "1",
            "T3RENDER_CAPTURE_DECLARATION_HISTORY": "1", "T3RENDER_CAPTURE_VERTEX_PROGRAMS": "1"})
        records = self.records()
        draws = [e for e in records if e["event"] == "draw"]
        self.assertEqual(["vertex_program_pre_reset" in e for e in draws], [True, True, False, False, False])
        self.assertTrue(all("vertex_program" not in draw for draw in draws))
        converted = subprocess.run([sys.executable, "-B", str(ROOT / "tools" / "trace_to_ir.py"), str(self.path)],
                                   text=True, capture_output=True)
        self.assertEqual(converted.returncode, 0, converted.stderr)
        commands = json.loads(converted.stdout)["frames"][0]["commands"]
        self.assertEqual(commands[0]["vertex_program_pre_reset"]["status"], "hypothesis")
        self.assertTrue(all("vertex_program_pre_reset" not in command for command in commands[1:]))
        self.assertTrue(all("vertex_program_observed" not in command for command in commands))

    def test_shader_unavailable_reason_contains_no_partial_words(self):
        self.run_capture("shader_reason", frame="1", extra={"T3RENDER_CAPTURE_DECLARATIONS": "1"})
        records = self.records()
        draws = [item for item in records if item["event"] == "draw"]
        self.assertEqual(len(draws), 4)
        for draw in draws:
            self.assertEqual(draw["vertex_declaration"]["status"], "unavailable")
            self.assertIsNone(draw["vertex_declaration"]["words"])
        self.assertEqual((draws[0]["vertex_declaration"]["reason"], draws[0]["vertex_declaration"]["scan_status"]),
                         ("scan_incomplete", "null_address"))
        self.assertEqual((draws[1]["vertex_declaration"]["reason"], draws[1]["vertex_declaration"]["scan_status"]),
                         ("reset_attempt", "null_address"))
        self.assertEqual((draws[2]["vertex_declaration"]["reason"], draws[2]["vertex_declaration"]["scan_status"]),
                         ("observation_error", "complete"))
        self.assertNotIn("reason", draws[3]["vertex_declaration"])

    def test_combined_history_configuration_requires_both_flags(self):
        for declarations, history, expected in (("0", "0", "0"), ("1", "0", "0"),
                                                ("0", "1", "0"), ("1", "1", "1")):
            with self.subTest(declarations=declarations, history=history):
                output = self.run_capture("history_config", frame="1", extra={
                    "T3RENDER_CAPTURE_DECLARATIONS": declarations,
                    "T3RENDER_CAPTURE_DECLARATION_HISTORY": history})
                self.assertIn("history_requested=" + expected, output)
                self.path.unlink(missing_ok=True)

    def test_shader_candidate_is_paired_with_result_and_reuses_sampled_getter(self):
        self.run_capture("shader", frame="1", extra={"T3RENDER_CAPTURE_DECLARATIONS": "1"})
        records = self.records()
        self.assertTrue(records[0]["declaration_capture"]["requested"])
        draws = [item for item in records if item["event"] == "draw"]
        results = {item["op_id"]: item for item in records if item["event"] == "draw_result"}
        self.assertEqual(len(draws), 4)
        for draw in draws[:3]:
            self.assertEqual(draw["vertex_shader"], 0xf0000001)
            self.assertEqual(draw["vertex_declaration"], {
                "status": "candidate", "generation": 7, "words": [0x20000000, 0xffffffff]})
        self.assertTrue(results[draws[0]["op_id"]]["shader_stable"])
        self.assertFalse(results[draws[1]["op_id"]]["shader_stable"])
        self.assertFalse(results[draws[2]["op_id"]]["shader_stable"])
        self.assertFalse(results[draws[2]["op_id"]]["buffer_stable"])
        self.assertIsNone(draws[3]["vertex_shader"])
        self.assertEqual(draws[3]["vertex_declaration"], {
            "status": "unknown", "generation": None, "words": None})

    def test_vertex_program_requires_both_opt_ins_and_a_stable_result(self):
        self.run_capture("shader_program", frame="1", extra={"T3RENDER_CAPTURE_VERTEX_PROGRAMS": "1"})
        self.assertTrue(all("vertex_program" not in e for e in self.records() if e["event"] == "draw"))
        self.assertTrue(all("vertex_declaration" not in e for e in self.records() if e["event"] == "draw"))
        self.path.unlink()
        self.run_capture("shader_program", frame="1", extra={"T3RENDER_CAPTURE_VERTEX_PROGRAMS": "1",
                                                       "T3RENDER_CAPTURE_DECLARATIONS": "1"})
        records = self.records()
        self.assertEqual(records[0]["vertex_program_capture"], {"requested": True,
                         "words_per_observation": 1024, "words_per_frame": 262144})
        draws = [item for item in records if item["event"] == "draw"]
        self.assertEqual(draws[0]["vertex_program"], {"status": "candidate", "version": "vs_1_1",
                         "generation": 7, "words": [0xfffe0101, 1, 0xc00f0000, 0x90e40000, 0xffff]})
        self.assertNotIn("vertex_program", draws[3])
        converted = subprocess.run([sys.executable, "-B", str(ROOT / "tools" / "trace_to_ir.py"), str(self.path)],
                                   text=True, capture_output=True)
        self.assertEqual(converted.returncode, 0, converted.stderr)
        commands = json.loads(converted.stdout)["frames"][0]["commands"]
        self.assertEqual(commands[0]["vertex_program_observed"]["status"], "candidate")
        self.assertTrue(all("vertex_program_observed" not in command for command in commands[1:]))

    def test_vertex_program_budget_resets_between_frames(self):
        self.run_capture("shader_program_budget", frame="1", extra={"T3RENDER_CAPTURE_DECLARATIONS": "1",
            "T3RENDER_CAPTURE_VERTEX_PROGRAMS": "1", "T3RENDER_CAPTURE_FRAMES": "2"})
        draws = [e for e in self.records() if e["event"] == "draw"]
        self.assertEqual(len(draws), 258)
        self.assertTrue(all("vertex_program" in draw for draw in draws[:256]))
        self.assertEqual(draws[256]["vertex_program"], {"status": "unavailable", "generation": 9,
                         "reason": "frame_word_limit", "words": None})
        self.assertIn("vertex_program", draws[257])

    def test_vertex_program_unavailable_reason_contains_no_words(self):
        self.run_capture("shader_program_unavailable", frame="1", extra={"T3RENDER_CAPTURE_DECLARATIONS": "1",
                                                                         "T3RENDER_CAPTURE_VERTEX_PROGRAMS": "1"})
        draws = [e for e in self.records() if e["event"] == "draw"]
        self.assertEqual(draws[0]["vertex_program"], {"status": "unavailable", "generation": 7,
                         "reason": "unsupported_version", "words": None, "first_word_observed": 0xfffe0200})

    def test_vertex_program_10_is_serialized_and_converted(self):
        self.run_capture("shader_program_10", frame="1", extra={"T3RENDER_CAPTURE_DECLARATIONS": "1",
                                                                "T3RENDER_CAPTURE_VERTEX_PROGRAMS": "1"})
        draws = [e for e in self.records() if e["event"] == "draw"]
        self.assertEqual(draws[0]["vertex_program"]["version"], "vs_1_0")
        self.assertEqual(draws[0]["vertex_program"]["words"][0], 0xfffe0100)
        result = subprocess.run([sys.executable, "-B", str(ROOT / "tools" / "trace_to_ir.py"), str(self.path)],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        command = json.loads(result.stdout)["frames"][0]["commands"][0]
        self.assertEqual(command["vertex_program_observed"]["version"], "vs_1_0")

    def test_vertex_constants_require_opt_in_and_stable_paired_draw(self):
        self.run_capture("shader_constants", frame="1", extra={"T3RENDER_CAPTURE_DECLARATIONS": "1"})
        self.assertTrue(all("vertex_constants" not in e for e in self.records() if e["event"] == "draw"))
        self.path.unlink()
        self.run_capture("shader_constants", frame="1", extra={"T3RENDER_CAPTURE_DECLARATIONS": "1",
            "T3RENDER_CAPTURE_VERTEX_CONSTANTS": "1"})
        records = self.records()
        self.assertEqual(records[0]["vertex_constant_capture"], {"requested": True,
                         "registers_per_observation": 96, "words_per_frame": 262144})
        begin = next(e for e in records if e["event"] == "frame_begin")
        self.assertEqual(begin["caps"]["vertex_shader_constants"], 96)
        draws = [e for e in records if e["event"] == "draw"]
        self.assertEqual(draws[0]["vertex_constants"]["bits"], [0x3f800000+i for i in range(384)])
        converted = subprocess.run([sys.executable, "-B", str(ROOT / "tools" / "trace_to_ir.py"), str(self.path)],
                                   text=True, capture_output=True)
        self.assertEqual(converted.returncode, 0, converted.stderr)
        commands = json.loads(converted.stdout)["frames"][0]["commands"]
        self.assertEqual(commands[0]["vertex_constants_observed"]["status"], "sampled")
        self.assertTrue(all("vertex_constants_observed" not in command for command in commands[1:]))

    def test_vertex_constant_failures_never_emit_partial_bits(self):
        self.run_capture("shader_constants_unavailable", frame="1", extra={"T3RENDER_CAPTURE_DECLARATIONS": "1",
            "T3RENDER_CAPTURE_VERTEX_CONSTANTS": "1"})
        draws = [e for e in self.records() if e["event"] == "draw"]
        self.assertEqual([e["vertex_constants"]["reason"] for e in draws],
                         ["getter_failed", "verification_failed", "inconsistent_read", "caps_unavailable"])
        self.assertTrue(all(e["vertex_constants"]["bits"] is None for e in draws))

    def test_vertex_constant_budget_resets_between_frames(self):
        self.run_capture("shader_constants_budget", frame="1", extra={"T3RENDER_CAPTURE_DECLARATIONS": "1",
            "T3RENDER_CAPTURE_VERTEX_CONSTANTS": "1", "T3RENDER_CAPTURE_FRAMES": "2"})
        draws = [e for e in self.records() if e["event"] == "draw"]
        self.assertEqual(len(draws), 684)
        self.assertTrue(all(e["vertex_constants"]["status"] == "sampled" for e in draws[:682]))
        self.assertEqual(draws[682]["vertex_constants"]["reason"], "frame_word_limit")
        self.assertEqual(draws[683]["vertex_constants"]["status"], "sampled")

    def test_frame_window_defaults_to_one_and_preserves_legacy_events(self):
        self.run_capture("window", frame="1")
        records = self.records()
        self.assertEqual([e["event"] for e in records], ["header", "frame_begin", "clear", "draw",
                         "clear_result", "draw_result", "frame_end", "capture_end"])
        self.assertEqual(records[-1]["frames"], 1)
        self.assertEqual(records[0]["requested_frames"], 1)
        self.assertTrue(records[-2]["concurrent_operations"])
        self.assertEqual(records[-2]["present_hr"], -1)
        self.assertEqual(trace_summary.summarize(self.path)["frame_count"], 1)

    def test_frame_windows_preserve_order_and_reset_frame_local_state(self):
        for count in (2, 3):
            with self.subTest(count=count):
                self.run_capture("window", frame="1", extra={"T3RENDER_CAPTURE_FRAMES": str(count)})
                events = self.records()
                summary = trace_summary.summarize(self.path)
                self.assertEqual([f["frame"] for f in summary["frames"]], list(range(1, count + 1)))
                self.assertEqual(summary["draw_count"], count)
                self.assertEqual(events[-1], {"event": "capture_end", "frames": count})
                self.assertEqual(events[0]["requested_frames"], count)
                ends = [e for e in events if e["event"] == "frame_end"]
                self.assertEqual([e["draws"] for e in ends], [1] * count)
                self.assertEqual([e["concurrent_operations"] for e in ends], [True] + [False] * (count - 1))
                self.assertEqual([e["had_reset"] for e in ends], [False, True] + [False] * (count - 2))
                operations = [e["op_id"] for e in events if e["event"] in ("clear", "draw")]
                self.assertEqual(operations, list(range(1, count * 2 + 1)))
                if count == 3:
                    begins = [e for e in events if e["event"] == "frame_begin"]
                    self.assertEqual([e["presentation"]["width"] for e in begins], [1280, 1280, 1600])
                result = subprocess.run([sys.executable, "-B", str(ROOT / "tools" / "trace_to_ir.py"), str(self.path)],
                                        text=True, capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)
                ir = json.loads(result.stdout)
                self.assertEqual([f["frame"] for f in ir["frames"]], list(range(1, count + 1)))
                self.assertEqual([len(f["commands"]) for f in ir["frames"]], [1] * count)
                self.path.unlink()

    def test_f11_window_captures_consecutive_frames_without_retrigger(self):
        self.run_capture("window_key", key="F11", extra={"T3RENDER_CAPTURE_FRAMES": "2"})
        summary = trace_summary.summarize(self.path)
        self.assertEqual([f["frame"] for f in summary["frames"]], [2, 3])
        self.assertEqual(self.records()[-1]["frames"], 2)
        self.assertEqual(self.records()[0]["requested_frames"], 2)

    def test_file_trigger_captures_next_consecutive_frames(self):
        marker = Path(self.temp.name) / "capture.ready"
        self.run_capture("window_file", extra={"T3RENDER_CAPTURE_TRIGGER_PATH": str(marker),
                                               "T3RENDER_CAPTURE_FRAMES": "2"})
        self.assertTrue(marker.exists())  # the writer observes but does not remove it
        self.assertEqual([f["frame"] for f in trace_summary.summarize(self.path)["frames"]], [2, 3])
        self.assertEqual(self.records()[0]["trigger"], "file")
        self.assertEqual(self.records()[-1]["frames"], 2)

    def test_file_trigger_rejects_stale_missing_parent_and_conflicting_triggers(self):
        marker = Path(self.temp.name) / "capture.ready"
        marker.touch()
        for signal, frame in ((marker, None), (Path(self.temp.name) / "missing" / "marker", None),
                              (Path(self.temp.name) / "unused", "1"), (self.path, None)):
            with self.subTest(signal=signal, frame=frame):
                output = self.run_capture(frame=frame, extra={"T3RENDER_CAPTURE_TRIGGER_PATH": str(signal)})
                self.assertIn("configured=0 getters=0", output)
                self.assertFalse(self.path.exists())

    def test_file_trigger_does_not_transfer_to_replacement_device(self):
        for scenario in ("file_release_armed", "file_release_pending"):
            with self.subTest(scenario=scenario):
                marker = Path(self.temp.name) / "capture.ready"
                self.run_capture(scenario, extra={"T3RENDER_CAPTURE_TRIGGER_PATH": str(marker)})
                self.assertFalse(self.path.exists())
                marker.unlink()

    def test_frame_window_resets_up_and_buffer_budgets(self):
        self.run_capture("window_budget", frame="1", extra={"T3RENDER_CAPTURE_FRAMES": "2"})
        draws = [event for event in self.records() if event["event"] == "draw"]
        first = [draw for draw in draws if draw["frame"] == 1]
        second = [draw for draw in draws if draw["frame"] == 2]
        self.assertEqual(len(first), 66)
        self.assertEqual(len(second), 2)
        for field in ("up_data", "buffer_data"):
            current = [draw[field]["vertex"]["status"] for draw in first if field in draw]
            self.assertEqual(current, ["captured"] * 32 + ["frame_limit"])
            self.assertEqual(next(draw[field]["vertex"]["status"] for draw in second if field in draw), "captured")
        self.assertEqual(trace_summary.summarize(self.path)["frame_count"], 2)

    def test_window_release_and_pending_operations_remain_incomplete(self):
        for scenario, expected in (("window_release", "missing capture_end"),
                                   ("window_pending", "frame 2 has no frame_end")):
            with self.subTest(scenario=scenario):
                self.run_capture(scenario, frame="1", extra={"T3RENDER_CAPTURE_FRAMES": "2"})
                events = self.records()
                self.assertEqual(events[0]["requested_frames"], 2)
                self.assertEqual(len([e for e in events if e["event"] == "frame_end"]), 1)
                self.assertFalse(any(e["event"] == "capture_end" for e in events))
                with self.assertRaisesRegex(trace_summary.TraceError, expected):
                    trace_summary.summarize(self.path)
                self.path.unlink()

    def test_invalid_frame_window_fails_closed(self):
        for value in ("", "0", "4", "-1", "+1", "01", "1 ", "abc", "99"):
            with self.subTest(value=value):
                output = self.run_capture(frame="1", extra={"T3RENDER_CAPTURE_FRAMES": value})
                self.assertIn("configured=0 getters=0", output)
                self.assertFalse(self.path.exists())
        output = self.run_capture(frame="18446744073709551614", extra={"T3RENDER_CAPTURE_FRAMES": "2"})
        self.assertIn("configured=0 getters=0", output)
        self.assertFalse(self.path.exists())

    def test_only_sdk_defined_observable_state_ids_are_queried(self):
        # The fake deliberately accepts *every* numeric ID, like a permissive
        # driver, so this verifies the writer's filtering rather than the fake's.
        self.run_capture(frame="1", extra={"T3_TEST_ALL_STATES": "1"})
        draw = next(event for event in self.records() if event["event"] == "draw")
        render_ids = {
            7, 8, 9, 10, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 30,
            34, 35, 36, 37, 38, 40, 47, 48, 52, 53, 54, 55, 56, 57, 58, 59, 60,
            *range(128, 138), *range(139, 144), *range(145, 149), *range(151, 165),
            *range(166, 169), *range(170, 174),
        }
        self.assertEqual({int(key) for key in draw["render_states"]}, render_ids)
        for key, value in draw["render_states"].items():
            self.assertEqual(value, int(key))
        stage_ids = {*range(1, 12), *range(13, 29)}
        for stage in draw["stages"]:
            self.assertEqual({int(key) for key in stage["states"]}, stage_ids)
            for key, value in stage["states"].items():
                self.assertEqual(value, stage["stage"] * 1000 + int(key))

    def test_prepared_bindings_serialize_buffers_and_shader_together(self):
        self.run_capture("prepared_buffers", frame="1", extra={
            "T3RENDER_CAPTURE_DECLARATIONS": "1", "T3RENDER_CAPTURE_VERTEX_PROGRAMS": "1",
            "T3RENDER_CAPTURE_VERTEX_CONSTANTS": "1"})
        events = self.records()
        draws = [event for event in events if event["event"] == "draw"]
        results = {event["op_id"]: event for event in events if event["event"] == "draw_result"}
        self.assertEqual(len(draws), 4)
        first = draws[0]
        self.assertEqual(bytes.fromhex(first["buffer_data"]["vertex"]["data"]), bytes(range(160, 256)))
        self.assertEqual(bytes.fromhex(first["buffer_data"]["index"]["data"]), b"\x02\x00\x03\x00\x04\x00")
        self.assertEqual(first["vertex_declaration"]["words"], [0x20000000, 0xffffffff])
        self.assertEqual(first["vertex_program"]["words"], [0xfffe0101, 1, 0xc00f0000, 0x90e40000, 0xffff])
        self.assertEqual(first["vertex_constants"]["bits"], [0x3f800000, 0, 0, 0])
        self.assertTrue(results[first["op_id"]]["shader_stable"])
        self.assertTrue(results[first["op_id"]]["buffer_stable"])
        for draw in draws[1:3]:
            for component in ("vertex", "index"):
                self.assertEqual(draw["buffer_data"][component]["status"], "getter_failed")
                self.assertIsNone(draw["buffer_data"][component]["data"])
        self.assertEqual(draws[3]["buffer_data"]["window_failure"],
                         {"status": "device_unsafe", "cause": "unlock_mismatch", "source_line": 123,
                          "unlock": {"resource": "vertex", "pending_lock": False, "locked": False,
                                     "pending_unlock": False, "operation_exhausted": False,
                                     "last_lock_hr": -1, "unlock_hr_candidate": 0},
                          "lock": {"resource": "vertex", "registry_result": "invalid_range", "recovery_attempted": True,
                                   "offset": 1, "requested_bytes": 0, "normalized_bytes": 0, "buffer_size": 8, "flags": 0}})
        self.assertEqual(draws[3]["buffer_data"]["repeated_unlocks_completed"], 2)
        self.assertFalse(results[draws[3]["op_id"]]["buffer_stable"])
        for component in ("vertex", "index"):
            self.assertEqual(draws[3]["buffer_data"][component]["status"], "window_unavailable")
            self.assertIsNone(draws[3]["buffer_data"][component]["data"])

    def test_buffered_observations_preserve_exact_ranges_and_result_stability(self):
        self.run_capture("buffered", frame="1")
        events = self.records()
        draws = [event for event in events if event["event"] == "draw"]
        results = {event["op_id"]: event for event in events if event["event"] == "draw_result"}
        self.assertEqual(len(draws), 21)
        self.assertEqual(trace_summary.summarize(self.path)["draw_count"], len(draws))
        nonindexed, indexed = draws[:2]
        self.assertEqual(nonindexed["buffer_data"]["source"], "application_lock_shadow")
        vertex = nonindexed["buffer_data"]["vertex"]
        self.assertEqual((vertex["status"], vertex["stream"], vertex["first_vertex"],
                          vertex["vertex_count"], vertex["stride"], vertex["byte_offset"], vertex["byte_count"]),
                         ("captured", 0, 2, 3, 32, 64, 96))
        self.assertEqual(bytes.fromhex(vertex["data"]), bytes(range(64, 160)))
        self.assertEqual(vertex["resource"], nonindexed["streams"][0]["buffer"])
        self.assertIsNone(nonindexed["buffer_data"]["index"])
        self.assertTrue(results[nonindexed["op_id"]]["buffer_stable"])

        vertex = indexed["buffer_data"]["vertex"]
        index = indexed["buffer_data"]["index"]
        self.assertEqual((vertex["status"], vertex["first_vertex"], vertex["vertex_count"],
                          vertex["byte_offset"], vertex["byte_count"]), ("captured", 5, 3, 160, 96))
        self.assertEqual(bytes.fromhex(vertex["data"]), bytes(range(160, 256)))
        self.assertEqual((index["status"], index["index_count"], index["index_format"],
                          index["byte_offset"], index["byte_count"]), ("captured", 3, 101, 2, 6))
        self.assertEqual(bytes.fromhex(index["data"]), b"\x02\x00\x03\x00\x04\x00")
        self.assertEqual(index["resource"], indexed["indices"]["buffer"])
        self.assertFalse(results[indexed["op_id"]]["buffer_stable"])

        # Callback failure after a partial write cannot leak those bytes.
        self.assertEqual(draws[2]["buffer_data"]["vertex"]["status"], "unknown")
        self.assertIsNone(draws[2]["buffer_data"]["vertex"]["data"])
        self.assertEqual(draws[2]["buffer_data"]["index"]["status"], "captured")
        self.assertEqual(draws[3]["buffer_data"]["vertex"]["status"], "captured")
        self.assertEqual(draws[3]["buffer_data"]["index"]["status"], "unknown")
        self.assertIsNone(draws[3]["buffer_data"]["index"]["data"])
        self.assertEqual(draws[4]["buffer_data"]["vertex"]["status"], "window_unavailable")
        self.assertEqual(draws[4]["buffer_data"]["index"]["status"], "window_unavailable")
        self.assertFalse(results[draws[4]["op_id"]]["buffer_stable"])
        self.assertEqual(results[draws[5]["op_id"]]["hr"], -1)
        self.assertTrue(results[draws[5]["op_id"]]["buffer_stable"])
        self.assertEqual(draws[6]["buffer_data"]["vertex"]["status"], "unknown")
        self.assertNotIn("buffer_data", draws[7])
        self.assertNotIn("buffer_stable", results[draws[7]["op_id"]])
        self.assertNotIn("buffer_data", draws[8])  # UP ignores an observer
        self.assertNotIn("buffer_stable", results[draws[8]["op_id"]])

    def test_buffered_observations_fail_closed_for_invalid_bindings_and_ranges(self):
        self.run_capture("buffered", frame="1")
        draws = [event for event in self.records() if event["event"] == "draw"]
        def statuses(index):
            data = draws[index]["buffer_data"]
            return data["vertex"]["status"], None if data["index"] is None else data["index"]["status"]
        self.assertEqual(statuses(9), ("empty", None))
        self.assertEqual(draws[9]["buffer_data"]["vertex"]["data"], "")
        self.assertEqual(statuses(10), ("overflow", "captured"))
        self.assertEqual(statuses(11), ("getter_failed", "getter_failed"))
        self.assertEqual(statuses(12), ("unbound", "unbound"))
        self.assertEqual(statuses(13), ("descriptor_failed", "descriptor_failed"))
        self.assertEqual(statuses(14), ("invalid_stride", "captured"))
        self.assertEqual(statuses(15), ("out_of_bounds", "out_of_bounds"))
        self.assertEqual(statuses(16), ("captured", "unsupported_index_format"))
        self.assertEqual(statuses(17), ("captured", "unsupported_topology"))
        self.assertEqual(statuses(18), ("unknown_range", None))
        self.assertEqual(statuses(19), ("per_draw_limit", None))
        self.assertEqual(statuses(20), ("captured", "captured"))
        index32 = draws[20]["buffer_data"]["index"]
        self.assertEqual((index32["index_format"], index32["byte_offset"], index32["byte_count"]), (102, 4, 12))
        for draw in draws:
            if "buffer_data" not in draw: continue
            for component in draw["buffer_data"].values():
                if isinstance(component, dict) and component["status"] not in ("captured", "empty"):
                    self.assertIsNone(component["data"])

    def test_shadow_failure_reasons_do_not_publish_partial_bytes(self):
        self.run_capture("shadow_reason", frame="1", extra={"T3RENDER_CAPTURE_BUFFERS": "1"})
        draws = [item for item in self.records() if item["event"] == "draw"]
        self.assertEqual(len(draws), 3)
        for part, reason in (("vertex", "abandoned"), ("index", "unknown_data")):
            failed = draws[0]["buffer_data"][part]
            self.assertEqual((failed["status"], failed["shadow_reason"], failed["data"]), ("unknown", reason, None))
            repeated = draws[1]["buffer_data"][part]
            self.assertEqual((repeated["status"], repeated["shadow_reason"], repeated["data"]), ("unknown", reason, None))
            self.assertEqual(repeated.get("shadow_abandon_cause"), "repeated_unlock" if part == "vertex" else None)
            self.assertNotIn("shadow_abandon_cause", failed)
            captured = draws[2]["buffer_data"][part]
            self.assertEqual(captured["status"], "captured")
            self.assertNotIn("shadow_reason", captured)
            self.assertEqual(captured.get("shadow_observation"), "post_repeated_unlock_write" if part == "vertex" else None)

    def test_buffered_budget_counts_only_successful_shadow_bytes(self):
        self.run_capture("buffer_budget", frame="1")
        draws = [event for event in self.records() if event["event"] == "draw"]
        self.assertEqual(len(draws), 33)
        self.assertTrue(all(draw["buffer_data"]["vertex"]["status"] == "captured" for draw in draws[:32]))
        self.assertEqual(draws[32]["buffer_data"]["vertex"]["status"], "frame_limit")
        self.assertIsNone(draws[32]["buffer_data"]["vertex"]["data"])
        self.path.unlink()
        self.run_capture("buffer_budget_fail", frame="1")
        draws = [event for event in self.records() if event["event"] == "draw"]
        self.assertEqual(len(draws), 41)
        self.assertTrue(all(draw["buffer_data"]["vertex"]["status"] == "unknown" for draw in draws[:40]))
        self.assertEqual(draws[40]["buffer_data"]["vertex"]["status"], "captured")
        self.path.unlink()
        self.run_capture("buffer_budget_mixed", frame="1")
        draws = [event for event in self.records() if event["event"] == "draw"]
        self.assertEqual(len(draws), 33)
        self.assertEqual(draws[31]["buffer_data"]["vertex"]["byte_count"], 262136)
        self.assertEqual(draws[31]["buffer_data"]["index"]["status"], "captured")
        self.assertEqual(draws[32]["buffer_data"]["vertex"]["status"], "empty")
        self.assertEqual(draws[32]["buffer_data"]["index"]["status"], "frame_limit")
        self.assertIsNone(draws[32]["buffer_data"]["index"]["data"])

    def test_clear_before_draw_and_operation_results_are_ordered(self):
        self.run_capture("clear", frame="1")
        events = self.records()
        self.assertEqual(events[0]["operation_results"], 1)
        self.assertEqual(events[0]["clear_rect_limit"], 256)
        self.assertEqual([e["event"] for e in events[:6]],
                         ["header", "frame_begin", "clear", "clear_result", "draw", "draw_result"])
        self.assertEqual([e["op_id"] for e in events if e["event"] in ("clear", "draw")], list(range(1, 9)))
        self.assertEqual(events[2]["rects_status"], "none")
        self.assertEqual(events[2]["rects"], [])
        self.assertEqual((events[2]["flags"], events[2]["color"], events[2]["depth"], events[2]["stencil"]),
                         (7, 0x12345678, 0.75, 9))
        self.assertEqual(events[2]["depth_bits"], 0x3f400000)
        self.assertEqual(events[3]["op_id"], events[2]["op_id"])
        self.assertEqual(events[5]["op_id"], events[4]["op_id"])
        clears = [e for e in events if e["event"] == "clear"]
        results = [e for e in events if e["event"] == "clear_result"]
        self.assertEqual(clears[1]["rects"], [{"x1": -5, "y1": 2, "x2": 31, "y2": 42},
                                              {"x1": 50, "y1": 60, "x2": 70, "y2": 80}])
        self.assertEqual(results[1]["hr"], -1)
        self.assertEqual([e["rects_status"] for e in clears[2:]],
                         ["null_pointer", "inconsistent", "unreadable", "count_limit", "address_overflow"])
        self.assertTrue(all(e["rects"] is None for e in clears[2:]))
        self.assertFalse(events[-2]["concurrent_operations"])
        self.assertEqual(trace_summary.summarize(self.path)["draw_count"], 1)

    def test_clear_partial_copy_is_unknown(self):
        self.run_capture("clear", frame="1", extra={"T3_TEST_PARTIAL_READ": "success"})
        clears = [e for e in self.records() if e["event"] == "clear"]
        self.assertEqual(clears[1]["rects_status"], "unreadable")
        self.assertIsNone(clears[1]["rects"])

    def test_clear_only_frame_is_valid(self):
        self.run_capture("clear_only", frame="1")
        self.assertEqual([e["event"] for e in self.records()],
                         ["header", "frame_begin", "clear", "clear_result", "frame_end", "capture_end"])
        summary = trace_summary.summarize(self.path)
        self.assertEqual(summary["draw_count"], 0)
        self.assertEqual(summary["events"]["clear"], 1)

    def test_clear_nonfinite_depth_preserves_bits_without_invalid_json(self):
        self.run_capture("clear_nan", frame="1")
        clear = [e for e in self.records() if e["event"] == "clear"][1]
        self.assertIsNone(clear["depth"])
        self.assertEqual(clear["depth_bits"] & 0x7f800000, 0x7f800000)
        self.assertNotEqual(clear["depth_bits"] & 0x007fffff, 0)
        trace_summary.summarize(self.path)

    def test_pending_clear_or_draw_leaves_incomplete_trace(self):
        for scenario in ("clear_pending", "draw_pending"):
            with self.subTest(scenario=scenario):
                if self.path.exists(): self.path.unlink()
                self.run_capture(scenario, frame="1")
                events = self.records()
                self.assertEqual(events[-1]["event"], "capture_incomplete")
                self.assertEqual(events[-1]["reason"], "pending_operations")
                self.assertFalse(any(e["event"] in ("frame_end", "capture_end") for e in events))
                with self.assertRaisesRegex(trace_summary.TraceError, "no frame_end"):
                    trace_summary.summarize(self.path)

    def test_overlapping_operations_are_flagged(self):
        self.run_capture("overlap", frame="1")
        events = self.records()
        self.assertEqual([e["event"] for e in events[2:-2]],
                         ["clear", "clear_result", "clear", "draw", "clear_result", "draw_result"])
        self.assertTrue(events[-2]["concurrent_operations"])
        self.assertEqual(trace_summary.summarize(self.path)["draw_count"], 1)

    def test_f11_arms_the_next_frame(self):
        output = self.run_capture("key", key="F11")
        self.assertIn("configured=1", output)
        summary = trace_summary.summarize(self.path)
        self.assertEqual(summary["frames"][0]["frame"], 2)
        self.assertEqual(summary["calls"], {"DrawIndexedPrimitive": 1})
        self.assertEqual(self.records()[0]["trigger"], "F11")

    def test_up_geometry_snapshots_and_failure_states(self):
        self.run_capture("up", frame="1")
        events = self.records()
        draws = [event for event in events if event["event"] == "draw"]
        self.assertEqual(len(draws), 26)
        self.assertEqual(events[0]["up_capture"]["component_limit_bytes"], 256 * 1024)
        expected_counts = [2, 4, 3, 6, 4, 4]
        for draw, count in zip(draws[:6], expected_counts):
            vertex = draw["up_data"]["vertex"]
            self.assertEqual(vertex["status"], "captured")
            self.assertEqual(vertex["first_vertex"], 0)
            self.assertEqual(vertex["vertex_count"], count)
            self.assertEqual(vertex["byte_offset"], 0)
            self.assertEqual(bytes.fromhex(vertex["data"]), bytes(range(count * 4)))
            self.assertIsNone(draw["up_data"]["index"])
        v16 = draws[6]["up_data"]["vertex"]
        self.assertEqual((v16["first_vertex"], v16["vertex_count"], v16["byte_offset"]), (2, 3, 8))
        self.assertEqual(bytes.fromhex(v16["data"]), bytes(range(8, 20)))
        i16 = draws[6]["up_data"]["index"]
        self.assertEqual((i16["index_count"], i16["index_format"]), (3, 101))
        self.assertEqual(bytes.fromhex(i16["data"]), b"\x02\x00\x03\x00\x04\x00")
        v32 = draws[7]["up_data"]["vertex"]
        self.assertEqual((v32["first_vertex"], v32["vertex_count"], v32["byte_offset"]), (3, 3, 12))
        self.assertEqual(bytes.fromhex(v32["data"]), bytes(range(12, 24)))
        i32 = draws[7]["up_data"]["index"]
        self.assertEqual((i32["index_count"], i32["index_format"]), (3, 102))
        self.assertEqual(bytes.fromhex(i32["data"]), b"\x03\x00\x00\x00\x04\x00\x00\x00\x05\x00\x00\x00")
        self.assertEqual([d["up_data"]["vertex"]["status"] for d in draws[8:17]], [
            "null_pointer", "unreadable", "unreadable", "unreadable", "invalid_stride",
            "unsupported_topology", "overflow", "overflow", "per_draw_limit",
        ])
        for draw in draws[8:17]:
            self.assertIsNone(draw["up_data"]["vertex"]["data"])
        self.assertEqual(draws[17]["up_data"]["index"]["status"], "unsupported_index_format")
        self.assertEqual(draws[18]["up_data"]["vertex"]["status"], "empty")
        self.assertEqual(draws[18]["up_data"]["index"]["status"], "empty")
        self.assertEqual(draws[18]["up_data"]["vertex"]["data"], "")
        for draw, count in zip(draws[19:25], expected_counts):
            index = draw["up_data"]["index"]
            self.assertEqual(index["status"], "captured")
            self.assertEqual(index["index_count"], count)
            self.assertEqual(index["byte_count"], count * 2)
            self.assertEqual(len(bytes.fromhex(index["data"])), count * 2)
        self.assertEqual(bytes.fromhex(draws[25]["up_data"]["vertex"]["data"]),
                         bytes([254, 255, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9]))
        self.assertFalse(events[-2]["replayable"])

    def test_up_partial_kernel_copy_is_never_emitted(self):
        self.run_capture("up", frame="1", extra={"T3_TEST_PARTIAL_READ": "1"})
        draws = [event for event in self.records() if event["event"] == "draw"]
        for draw in draws[:8]:
            self.assertEqual(draw["up_data"]["vertex"]["status"], "unreadable")
            self.assertIsNone(draw["up_data"]["vertex"]["data"])
        for draw in draws[6:8]:
            self.assertEqual(draw["up_data"]["index"]["status"], "unreadable")
            self.assertIsNone(draw["up_data"]["index"]["data"])

    def test_up_short_successful_kernel_copy_is_also_rejected(self):
        self.run_capture("up", frame="1", extra={"T3_TEST_PARTIAL_READ": "success"})
        draws = [event for event in self.records() if event["event"] == "draw"]
        self.assertTrue(all(d["up_data"]["vertex"]["status"] == "unreadable" for d in draws[:8]))
        self.assertTrue(all(d["up_data"]["vertex"]["data"] is None for d in draws[:8]))

    def test_up_frame_budget_counts_only_captured_raw_bytes(self):
        self.run_capture("up_budget", frame="1")
        draws = [event for event in self.records() if event["event"] == "draw"]
        self.assertEqual(len(draws), 33)
        self.assertTrue(all(d["up_data"]["vertex"]["status"] == "captured" for d in draws[:32]))
        self.assertEqual(draws[32]["up_data"]["vertex"]["status"], "frame_limit")
        self.assertIsNone(draws[32]["up_data"]["vertex"]["data"])

    def test_failed_up_reads_do_not_consume_frame_budget(self):
        self.run_capture("up_budget", frame="1", extra={"T3_TEST_PARTIAL_READ": "1"})
        draws = [event for event in self.records() if event["event"] == "draw"]
        self.assertEqual(len(draws), 33)
        self.assertTrue(all(d["up_data"]["vertex"]["status"] == "unreadable" for d in draws))
        self.assertTrue(all(d["up_data"]["vertex"]["data"] is None for d in draws))

    def test_up_frame_budget_includes_indices(self):
        self.run_capture("up_mixed_budget", frame="1")
        draws = [event for event in self.records() if event["event"] == "draw"]
        self.assertEqual(len(draws), 33)
        self.assertEqual(draws[31]["up_data"]["vertex"]["byte_count"], 262136)
        self.assertEqual(draws[31]["up_data"]["index"]["status"], "captured")
        self.assertEqual(draws[32]["up_data"]["vertex"]["status"], "empty")
        self.assertEqual(draws[32]["up_data"]["index"]["status"], "frame_limit")
        self.assertIsNone(draws[32]["up_data"]["index"]["data"])

    def test_release_before_capture_restarts_with_second_device(self):
        self.run_capture("release_before", frame="1")
        summary = trace_summary.summarize(self.path)
        self.assertEqual(summary["frames"][0]["frame"], 1)
        self.assertEqual(summary["calls"], {"DrawIndexedPrimitive": 1})
        self.assertEqual(self.records()[1]["presentation"]["width"], 1600)

    def test_release_clears_armed_f11_for_second_device(self):
        self.run_capture("release_key", key="F11")
        summary = trace_summary.summarize(self.path)
        self.assertEqual(summary["frames"][0]["frame"], 2)
        self.assertEqual(summary["calls"], {"DrawIndexedPrimitive": 1})
        self.assertEqual(self.records()[1]["presentation"]["width"], 1600)

    def test_held_f11_does_not_retrigger_after_device_release(self):
        self.run_capture("release_key_held", key="F11")
        summary = trace_summary.summarize(self.path)
        self.assertEqual(summary["frames"][0]["frame"], 4)
        self.assertEqual(summary["calls"], {"DrawIndexedPrimitive": 1})

    def test_release_during_frame_leaves_detectably_incomplete_trace(self):
        self.run_capture("release_open", frame="1")
        self.assertEqual([event["event"] for event in self.records()], ["header", "frame_begin", "draw", "draw_result"])
        with self.assertRaisesRegex(trace_summary.TraceError, "frame 1 has no frame_end"):
            trace_summary.summarize(self.path)

    def test_short_writes_are_completed(self):
        self.run_capture(frame="1", extra={"T3_TEST_MAX_WRITE": "5"})
        self.assertEqual(trace_summary.summarize(self.path)["draw_count"], 4)

    def test_successful_reset_before_capture_updates_presentation(self):
        self.run_capture("reset_before", frame="1")
        self.assertEqual(self.records()[1]["presentation"]["width"], 1600)
        self.assertEqual(trace_summary.summarize(self.path)["draw_count"], 4)

    def test_failed_reset_before_capture_preserves_presentation(self):
        self.run_capture("reset_before_failure", frame="1")
        self.assertEqual(self.records()[1]["presentation"]["width"], 1280)
        self.assertEqual(trace_summary.summarize(self.path)["draw_count"], 4)

    def test_write_failure_stops_with_detectably_incomplete_trace(self):
        self.run_capture(frame="1", extra={"T3_TEST_WRITE_FAIL_AFTER": "1"})
        with self.assertRaisesRegex(trace_summary.TraceError, "missing capture_end"):
            trace_summary.summarize(self.path)

    def test_existing_trace_is_not_overwritten(self):
        self.path.write_text("preserve me", encoding="utf-8")
        self.run_capture(frame="1")
        self.assertEqual(self.path.read_text(encoding="utf-8"), "preserve me")

    def test_invalid_or_disabled_configuration_stays_passive(self):
        cases = [
            {"frame": None}, {"frame": "-1"}, {"frame": "+1"}, {"frame": "abc"},
            {"frame": "18446744073709551615"}, {"key": "F10"}, {"frame": "1", "key": "F11"},
            {"frame": "1", "path": False},
        ]
        for case in cases:
            with self.subTest(case=case):
                output = self.run_capture(**case)
                self.assertIn("configured=0 getters=0", output)
                self.assertFalse(self.path.exists())


if __name__ == "__main__":
    unittest.main()
