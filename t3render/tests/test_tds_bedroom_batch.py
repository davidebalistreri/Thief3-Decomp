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

    def test_wine_crash_monitor_requires_matching_loader_thread_and_complete_line(self):
        log = self.root / "wine.log"
        executable = r"C:\Program Files (x86)\Steam\steamapps\common\Thief Deadly Shadows\System\T3Main.exe"
        loader = executable.replace("\\", "\\\\")
        log.write_text('05ab:trace:loaddll:build_module Loaded L"C:\\\\Other\\\\T3Main.exe" at 10900000: native\n'
                       f'0888:trace:loaddll:build_module Loaded L"{loader}" at 10900000: builtin\n'
                       f'09c4:trace:loaddll:build_module Loaded L"{loader}" at 10900000: native\n'
                       'wine: Unhandled page fault on read access to 00000000 at address 12345678 '
                       '(thread 05ab), starting debugger...\n'
                       'wine: Unhandled page fault on read access to 00000000 at address 109722CF '
                       '(thread 09c4), starting debugger...', encoding="utf-8")
        monitor = batch.WineMainThreadCrash(log, executable)
        monitor.check()  # The other thread and an incomplete final line are not conclusive.
        with log.open("a", encoding="utf-8") as stream:
            stream.write("\n")
        with self.assertRaisesRegex(batch.BatchError, "T3Main loader thread"):
            monitor.check()

    def test_startup_detects_a_crash_before_a_game_process_can_be_observed(self):
        log = self.root / "wine.log"
        executable = r"C:\Games\Thief\T3Main.exe"
        loader = executable.replace("\\", "\\\\")
        log.write_text(f'09c4:trace:loaddll:build_module Loaded L"{loader}" at 10900000: native\n'
                       'wine: Unhandled page fault on read access to 00000000 at address 109722CF '
                       '(thread 09c4), starting debugger...\n', encoding="utf-8")
        with mock.patch.object(batch, "game_pids") as processes:
            with self.assertRaisesRegex(batch.BatchError, "T3Main loader thread"):
                batch.wait_for_one_game(batch.time.monotonic() + 5, batch.WineMainThreadCrash(log, executable))
            processes.assert_not_called()

    def test_wine_crash_monitor_ignores_ambiguous_game_loads(self):
        log = self.root / "wine.log"
        executable = r"C:\Games\Thief\T3Main.exe"
        loader = executable.replace("\\", "\\\\")
        log.write_text(f'09c4:trace:loaddll:build_module Loaded L"{loader}" at 10900000: native\n'
                       f'09d0:trace:loaddll:build_module Loaded L"{loader}" at 10900000: native\n'
                       'wine: Unhandled page fault on read access to 00000000 at address 109722CF '
                       '(thread 09c4), starting debugger...\n', encoding="utf-8")
        batch.WineMainThreadCrash(log, executable).check()

    def test_manual_wait_allows_a_level_restart_before_the_request(self):
        request = mock.Mock()
        request.is_file.side_effect = [False, False, True, True]
        with mock.patch.object(batch, "game_pids", side_effect=[[42], [], [42, 50], [50]]), \
             mock.patch.object(batch, "process_start", return_value="replacement"), \
             mock.patch.object(batch.time, "sleep"):
            result = batch.wait_for_trigger(batch.time.monotonic() + 5, mock.Mock(), request=request)
        self.assertEqual(result, (50, "replacement"))

    def test_timed_wait_allows_a_level_restart_before_capture(self):
        with mock.patch.object(batch, "game_pids", side_effect=[[42], [], [50]]), \
             mock.patch.object(batch, "process_start", return_value="replacement"), \
             mock.patch.object(batch.time, "monotonic", side_effect=[0, 0, 1, 1, 2, 2]), \
             mock.patch.object(batch.time, "sleep"):
            result = batch.wait_for_trigger(5, mock.Mock(), trigger_time=2)
        self.assertEqual(result, (50, "replacement"))

    def test_manual_wait_stops_after_prolonged_game_absence(self):
        request = self.root / "capture.request"
        with mock.patch.object(batch, "game_pids", return_value=[]), \
             mock.patch.object(batch.time, "monotonic", side_effect=[0, 0, 31, 31]), \
             mock.patch.object(batch.time, "sleep"):
            with self.assertRaisesRegex(batch.BatchError, "no successor"):
                batch.wait_for_trigger(100, mock.Mock(), request=request)

    def test_save_guard_requires_real_directory_and_detects_wine(self):
        system = self.root / "game/System"
        saves = self.root / "game/save/SaveGames"
        system.mkdir(parents=True)
        saves.mkdir(parents=True)
        profile = batch.readonly_save_profile(system)
        self.assertIn('(deny file-write* (subpath ', profile)
        self.assertIn(str(saves.resolve()), profile)
        env = {"DYLD_FALLBACK_LIBRARY_PATH": "lib path", "DYLD_FALLBACK_FRAMEWORK_PATH": "framework path"}
        command = batch.readonly_save_command(["wine", "game"], profile, env)
        self.assertEqual(command[:4], ["/usr/bin/sandbox-exec", "-p", profile, "/usr/bin/env"])
        self.assertEqual(command[4:], ["DYLD_FALLBACK_LIBRARY_PATH=lib path",
                                       "DYLD_FALLBACK_FRAMEWORK_PATH=framework path", "wine", "game"])
        saves.rmdir()
        saves.symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(batch.BatchError, "real SaveGames"):
            batch.readonly_save_profile(system)
        listing = ('21 C:\\Program Files (x86)\\Steam\\steam.exe\n'
                   '22 /opt/engine/bin/wineserver\n23 /usr/bin/python3\n')
        with mock.patch.object(batch.subprocess, "check_output", return_value=listing):
            self.assertEqual(batch.wine_or_steam_pids(), [21, 22])

    def test_environment_does_not_inherit_capture_configuration(self):
        with mock.patch.dict(os.environ, {"T3RENDER_CAPTURE_KEY": "F11",
                                         "T3RENDER_EXPERIMENTAL_UNRELATED": "1"}):
            env, config = batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3)
        self.assertNotIn("T3RENDER_CAPTURE_KEY", env)
        self.assertNotIn("T3RENDER_EXPERIMENTAL_UNRELATED", env)
        self.assertEqual(config["T3RENDER_CAPTURE_FRAMES"], "3")
        self.assertEqual(config["T3RENDER_CAPTURE_DECLARATION_HISTORY"], "1")
        self.assertTrue(config["T3RENDER_CAPTURE_PATH"].startswith("Z:\\"))
        self.assertNotIn("T3RENDER_SCENE_BRIDGE", env)
        enabled, selected = batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                                          scene_bridge=True)
        self.assertEqual(enabled["T3RENDER_SCENE_BRIDGE"], "1")
        self.assertEqual(selected["T3RENDER_SCENE_BRIDGE"], "1")
        catalog = self.root / "catalog.bin"
        with self.assertRaises(batch.BatchError):
            batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                          live_catalog=catalog)
        live, selected = batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                                       scene_bridge=True, submission_scope=True, live_catalog=catalog)
        self.assertEqual(live["T3RENDER_SCENE_LIVE_DX12"], "1")
        self.assertEqual(selected["T3RENDER_LIVE_CATALOG"], batch.windows_path(catalog))
        self.assertEqual(selected["T3RENDER_SCENE_LIVE_DIAGNOSTIC_DIR"], batch.windows_path(self.root / "output"))
        self.assertNotIn("T3RENDER_SCENE_LIVE_INITIAL_IMAGES", selected)
        self.assertNotIn("T3RENDER_SCENE_LIVE_STATIC_MESHES", selected)
        self.assertNotIn("T3RENDER_SCENE_LIVE_GPU_METRICS", selected)
        with self.assertRaises(batch.BatchError):
            batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3, gpu_metrics=True)
        _, metrics = batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                                   scene_bridge=True, submission_scope=True, live_catalog=catalog, gpu_metrics=True)
        self.assertEqual(metrics["T3RENDER_SCENE_LIVE_GPU_METRICS"], "1")
        self.assertNotIn("T3RENDER_SCENE_LIVE_PREVIEW_PBR", selected)
        with self.assertRaises(batch.BatchError):
            batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3, preview_pbr=True)
        _, preview = batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                                   scene_bridge=True, submission_scope=True, live_catalog=catalog, preview_pbr=True)
        self.assertEqual(preview["T3RENDER_SCENE_LIVE_PREVIEW_PBR"], "1")
        with self.assertRaises(batch.BatchError):
            batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                          scene_bridge=True, submission_scope=True, live_catalog=catalog, preview_candle=True)
        with self.assertRaises(batch.BatchError):
            batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                          scene_bridge=True, submission_scope=True, live_catalog=catalog, live_lights=True,
                          preview_pbr=True, preview_candle=True)
        _, candle = batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                                  scene_bridge=True, submission_scope=True, live_catalog=catalog,
                                  live_lights=True, preview_candle=True)
        self.assertEqual(candle["T3RENDER_SCENE_LIVE_PREVIEW_CANDLE"], "1")
        self.assertNotIn("T3RENDER_SCENE_LIVE_CANDLE_LINK", candle)
        with self.assertRaises(batch.BatchError):
            batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                          scene_bridge=True, submission_scope=True, live_catalog=catalog, candle_link_diagnostics=True)
        _, candle_link = batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                                       scene_bridge=True, submission_scope=True, live_catalog=catalog,
                                       live_lights=True, candle_link_diagnostics=True)
        self.assertEqual(candle_link["T3RENDER_SCENE_LIVE_CANDLE_LINK"], "1")
        with self.assertRaises(batch.BatchError):
            batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3, wall_displacement=True)
        _, displacement = batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                                        scene_bridge=True, submission_scope=True, live_catalog=catalog, wall_displacement=True)
        self.assertEqual(displacement["T3RENDER_SCENE_LIVE_WALL_DISPLACEMENT"], "1")
        with self.assertRaises(batch.BatchError):
            batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3, preview_gi=True)
        _, gi = batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                              scene_bridge=True, submission_scope=True, live_catalog=catalog, preview_gi=True)
        self.assertEqual(gi["T3RENDER_SCENE_LIVE_PREVIEW_GI"], "1")
        with self.assertRaises(batch.BatchError):
            batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                          live_static_meshes=True)
        _, static = batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                                  scene_bridge=True, submission_scope=True, live_catalog=catalog, live_static_meshes=True)
        self.assertEqual(static["T3RENDER_SCENE_LIVE_STATIC_MESHES"], "1")
        with self.assertRaises(batch.BatchError):
            batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3, live_lights=True)
        _, lights = batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                                  scene_bridge=True, submission_scope=True, live_catalog=catalog, live_lights=True)
        self.assertEqual(lights["T3RENDER_SCENE_LIVE_LIGHTS"], "1")
        self.assertNotIn("T3RENDER_SCENE_LIVE_LIGHTS", selected)
        live_only, selected = batch.env_for(self.root / "engine", self.root / "bottle", self.root / "output", 3,
                                            scene_bridge=True, submission_scope=True, live_catalog=catalog, live_only=True)
        self.assertNotIn("T3RENDER_CAPTURE_PATH", live_only)
        self.assertNotIn("T3RENDER_EXPERIMENTAL_REPEATED_UNLOCK", live_only)
        self.assertEqual(live_only["T3RENDER_SCENE_LIVE_INITIAL_IMAGES"], "0")
        self.assertEqual(selected["T3RENDER_SCENE_LIVE_DX12"], "1")

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
             mock.patch.object(batch, "game_pids", side_effect=[[], [], [42], [42], []]), \
             mock.patch.object(batch, "wait_for_one_game", return_value=42), \
             mock.patch.object(batch, "process_start", return_value="same"), \
             mock.patch.object(batch, "terminate_game") as terminate, \
             mock.patch.object(batch, "loader_verified", return_value=True), \
             mock.patch.object(batch, "complete_trace", side_effect=completed), \
             mock.patch.object(batch.subprocess, "Popen"):
            result = batch.execute(*map(str, resolved), delay=0, timeout=10, frames=3, terminate=True)
        self.assertEqual(result["status"], "PARTIAL")
        self.assertTrue(result["temporary_proxy_removed"])
        self.assertFalse(result["game_restarted_before_capture"])
        self.assertEqual(result["scene_confirmation"], "unconfirmed")
        self.assertFalse((system / "d3d8.dll").exists())
        self.assertEqual(json.loads((output / "summary.json").read_text())["frames_captured"], 3)
        terminate.assert_called_once()

    def test_simulated_live_only_does_not_trigger_a_trace(self):
        engine, bottle = self.root / "engine", self.root / "bottle"
        system = bottle / "drive_c/Steam/Thief/System"
        system.mkdir(parents=True)
        proxy, catalog = self.root / "proxy.dll", self.root / "catalog.bin"
        proxy.write_bytes(b"fixture proxy")
        for version in (b"04", b"05"):
            with self.subTest(version=version):
                output = self.root / ("result-" + version.decode())
                catalog.write_bytes(b"T3LCAT" + version + b"fixture catalog")
                resolved = tuple(p.resolve() for p in (engine, bottle, system, proxy, output))
                with mock.patch.object(batch, "validate_paths", return_value=resolved), \
                     mock.patch.object(batch, "game_pids", side_effect=[[], [], [42], [42], []]), \
                     mock.patch.object(batch, "wait_for_one_game", return_value=42), \
                     mock.patch.object(batch, "process_start", return_value="same"), \
                     mock.patch.object(batch, "terminate_game") as terminate, \
                     mock.patch.object(batch, "loader_verified", return_value=True), \
                     mock.patch.object(batch.subprocess, "Popen"):
                    result = batch.execute(*map(str, resolved), delay=0, timeout=10, frames=3, terminate=True,
                                           scene_bridge=True, submission_scope=True, live_catalog=str(catalog), live_only=True,
                                           live_static_meshes=True, live_lights=True)
                self.assertTrue(result["live_only"] and result["temporary_proxy_removed"])
                self.assertTrue(result["live_static_meshes_requested"])
                self.assertTrue(result["live_light_candidates_requested"])
                deployment = json.loads((output / "deployment.json").read_text())
                self.assertEqual(deployment["capture_config"]["T3RENDER_SCENE_LIVE_STATIC_MESHES"], "1")
                self.assertEqual(deployment["capture_config"]["T3RENDER_SCENE_LIVE_LIGHTS"], "1")
                self.assertEqual(result["frames_requested"], 0)
                self.assertFalse((output / "bedroom.jsonl").exists())
                self.assertFalse((output / "capture.ready").exists())
                terminate.assert_called_once()

    def test_steam_launch_waits_for_manual_capture_request(self):
        engine, bottle = self.root / "engine", self.root / "bottle"
        steam = bottle / "drive_c/Program Files (x86)/Steam/steam.exe"
        system = steam.parent / "steamapps/common/Thief Deadly Shadows/System"
        system.mkdir(parents=True)
        steam.write_bytes(b"steam fixture")
        proxy, output = self.root / "proxy.dll", self.root / "result"
        proxy.write_bytes(b"fixture proxy")
        resolved = tuple(p.resolve() for p in (engine, bottle, system, proxy, output))
        def request(_seconds):
            self.assertFalse((output / "capture.ready").exists())
            (output / "capture.request").touch()
        def completed(path, frames):
            self.assertTrue((output / "capture.request").is_file())
            self.assertTrue((output / "capture.ready").is_file())
            if not path.exists():
                write_trace(path, frames)
            return True
        with mock.patch.object(batch, "validate_paths", return_value=resolved), \
             mock.patch.object(batch, "game_pids", side_effect=[[], [], [42], [42], [42], []]), \
             mock.patch.object(batch, "wait_for_one_game", return_value=42), \
             mock.patch.object(batch, "process_start", return_value="same"), \
             mock.patch.object(batch, "terminate_game") as terminate, \
             mock.patch.object(batch, "loader_verified", return_value=True), \
             mock.patch.object(batch, "complete_trace", side_effect=completed), \
             mock.patch.object(batch.time, "sleep", side_effect=request), \
             mock.patch.object(batch.subprocess, "Popen") as popen:
            result = batch.execute(*map(str, resolved), delay=0, timeout=10, frames=3, terminate=True,
                                   manual_trigger=True, steam_launch=True, resume_bedroom=True)
        self.assertEqual(popen.call_args.args[0][-4:], ["-silent", "-applaunch", "6980", batch.BEDROOM_SAVE_URL])
        self.assertTrue(result["manual_trigger"] and result["steam_launch"] and result["resume_bedroom"] and
                        result["temporary_proxy_removed"])
        terminate.assert_called_once()

    def test_isolated_launch_closes_its_private_prefix(self):
        engine, bottle = self.root / "engine", self.root / "bottle"
        steam = bottle / "drive_c/Program Files (x86)/Steam/steam.exe"
        system = steam.parent / "steamapps/common/Thief Deadly Shadows/System"
        system.mkdir(parents=True)
        steam.write_bytes(b"steam fixture")
        proxy, output = self.root / "proxy.dll", self.root / "result"
        proxy.write_bytes(b"fixture proxy")
        resolved = tuple(p.resolve() for p in (engine, bottle, system, proxy, output))
        sandbox = mock.Mock()
        sandbox.root = self.root / "private-prefix"
        sandbox.removed = True
        sandbox.saves_unchanged = True
        sandbox.environment.side_effect = lambda env: {**env, "WINEPREFIX": str(sandbox.root)}
        sandbox.command.side_effect = lambda command, env: ["isolated"] + command

        def completed(path, frames):
            if not path.exists():
                write_trace(path, frames)
            return True

        original_is_file = Path.is_file
        def is_file(path):
            return True if str(path) == "/usr/bin/sandbox-exec" else original_is_file(path)

        with mock.patch.object(batch, "validate_paths", return_value=resolved), \
             mock.patch.object(batch.sys, "platform", "darwin"), \
             mock.patch.object(Path, "is_file", is_file), \
             mock.patch.object(batch, "wine_or_steam_pids", return_value=[]), \
             mock.patch.object(batch, "game_pids", side_effect=[[], [], [42], [42], []]), \
             mock.patch.object(batch, "wait_for_one_game", return_value=42), \
             mock.patch.object(batch, "process_start", return_value="same"), \
             mock.patch.object(batch, "terminate_game") as terminate, \
             mock.patch.object(batch, "loader_verified", return_value=True), \
             mock.patch.object(batch, "complete_trace", side_effect=completed), \
             mock.patch.object(batch.IsolatedSavePrefix, "prepare", return_value=sandbox), \
             mock.patch.object(batch.subprocess, "Popen") as popen:
            result = batch.execute(*map(str, resolved), delay=0, timeout=10, frames=1, terminate=True,
                                   steam_launch=True, isolated_saves=True)
        self.assertTrue(result["isolated_saves"] and result["sandbox_removed"] and result["temporary_proxy_removed"] and
                        result["original_saves_unchanged"])
        self.assertTrue(sandbox.started)
        self.assertEqual(popen.call_args.args[0][0], "isolated")
        self.assertEqual(popen.call_args.kwargs["env"]["WINEPREFIX"], str(sandbox.root))
        sandbox.close.assert_called_once()
        terminate.assert_not_called()

    def test_restarted_nonisolated_game_is_not_automatically_terminated(self):
        engine, bottle = self.root / "engine", self.root / "bottle"
        steam = bottle / "drive_c/Program Files (x86)/Steam/steam.exe"
        system = steam.parent / "steamapps/common/Thief Deadly Shadows/System"
        system.mkdir(parents=True)
        steam.write_bytes(b"steam fixture")
        proxy, output = self.root / "proxy.dll", self.root / "result"
        proxy.write_bytes(b"fixture proxy")
        resolved = tuple(p.resolve() for p in (engine, bottle, system, proxy, output))

        def launch(*args, **kwargs):
            (output / "capture.request").touch()
            return mock.Mock()

        def completed(path, frames):
            if not path.exists():
                write_trace(path, frames)
            return True

        with mock.patch.object(batch, "validate_paths", return_value=resolved), \
             mock.patch.object(batch, "game_pids", side_effect=[[], [], [42], [42], [42]]), \
             mock.patch.object(batch, "wait_for_one_game", return_value=42), \
             mock.patch.object(batch, "process_start", side_effect=["original", "replacement", "replacement"]), \
             mock.patch.object(batch, "terminate_game") as terminate, \
             mock.patch.object(batch, "loader_verified", return_value=True), \
             mock.patch.object(batch, "complete_trace", side_effect=completed), \
             mock.patch.object(batch.subprocess, "Popen", side_effect=launch):
            with self.assertRaisesRegex(batch.BatchError, "automatic termination withheld"):
                batch.execute(*map(str, resolved), delay=0, timeout=10, frames=1, terminate=True,
                              manual_trigger=True, steam_launch=True)
        terminate.assert_not_called()
        self.assertTrue((output / "capture.ready").exists())
        self.assertTrue((output / "summary.json").exists())
        self.assertTrue(json.loads((output / "batch.json").read_text())["game_restarted_before_capture"])
        self.assertTrue((system / "d3d8.dll").is_file())
        self.assertFalse(json.loads((output / "deployment.json").read_text())["temporary_proxy_removed"])
        with mock.patch.object(batch, "game_pids", return_value=[]):
            batch.cleanup(output / "deployment.json")
        self.assertFalse((system / "d3d8.dll").exists())

    def test_isolated_manual_capture_survives_a_level_restart(self):
        engine, bottle = self.root / "engine", self.root / "bottle"
        steam = bottle / "drive_c/Program Files (x86)/Steam/steam.exe"
        system = steam.parent / "steamapps/common/Thief Deadly Shadows/System"
        system.mkdir(parents=True)
        steam.write_bytes(b"steam fixture")
        proxy, output = self.root / "proxy.dll", self.root / "result"
        proxy.write_bytes(b"fixture proxy")
        resolved = tuple(p.resolve() for p in (engine, bottle, system, proxy, output))
        sandbox = mock.Mock()
        sandbox.root = self.root / "private-prefix"
        sandbox.removed = True
        sandbox.saves_unchanged = True
        sandbox.environment.side_effect = lambda env: {**env, "WINEPREFIX": str(sandbox.root)}
        sandbox.command.side_effect = lambda command, env: ["isolated"] + command
        original_is_file = Path.is_file

        def is_file(path):
            return True if str(path) == "/usr/bin/sandbox-exec" else original_is_file(path)

        def launch(*args, **kwargs):
            (output / "capture.request").touch()
            return mock.Mock()

        def completed(path, frames):
            if not path.exists():
                write_trace(path, frames)
            return True

        with mock.patch.object(batch, "validate_paths", return_value=resolved), \
             mock.patch.object(batch.sys, "platform", "darwin"), \
             mock.patch.object(Path, "is_file", is_file), \
             mock.patch.object(batch, "wine_or_steam_pids", return_value=[]), \
             mock.patch.object(batch, "game_pids", side_effect=[[], [], [50], [50], []]), \
             mock.patch.object(batch, "wait_for_one_game", return_value=42), \
             mock.patch.object(batch, "process_start", side_effect=lambda pid: {42: "old", 50: "new"}[pid]), \
             mock.patch.object(batch, "terminate_game") as terminate, \
             mock.patch.object(batch, "loader_verified", return_value=True), \
             mock.patch.object(batch, "complete_trace", side_effect=completed), \
             mock.patch.object(batch.IsolatedSavePrefix, "prepare", return_value=sandbox), \
             mock.patch.object(batch.subprocess, "Popen", side_effect=launch):
            result = batch.execute(*map(str, resolved), delay=0, timeout=10, frames=1, terminate=True,
                                   manual_trigger=True, steam_launch=True, isolated_saves=True)
        self.assertEqual(result["status"], "PARTIAL")
        self.assertTrue(result["game_restarted_before_capture"])
        self.assertTrue(result["sandbox_removed"] and result["temporary_proxy_removed"] and
                        result["original_saves_unchanged"])
        self.assertTrue((output / "summary.json").is_file())
        sandbox.close.assert_called_once()
        terminate.assert_not_called()

    def test_isolated_launch_requires_its_own_terminated_steam_session(self):
        with self.assertRaisesRegex(batch.BatchError, "isolated saves require"):
            batch.execute("", "", "", "", "", 0, 1, 1, False, isolated_saves=True)
        with self.assertRaisesRegex(batch.BatchError, "isolated saves require"):
            batch.execute("", "", "", "", "", 0, 1, 1, True, steam_launch=True,
                          readonly_saves=True, isolated_saves=True)

    def test_isolated_manual_crash_stops_waiting_and_cleans_the_private_session(self):
        engine, bottle = self.root / "engine", self.root / "bottle"
        steam = bottle / "drive_c/Program Files (x86)/Steam/steam.exe"
        system = steam.parent / "steamapps/common/Thief Deadly Shadows/System"
        system.mkdir(parents=True)
        steam.write_bytes(b"steam fixture")
        proxy, output = self.root / "proxy.dll", self.root / "result"
        proxy.write_bytes(b"fixture proxy")
        resolved = tuple(p.resolve() for p in (engine, bottle, system, proxy, output))
        sandbox = mock.Mock()
        sandbox.root = self.root / "private-prefix"
        sandbox.removed = True
        sandbox.saves_unchanged = True
        sandbox.environment.side_effect = lambda env: {**env, "WINEPREFIX": str(sandbox.root)}
        sandbox.command.side_effect = lambda command, env: ["isolated"] + command
        original_is_file = Path.is_file

        def is_file(path):
            return True if str(path) == "/usr/bin/sandbox-exec" else original_is_file(path)

        def launch(*args, **kwargs):
            executable = r"C:\Program Files (x86)\Steam\steamapps\common\Thief Deadly Shadows\System\T3Main.exe"
            loader = executable.replace("\\", "\\\\")
            log = (f'09c4:trace:loaddll:build_module Loaded L"{loader}" at 10900000: native\n'
                   'wine: Unhandled page fault on read access to 00000000 at address 109722CF '
                   '(thread 09c4), starting debugger...\n')
            kwargs["stdout"].write(log.encode("utf-8"))
            kwargs["stdout"].flush()
            return mock.Mock()

        with mock.patch.object(batch, "validate_paths", return_value=resolved), \
             mock.patch.object(batch.sys, "platform", "darwin"), \
             mock.patch.object(Path, "is_file", is_file), \
             mock.patch.object(batch, "wine_or_steam_pids", return_value=[]), \
             mock.patch.object(batch, "game_pids", side_effect=[[], [], []]), \
             mock.patch.object(batch, "wait_for_one_game", return_value=42), \
             mock.patch.object(batch, "process_start", return_value="same"), \
             mock.patch.object(batch, "terminate_game") as terminate, \
             mock.patch.object(batch.IsolatedSavePrefix, "prepare", return_value=sandbox), \
             mock.patch.object(batch.subprocess, "Popen", side_effect=launch):
            with self.assertRaisesRegex(batch.BatchError, "T3Main loader thread"):
                batch.execute(*map(str, resolved), delay=0, timeout=10, frames=1, terminate=True,
                              manual_trigger=True, steam_launch=True, isolated_saves=True)
        result = json.loads((output / "batch.json").read_text())
        self.assertEqual(result["status"], "FAILED")
        self.assertIn("T3Main loader thread", result["error"])
        self.assertTrue(result["sandbox_removed"] and result["temporary_proxy_removed"] and
                        result["original_saves_unchanged"])
        self.assertFalse((output / "capture.ready").exists())
        self.assertFalse((system / "d3d8.dll").exists())
        sandbox.close.assert_called_once()
        terminate.assert_not_called()

    def test_static_mesh_sampling_rejects_old_catalog_before_launch(self):
        engine, bottle = self.root / "engine", self.root / "bottle"
        system = bottle / "drive_c/Steam/Thief/System"
        system.mkdir(parents=True)
        proxy, catalog, output = self.root / "proxy.dll", self.root / "catalog.bin", self.root / "result"
        proxy.write_bytes(b"fixture proxy")
        resolved = tuple(p.resolve() for p in (engine, bottle, system, proxy, output))
        for content in (b"T3LCAT03old catalog", b"T3LCAT06\x00\x00\x00\x00"):
            catalog.write_bytes(content)
            with self.subTest(content=content), mock.patch.object(batch, "validate_paths", return_value=resolved), \
                 mock.patch.object(batch.subprocess, "Popen") as popen:
                with self.assertRaisesRegex(batch.BatchError, "v4"):
                    batch.execute(*map(str, resolved), delay=0, timeout=10, frames=1, terminate=True,
                                  scene_bridge=True, submission_scope=True, live_catalog=str(catalog), live_static_meshes=True)
            popen.assert_not_called()
            self.assertFalse(output.exists())

    def test_displacement_rejects_catalog_without_height_before_launch(self):
        engine, bottle = self.root / "engine", self.root / "bottle"
        system = bottle / "drive_c/Steam/Thief/System";system.mkdir(parents=True)
        proxy, catalog, output = self.root / "proxy.dll", self.root / "catalog.bin", self.root / "result"
        proxy.write_bytes(b"fixture proxy");catalog.write_bytes(b"T3LCAT03old catalog")
        resolved = tuple(p.resolve() for p in (engine, bottle, system, proxy, output))
        for option in ("wall_displacement", "preview_gi"):
            with self.subTest(option=option), mock.patch.object(batch, "validate_paths", return_value=resolved), \
                 mock.patch.object(batch.subprocess, "Popen") as popen:
                with self.assertRaisesRegex(batch.BatchError, "v6"):
                    batch.execute(*map(str, resolved), delay=0, timeout=10, frames=1, terminate=True,
                                  scene_bridge=True, submission_scope=True, live_catalog=str(catalog), **{option: True})
            popen.assert_not_called()
            self.assertFalse(output.exists())

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
             mock.patch.object(batch, "game_pids", side_effect=[[], [], [42], [42], [42], [42]]), \
             mock.patch.object(batch, "wait_for_one_game", return_value=42), \
             mock.patch.object(batch, "process_start", return_value="same"), \
             mock.patch.object(batch, "terminate_game", side_effect=batch.BatchError("process identity changed")), \
             mock.patch.object(batch, "complete_trace", return_value=False), \
             mock.patch.object(batch.subprocess, "Popen"):
            with self.assertRaises(batch.BatchError):
                batch.execute(*map(str, resolved), delay=0, timeout=0.01, frames=1, terminate=True)
        self.assertTrue((system / "d3d8.dll").exists())
        self.assertFalse(json.loads((output / "deployment.json").read_text())["temporary_proxy_removed"])
        with mock.patch.object(batch, "game_pids", return_value=[]):
            batch.cleanup(output / "deployment.json")
        self.assertFalse((system / "d3d8.dll").exists())

    def test_analysis_failure_closes_same_game_before_removing_proxy(self):
        engine, bottle = self.root / "engine", self.root / "bottle"
        system = bottle / "drive_c/Steam/Thief/System"
        system.mkdir(parents=True)
        proxy = self.root / "proxy.dll"
        proxy.write_bytes(b"fixture proxy")
        output = self.root / "result"
        resolved = tuple(p.resolve() for p in (engine, bottle, system, proxy, output))
        with mock.patch.object(batch, "validate_paths", return_value=resolved), \
             mock.patch.object(batch, "game_pids", side_effect=[[], [], [42], [42], []]), \
             mock.patch.object(batch, "wait_for_one_game", return_value=42), \
             mock.patch.object(batch, "process_start", return_value="same"), \
             mock.patch.object(batch, "terminate_game") as terminate, \
             mock.patch.object(batch, "loader_verified", return_value=True), \
             mock.patch.object(batch, "complete_trace", return_value=True), \
             mock.patch.object(batch.analyze_tds_capture, "analyze", side_effect=ValueError("invalid trace")), \
             mock.patch.object(batch.subprocess, "Popen"):
            with self.assertRaises(batch.BatchError):
                batch.execute(*map(str, resolved), delay=0, timeout=10, frames=1, terminate=True)
        terminate.assert_called_once()
        self.assertEqual(terminate.call_args.args[1:], (42, "same"))
        self.assertFalse((system / "d3d8.dll").exists())
        self.assertTrue(json.loads((output / "deployment.json").read_text())["temporary_proxy_removed"])


if __name__ == "__main__":
    unittest.main()
