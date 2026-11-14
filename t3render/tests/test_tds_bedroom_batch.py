import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import analyze_tds_capture
import run_tds_bedroom_batch as batch


def trace_events(frames=2):
    header = {"event": "header", "format": "t3render-d3d8-diagnostic", "version": 1,
              "requested_frames": frames, "operation_results": 1, "clear_rect_limit": 256,
              "buffer_capture": {"requested": True, "encoding": "hex", "component_limit_bytes": 262144,
                                 "frame_limit_bytes": 8388608},
              "experimental_facade": {"enabled": True, "repeated_unlock_resource_discard": True,
                                      "repeated_unlock_post_write_recovery": True}}
    events = [header]
    for frame in range(10, 10 + frames):
        vertex = {"status": "captured", "resource": "0xdeadbeef", "stream": 0, "first_vertex": 5,
                  "vertex_count": 3, "stride": 4, "byte_offset": 20, "byte_count": 12,
                  "encoding": "hex", "data": "1415161718191a1b1c1d1e1f",
                  "shadow_observation": "post_repeated_unlock_write"}
        if frame == 11:
            vertex["data"] = "14" * 11 + "gg"
        draw = {"event": "draw", "frame": frame, "op_id": 1, "call": "DrawIndexedPrimitive",
                "primitive_type": 4, "primitive_count": 1, "min_vertex_index": 2, "num_vertices": 3,
                "start_index": 1, "render_states": {"168": 15},
                "streams": [{"stream": 0, "hr": 0, "buffer": "0xdeadbeef", "stride": 4,
                             "description": {"size": 64}}],
                "indices": {"hr": 0, "buffer": "0xbbb", "base_vertex_index": 3,
                            "description": {"format": 101, "size": 32}},
                "buffer_data": {"source": "application_lock_shadow", "vertex": vertex,
                    "index": {"status": "captured", "resource": "0xbbb", "index_count": 3,
                              "index_format": 101, "byte_offset": 2, "byte_count": 6,
                              "encoding": "hex", "data": "020003000400"}}}
        events.extend(({"event": "frame_begin", "frame": frame}, draw,
                       {"event": "draw_result", "frame": frame, "op_id": 1, "hr": 0,
                        "buffer_stable": True, "shader_stable": True},
                       {"event": "frame_end", "frame": frame, "present_hr": 0,
                        "concurrent_operations": False, "had_reset": False, "draws": 1}))
    events.append({"event": "capture_end", "frames": frames})
    return events


def write_trace(path, frames=2):
    Path(path).write_text("\n".join(json.dumps(item) for item in trace_events(frames)) + "\n")


class CaptureBatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-batch-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_analyzer_validates_and_redacts_multiple_frames(self):
        trace = self.root / "trace.jsonl"
        write_trace(trace)
        result = analyze_tds_capture.analyze(trace)
        self.assertEqual((result["status"], result["frames_captured"], result["best_frame_by_observed_candidates"]),
                         ("PARTIAL", 2, 10))
        first, second = result["frames"]
        self.assertEqual((first["indexed_with_both_buffers"], first["color_indexed_with_both_buffers"],
                          first["post_repeat_vertex_payloads"]), (1, 1, 1))
        self.assertEqual((second["indexed_with_both_buffers"], second["post_repeat_vertex_payloads"]), (0, 0))
        self.assertNotIn("0xdeadbeef", json.dumps(result))
        self.assertNotIn("1415161718191a1b1c1d1e1f", json.dumps(result))
        events = trace_events(1)
        events[2]["buffer_data"]["vertex"]["data"] = "00"
        trace.write_text("\n".join(json.dumps(item) for item in events) + "\n")
        self.assertIsNone(analyze_tds_capture.analyze(trace)["best_frame_by_observed_candidates"])
        events = trace_events()
        events[4]["had_reset"] = True
        events[8]["draws"] = 99
        trace.write_text("\n".join(json.dumps(item) for item in events) + "\n")
        self.assertIsNone(analyze_tds_capture.analyze(trace)["best_frame_by_observed_candidates"])
        trace.write_text(trace.read_text().rsplit("\n", 2)[0] + "\n")
        with self.assertRaises(analyze_tds_capture.trace_summary.TraceError):
            analyze_tds_capture.analyze(trace)

    def test_temporary_proxy_never_overwrites_or_deletes_unknown_files(self):
        source, target = self.root / "proxy", self.root / "d3d8.dll"
        source.write_bytes(b"temporary proxy")
        target.write_bytes(b"user dll")
        with self.assertRaises(batch.BatchError):
            batch.install_proxy(source, target)
        self.assertEqual(target.read_bytes(), b"user dll")
        target.unlink()
        with self.assertRaises(batch.BatchError):
            batch.install_proxy(source, target, expected_hash="wrong")
        self.assertFalse(target.exists())
        expected, identity = batch.install_proxy(source, target)
        with self.assertRaises(batch.BatchError):
            batch.remove_proxy(target, expected, [42])
        target.write_bytes(b"different dll")
        with self.assertRaises(batch.BatchError):
            batch.remove_proxy(target, expected, [], identity)
        self.assertEqual(target.read_bytes(), b"different dll")
        replacement = self.root / "replacement"
        replacement.write_bytes(source.read_bytes())
        replacement.replace(target)
        with self.assertRaises(batch.BatchError):
            batch.remove_proxy(target, expected, [], identity)
        self.assertTrue(target.exists())
        target.unlink()
        target.symlink_to(source)
        with self.assertRaises(batch.BatchError):
            batch.install_proxy(source, target)
        with self.assertRaises(batch.BatchError):
            batch.remove_proxy(target, expected, [])
        target.unlink()
        expected, identity = batch.install_proxy(source, target)
        batch.remove_proxy(target, expected, [], identity)
        self.assertFalse(target.exists())
        self.assertEqual(source.read_bytes(), b"temporary proxy")

    def test_pre_reset_history_is_counted_as_hypothesis_not_active_shader(self):
        events = trace_events(1)
        events[0]["declaration_capture"] = {"requested": True}
        events[0]["declaration_history"] = {"requested": True, "words_per_observation": 1024,
                                            "words_per_frame": 262144}
        events[0]["vertex_program_capture"] = {"requested": True, "words_per_observation": 1024,
                                               "words_per_frame": 262144}
        draw = events[2]
        draw["vertex_shader"] = 0x44
        draw["vertex_declaration"] = {"status": "unavailable", "generation": 5, "words": None,
                                       "reason": "reset_attempt", "scan_status": "complete"}
        history = {"status": "hypothesis", "source": "application_memory_before_reset", "generation": 5,
                   "reset_epoch": 1, "reset_hr": 0, "rebind_epoch": 2}
        draw["vertex_declaration_pre_reset"] = {**history, "words": [0x20000000, 0x40020000, 0xffffffff]}
        draw["vertex_program_pre_reset"] = {**history, "words": [0xfffe0101, 1, 0xc00f0000, 0x90e40000, 0xffff]}
        trace = self.root / "history.jsonl"
        trace.write_text("\n".join(json.dumps(item) for item in events) + "\n")
        frame = analyze_tds_capture.analyze(trace)["frames"][0]
        self.assertEqual((frame["declaration_candidates"], frame["program_candidates"]), (0, 0))
        self.assertEqual((frame["pre_reset_declaration_hypotheses"], frame["pre_reset_program_hypotheses"]), (1, 1))

    def test_unrecognized_metadata_cannot_leak_into_summary(self):
        events = trace_events(1)
        marker = "0xfeedface"
        events[0]["experimental_facade"]["identity_verified"] = marker
        draw = events[2]
        draw["buffer_data"]["vertex"]["shadow_reason"] = marker
        draw["buffer_data"]["window_failure"] = {"status": marker, "cause": marker, "source_line": marker}
        trace = self.root / "unexpected.jsonl"
        trace.write_text("\n".join(json.dumps(item) for item in events) + "\n")
        summary = analyze_tds_capture.analyze(trace)
        self.assertNotIn(marker, json.dumps(summary))
        self.assertIsNone(summary["experimental_facade"]["identity_verified"])

    def test_unknown_provenance_summarizes_reset_and_pre_frame_writes_without_addresses(self):
        events = trace_events(1)
        vertex = events[2]["buffer_data"]["vertex"]
        vertex.update(status="unknown", data=None, shadow_reason="unknown_resource", shadow_provenance={
            "lookup": "retired_reset_invalidated", "last_reset": "succeeded", "reset_attempts": 1,
            "reset_successes": 1, "reset_failures": 0,
            "history_known": True, "resets_since_creation": 1, "lock_attempts": 2,
            "successful_locks": 2, "successful_write_unlocks": 2,
            "last_lock": {"offset": 4, "requested_bytes": 12, "flags": 0, "hr": 0}})
        trace = self.root / "provenance.jsonl"
        trace.write_text("\n".join(json.dumps(item) for item in events) + "\n")
        result = analyze_tds_capture.analyze(trace)
        self.assertEqual(result["frames"][0]["unknown_resource_provenance"], [{
            "resource": "vertex", "lookup": "retired_reset_invalidated", "last_reset": "succeeded",
            "reset_attempts": 1, "reset_successes": 1, "reset_failures": 0, "history_known": True, "resets_since_creation": 1,
            "lock_attempts": 2, "successful_locks": 2, "successful_write_unlocks": 2,
            "last_lock_offset": 4, "last_lock_requested_bytes": 12, "last_lock_flags": 0, "last_lock_hr": 0,
            "draw_observations": 1, "distinct_resource_pointers": 1}])
        self.assertNotIn("0xdeadbeef", json.dumps(result))

    def test_lock_registry_failure_is_actionable_without_resource_addresses(self):
        events = trace_events(1)
        events[2]["buffer_data"]["window_failure"] = {"status": "device_unsafe", "cause": "lock_registry",
            "source_line": 539, "lock": {"resource": "vertex", "registry_result": "invalid_range",
                "recovery_attempted": True, "offset": 7, "requested_bytes": 0,
                "normalized_bytes": 0, "buffer_size": 8, "flags": 0}}
        events[3]["buffer_stable"] = False
        trace = self.root / "lock_failure.jsonl"
        trace.write_text("\n".join(json.dumps(item) for item in events) + "\n")
        frame = analyze_tds_capture.analyze(trace)["frames"][0]
        self.assertEqual(frame["buffer_window_failures"], {"device_unsafe:lock_registry:539": 1})
        self.assertEqual(frame["lock_failures"], [{"resource": "vertex", "registry_result": "invalid_range",
            "recovery_attempted": True, "offset": 7, "requested_bytes": 0,
            "normalized_bytes": 0, "buffer_size": 8, "flags": 0, "draw_observations": 1}])

    def test_never_terminate_a_different_game_process(self):
        with mock.patch.object(batch, "game_pids", return_value=[11]), \
             mock.patch.object(batch, "process_start", return_value="different"), \
             mock.patch.object(batch.os, "kill") as kill:
            with self.assertRaises(batch.BatchError):
                batch.terminate_game(100, 11, "original")
            kill.assert_not_called()
        with mock.patch.object(batch, "game_pids", return_value=[12]), \
             mock.patch.object(batch.os, "kill") as kill:
            with self.assertRaises(batch.BatchError):
                batch.terminate_game(100, 11, "original")
            kill.assert_not_called()

    def test_environment_does_not_inherit_capture_configuration(self):
        with mock.patch.dict(os.environ, {"T3RENDER_CAPTURE_KEY": "F11",
                                         "T3RENDER_EXPERIMENTAL_UNRELATED": "1"}):
            env, config = batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3)
        self.assertNotIn("T3RENDER_CAPTURE_KEY", env)
        self.assertNotIn("T3RENDER_EXPERIMENTAL_UNRELATED", env)
        self.assertEqual(config["T3RENDER_CAPTURE_FRAMES"], "3")
        self.assertEqual(config["T3RENDER_CAPTURE_DECLARATION_HISTORY"], "1")
        self.assertTrue(config["T3RENDER_CAPTURE_PATH"].startswith("Z:\\"))

    def test_simulated_run_captures_analyzes_and_removes_proxy(self):
        engine, bottle = self.root / "engine", self.root / "bottle"
        system = bottle / "drive_c/Steam/Thief/System"
        system.mkdir(parents=True)
        proxy = self.root / "proxy.dll"
        proxy.write_bytes(b"fixture proxy")
        output = self.root / "result"
        resolved = tuple(p.resolve() for p in (engine, bottle, system, proxy, output))
        def completed(path, frames):
            self.assertTrue((output / "capture.ready").is_file())
            if not path.exists():
                write_trace(path, frames)
            return True
        with mock.patch.object(batch, "validate_paths", return_value=resolved), \
             mock.patch.object(batch, "game_pids", side_effect=[[], [], [42], []]), \
             mock.patch.object(batch, "wait_for_one_game"), \
             mock.patch.object(batch, "process_start", return_value="same"), \
             mock.patch.object(batch, "terminate_game") as terminate, \
             mock.patch.object(batch, "loader_verified", return_value=True), \
             mock.patch.object(batch, "complete_trace", side_effect=completed), \
             mock.patch.object(batch.subprocess, "Popen"):
            result = batch.execute(*map(str, resolved), delay=0, timeout=10, frames=3, terminate=True)
        self.assertEqual(result["status"], "PARTIAL")
        self.assertTrue(result["temporary_proxy_removed"])
        self.assertEqual(result["scene_confirmation"], "unconfirmed")
        self.assertFalse((system / "d3d8.dll").exists())
        self.assertEqual(json.loads((output / "summary.json").read_text())["frames_captured"], 3)
        terminate.assert_called_once()

    def test_failed_preflight_never_installs_proxy_or_launches_game(self):
        engine, bottle = self.root / "engine", self.root / "bottle"
        system = bottle / "drive_c/Steam/Thief/System"
        system.mkdir(parents=True)
        proxy = self.root / "proxy.dll"
        proxy.write_bytes(b"fixture proxy")
        output = self.root / "result"
        resolved = tuple(p.resolve() for p in (engine, bottle, system, proxy, output))
        with mock.patch.object(batch, "validate_paths", return_value=resolved), \
             mock.patch.object(batch, "game_pids", return_value=[]), \
             mock.patch.object(batch.subprocess, "run", return_value=mock.Mock(returncode=1)), \
             mock.patch.object(batch.subprocess, "Popen") as popen:
            with self.assertRaises(batch.BatchError):
                batch.execute(*map(str, resolved), delay=0, timeout=10, frames=1, terminate=True, preflight=True)
        popen.assert_not_called()
        self.assertFalse((system / "d3d8.dll").exists())
        self.assertEqual(json.loads((output / "batch.json").read_text())["host_preflight"], "FAILED")

    def test_timeout_leaves_proxy_until_game_closes_then_cleanup_removes_it(self):
        engine, bottle = self.root / "engine", self.root / "bottle"
        system = bottle / "drive_c/Steam/Thief/System"
        system.mkdir(parents=True)
        proxy = self.root / "proxy.dll"
        proxy.write_bytes(b"fixture proxy")
        output = self.root / "result"
        resolved = tuple(p.resolve() for p in (engine, bottle, system, proxy, output))
        with mock.patch.object(batch, "validate_paths", return_value=resolved), \
             mock.patch.object(batch, "game_pids", side_effect=[[], [], [42], [42], [42]]), \
             mock.patch.object(batch, "wait_for_one_game"), \
             mock.patch.object(batch, "process_start", return_value="same"), \
             mock.patch.object(batch, "complete_trace", return_value=False), \
             mock.patch.object(batch.subprocess, "Popen"):
            with self.assertRaises(batch.BatchError):
                batch.execute(*map(str, resolved), delay=0, timeout=0.01, frames=1, terminate=True)
        self.assertTrue((system / "d3d8.dll").exists())
        self.assertFalse(json.loads((output / "deployment.json").read_text())["temporary_proxy_removed"])
        with mock.patch.object(batch, "game_pids", return_value=[]):
            batch.cleanup(output / "deployment.json")
        self.assertFalse((system / "d3d8.dll").exists())


if __name__ == "__main__":
    unittest.main()
