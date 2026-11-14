"""Host validation for the synthetic device-facade comparison runner."""
from pathlib import Path
import copy
import json
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import run_d3d8_device_facade as probe


def fixture_log(mode, buffer_facade=False, resource_facade=False):
    lines = [f"parity {key}={'0x00000000' if value is None else '22' if value == 'format' else value}"
             for key, value in probe.expected_parity()]
    suffix = "PASS" if mode == "facade" else "not_applicable"
    lines.append(f"factory_and_device_QI_roundtrip=PASS factory_initial_release={suffix}")
    if resource_facade:
        native_private = ("missing=0x88760866 set=0x00000000 query=0x00000000 query_size=4 "
                          "small=0x88760867 small_size=4 get=0x00000000 match=1 free=0x00000000 after_free=0x88760866 "
                          "object_set=0x00000000 object_get=0x00000000 object_same=1 "
                          "object_free=0x00000000 refs=2,3,2,1")
        facade_private = ("missing=0x80004001 set=0x00000000 query=0x00000000 query_size=4 "
                          "small=0x88760867 small_size=4 get=0x00000000 match=1 free=0x00000000 after_free=0x88760866 "
                          "object_set=0x80004001 object_get=0x80004001 object_same=0 "
                          "object_free=0x80004001 refs=1,1,1,1")
        private = native_private if mode == "baseline" else facade_private
        lines.extend(f"private_data kind={kind} {private}" for kind in probe.PRIVATE_KINDS)
        lines.append("private_data_alias set=0x00000000 get=0x00000000 same=1 parent=1 free=0x00000000")
        lines.append("private_data_index_alias set=0x00000000 get=0x00000000 same=1 parent=1")
        lines.append("private_resource_self_and_cross_aliases=PASS")
        lines.append("private_buffer_sources=PASS")
        lines.append("private_lifetime_native_dependent_release=PASS" if mode == "baseline" else
                     "private_lifetime_naive_cycle_and_cleanup=PASS")
        lines.append("private_translated_lifetime=PASS")
        lines.append("private_buffer_sources_reset=PASS")
        lines.append("private_external_naive_cycle_and_cleanup=PASS" if mode == "facade" else
                     "private_external_native_dependent_release=PASS")
        lines.append("private_external_eager_release_and_cleanup=PASS" if mode == "facade" else
                     "private_external_eager_native_dependent_release=PASS")
        lines.append("private_external_eager_reacquire_provenance=preserved")
        lines.append("private_external_eager_stateblock_and_resource=PASS")
        lines.append("eager_buffer_lifetime=PASS shadow=unavailable provenance=unavailable" if mode == "facade" else
                     "eager_buffer_lifetime=PASS shadow=not_observed provenance=preserved")
        lines.append("persistent_eager_buffer=PASS shadow=preserved provenance=preserved reset=unknown" if mode == "facade" else
                     "persistent_eager_buffer=PASS shadow=not_observed provenance=preserved reset=not_observed")
        if mode == "facade":
            lines.append("prepared_eager_buffer=PASS")
            lines.append("prepared_capture_draw=PASS")
            lines.append("combined_capture_shader_pixel=PASS")
            lines.append("combined_external_shader_invalidation=PASS")
            lines.append("combined_reset_shader_and_fresh_buffer_epoch=PASS")
            lines.append("combined_stateblock_shader_buffer_reacquisition=PASS")
            lines.append("combined_stateblock_shader_reuse_invalidates=PASS")
            lines.append("combined_unknown_stateblocks_invalidate=PASS")
        lines.append("private_cross_device_cycle=PASS")
        lines.append("shader_stateblock_lifetime delete=0x00000000 create=0x00000000 apply=0x00000000 get=0x00000000 "
                     "same_handle=1 bound_old=1 bound_new=1 function_hr=0x00000000 function=replacement "
                     "before_draw=1 before_pixel=0xe43b3b draw=1 pixel=0x00ff00 cleanup=PASS")
        lines.append("shader_bound_deletion delete=0x00000000 create=0x00000000 get=0x00000000 same_handle=1 "
                     "bound_new=1 retained_draw=1 retained_pixel=0x00ff00 replacement_draw=1 replacement_pixel=0xe43b3b cleanup=PASS")
        lines.extend(("resource_facade_textured_pixels=PASS", "resource_facade_stateblock_texture_identities=PASS",
                      "resource_facade_volume_roundtrips=PASS",
                      "resource_facade_surface_copy_and_target=PASS", "resource_facade_swapchain_roundtrips=PASS",
                      "resource_facade_swapchain_container_hr=0x80004002",
                      "resource_facade_swapchain_device_hr=0x80004002",
                      "resource_facade_swapchain_container_device_hr=0x00000000",
                      "resource_facade_reset_hr=0x00000000", "resource_facade_reset_pixel=0xe43b3b",
                      "resource_facade_reset_roundtrips=PASS", "resource_facade_live_reset_hr=0x80004005",
                      "resource_facade_recovery_reset_hr=0x00000000", "resource_facade_live_reset_roundtrips=PASS"))
    if mode == "facade":
        names = (() if resource_facade else ("surface", "texture") if buffer_facade else
                 ("surface", "texture", "vertex_buffer", "index_buffer"))
        lines.extend(f"resource_{name}_native_device_alias=PASS hr=0x00000000" for name in names)
        if buffer_facade or resource_facade:
            lines.extend(("buffer_parent_facade_roundtrip=PASS", "buffer_shadow_writeonly_and_draw_window=PASS",
                          "buffer_stateblock_retained_identity=PASS", "buffer_facade_final_close=PASS"))
            if resource_facade:
                lines.extend(("resource_get_device_boundary=facade_for_tested_types; other_types=unverified",
                              "resource_facade_2d_cube_surface_roundtrips=PASS", "resource_facade_cube_binding=PASS",
                              "resource_facade_device_unwraps_buffers=PASS", "resource_facade_rejects_unknown_inputs=PASS",
                              "resource_facade_post_reset_buffer_epoch=PASS", "resource_facade_recovery_buffer_epoch=PASS",
                              "resource_facade_stateblock_buffer_shadows=PASS", "resource_facade_failed_stateblock_invalidates=PASS",
                              "resource_facade_final_close=PASS"))
            else:
                lines.append("resource_get_device_boundary=native_nonbuffer; wrapped_buffers=facade")
        else:
            lines.append("resource_get_device_boundary=native_device; wrapped_buffers=not_tested")
    lines.append(f"device_facade_fixture=PASS mode={mode}")
    return lines


def fixture_ppm(resource_facade=False):
    data = bytearray(probe.HEADER + bytes(256 * 256 * 3))
    samples = [(30, 30, (228, 59, 59)), (225, 30, (53, 198, 101)),
               (30, 225, (67, 138, 240)), (225, 160, (230, 201, 72))]
    if resource_facade:
        samples.extend(((230, 210, (18, 52, 208)), (160, 210, (36, 228, 206))))
    for x, y, value in samples:
        index = len(probe.HEADER) + (y * 256 + x) * 3
        data[index:index + 3] = bytes(value)
    return bytes(data)


class DeviceFacadeRunnerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="t3render-device-facade-test-")
        self.addCleanup(self.directory.cleanup)
        self.log = Path(self.directory.name) / "fixture.log"
        self.ppm = Path(self.directory.name) / "fixture.ppm"

    def test_prepared_trace_checks_bytes_and_paired_stability(self):
        vertices = b"".join(struct.pack("<ffffI", x, y, 0.5, 1, 0xffe43b3b)
                            for x, y in ((-0.8, 0.8), (0.8, 0.8), (-0.8, -0.8)))
        records = [{"event": "header", "replayable": False, "operation_results": 1, "buffer_capture": {"requested": True},
                    "declaration_capture": {"requested": True}, "vertex_program_capture": {"requested": True},
                    "vertex_constant_capture": {"requested": True}},
                   {"event": "frame_begin", "frame": 1,
                    "presentation": {"width": 256, "height": 256, "format": 22, "multisample": 0,
                                     "count": 1, "windowed": True}},
                   {"event": "clear", "frame": 1, "op_id": 1, "rect_count": 0, "rects_status": "none",
                    "rects": [], "flags": 1, "color": 0xff111318},
                   {"event": "clear_result", "frame": 1, "op_id": 1, "hr": 0},
                   {"event": "draw", "frame": 1, "op_id": 2, "call": "DrawIndexedPrimitive",
                    "primitive_type": 4, "primitive_count": 1, "min_vertex_index": 0, "num_vertices": 3,
                    "start_index": 0, "vertex_shader": 0xf0000002, "pixel_shader": 0,
                    "viewport": {"x": 0, "y": 0, "width": 256, "height": 256, "min_z": 0, "max_z": 1},
                    "render_states": dict(probe.trace_to_up_packet.REQUIRED_RENDER_STATES),
                    "stages": [{"stage": 0, "texture": None, "states": dict(probe.trace_to_up_packet.STAGE0_STATES)},
                               {"stage": 1, "texture": None, "states": {"1": 1}}],
                    "streams": [{"stream": 0, "hr": 0, "stride": 20, "buffer": "vb"}],
                    "indices": {"hr": 0, "base_vertex_index": 0, "buffer": "ib"},
                    "vertex_declaration": {"status": "candidate", "generation": 1,
                                           "words": [0x20000000, 0x40030000, 0x40040005, 0xffffffff]},
                    "vertex_program": {"status": "candidate", "generation": 1, "version": "vs_1_1",
                                       "words": [0xfffe0101, 1, 0xc00f0000, 0x90e40000, 1, 0xd00f0000, 0xa0e40000, 0xffff]},
                    "vertex_constants": {"status": "sampled", "start_register": 0, "count": 1,
                                         "getter_hr": 0, "verify_hr": 0, "bits": [0, 0x3f800000, 0, 0x3f800000]},
                    "buffer_data": {"source": "application_lock_shadow",
                        "vertex": {"status": "captured", "data": vertices.hex(), "byte_count": 60, "resource": "vb",
                                   "first_vertex": 0, "vertex_count": 3, "stride": 20, "byte_offset": 0},
                        "index": {"status": "captured", "data": struct.pack("<3H", 0, 1, 2).hex(), "resource": "ib",
                                  "byte_count": 6, "index_count": 3, "index_format": 101, "byte_offset": 0}}},
                   {"event": "draw_result", "frame": 1, "op_id": 2, "hr": 0,
                    "buffer_stable": True, "shader_stable": True},
                   {"event": "frame_end", "frame": 1, "present_hr": 0, "draws": 1, "replayable": False,
                    "concurrent_operations": False, "had_reset": False},
                   {"event": "capture_end", "frames": 1}]
        path = Path(self.directory.name) / "trace.jsonl"
        def inspect(value):
            path.write_text("".join(json.dumps(record) + "\n" for record in value), encoding="utf-8")
            probe.inspect_prepared_trace(path)
        inspect(records)
        with self.assertRaises(probe.RunError):
            probe.prepared_replay_packet(records)
        packet = probe.prepared_replay_packet(records, synthetic_fixture=True).splitlines()
        self.assertEqual(packet[:3], ["T3R_XYZ_V1", "256 256", str(0xff111318)])
        self.assertEqual(len(packet), 9)
        self.assertTrue(all(line.endswith(str(0xff00ff00)) for line in packet[6:]))
        for change in ("state", "binding", "viewport", "reset", "order"):
            bad = copy.deepcopy(records)
            if change == "state": bad[4]["render_states"]["27"] = 1
            if change == "binding": bad[4]["streams"][0]["buffer"] = "other"
            if change == "viewport": bad[4]["viewport"]["width"] = 128
            if change == "reset": bad[6]["had_reset"] = True
            if change == "order": bad[2]["op_id"] = bad[3]["op_id"] = 3
            with self.subTest(change=change), self.assertRaises((probe.RunError, ValueError)):
                probe.prepared_replay_packet(bad, synthetic_fixture=True)
        for change in ("missing", "vertex", "index", "stable", "result", "frame", "replayable", "shader", "constant"):
            bad = copy.deepcopy(records)
            if change == "missing": bad.pop()
            if change == "vertex": bad[4]["buffer_data"]["vertex"]["data"] = "00" * 60
            if change == "index": bad[4]["buffer_data"]["index"]["data"] = "000000000000"
            if change == "stable": bad[5]["buffer_stable"] = False
            if change == "result": bad[5]["op_id"] = 3
            if change == "frame": bad[6]["present_hr"] = -1
            if change == "replayable": bad[0]["replayable"] = True
            if change == "shader": bad[4]["vertex_program"]["words"][0] = 0
            if change == "constant": bad[4]["vertex_constants"]["bits"][1] = 0
            with self.subTest(change=change), self.assertRaises(probe.RunError):
                inspect(bad)
    def inspect(self, lines, mode="facade", buffer_facade=False, resource_facade=False):
        self.log.write_text("noise from Wine\n" + "\n".join(lines) + "\n", encoding="utf-8")
        return probe.inspect_log(self.log, mode, buffer_facade, resource_facade)

    def test_prepared_image_requires_shader_pixel(self):
        image = bytearray(probe.HEADER + b"\x11\x13\x18" * (256 * 256))
        for y in range(40, 90):
            for x in range(40, 90):
                pos = len(probe.HEADER) + (y * 256 + x) * 3
                image[pos:pos + 3] = b"\x00\xff\x00"
        pixel = len(probe.HEADER) + (64 * 256 + 64) * 3
        image[pixel:pixel + 3] = b"\x00\xff\x00"
        self.ppm.write_bytes(image)
        probe.inspect_prepared_image(self.ppm)
        image[pixel:pixel + 3] = b"\xff\x00\x00"
        self.ppm.write_bytes(image)
        with self.assertRaises(probe.RunError):
            probe.inspect_prepared_image(self.ppm)

    def test_shader_stateblock_observation_requires_consistent_complete_record(self):
        valid = next(line for line in fixture_log("facade", resource_facade=True)
                     if line.startswith("shader_stateblock_lifetime "))
        observation = probe.inspect_shader_stateblock([valid])
        self.assertTrue(observation["same_handle"])
        self.assertEqual(observation["function"], "replacement")
        for lines in ([], [valid, valid], [valid.replace("cleanup=PASS", "cleanup=FAIL")],
                      [valid.replace("create=0x00000000", "create=0x80004005")],
                      [valid.replace("bound_new=1", "bound_new=0")]):
            with self.subTest(lines=lines), self.assertRaises(probe.RunError):
                probe.inspect_shader_stateblock(lines)

    def test_bound_shader_deletion_requires_consistent_record(self):
        valid = next(line for line in fixture_log("facade", resource_facade=True)
                     if line.startswith("shader_bound_deletion "))
        self.assertEqual(probe.inspect_bound_shader_deletion([valid])["replacement_pixel"], "0xe43b3b")
        for lines in ([], [valid, valid], [valid.replace("cleanup=PASS", "cleanup=FAIL")],
                      [valid.replace("bound_new=1", "bound_new=0")],
                      [valid.replace("retained_draw=1", "retained_draw=0")]):
            with self.subTest(lines=lines), self.assertRaises(probe.RunError):
                probe.inspect_bound_shader_deletion(lines)

    def test_private_data_gap_requires_ordered_native_and_facade_evidence(self):
        for mode in ("baseline", "facade"):
            original = fixture_log(mode, resource_facade=True)
            self.inspect(original, mode, resource_facade=True)
            marker = next(line for line in original if line.startswith("private_data kind=vertex "))
            alterations = ([line for line in original if line != marker], original + [marker],
                           [line.replace(marker, marker.replace("refs=", "refs=9,")) for line in original],
                           [line.replace(marker, marker.replace(
                               "object_same=1" if mode == "baseline" else "object_same=0",
                               "object_same=0" if mode == "baseline" else "object_same=1")) for line in original])
            for changed in alterations:
                with self.subTest(mode=mode), self.assertRaises(probe.RunError):
                    self.inspect(changed, mode, resource_facade=True)
            alias = next(line for line in original if line.startswith("private_data_alias "))
            for changed in ([line for line in original if line != alias], original + [alias],
                            [line.replace(alias, alias.replace("parent=", "parent=9")) for line in original]):
                with self.subTest(mode=mode), self.assertRaises(probe.RunError):
                    self.inspect(changed, mode, resource_facade=True)
            index_alias = next(line for line in original if line.startswith("private_data_index_alias "))
            for changed in ([line for line in original if line != index_alias], original + [index_alias],
                            [line.replace(index_alias, index_alias.replace("parent=1", "parent=0")) for line in original]):
                with self.subTest(mode=mode), self.assertRaises(probe.RunError):
                    self.inspect(changed, mode, resource_facade=True)
            resources = "private_resource_self_and_cross_aliases=PASS"
            for changed in ([line for line in original if line != resources], original + [resources],
                            [line.replace(resources, "private_resource_self_and_cross_aliases=FAIL") for line in original]):
                with self.subTest(mode=mode), self.assertRaises(probe.RunError):
                    self.inspect(changed, mode, resource_facade=True)
            buffer_sources = "private_buffer_sources=PASS"
            for changed in ([line for line in original if line != buffer_sources], original + [buffer_sources],
                            [line.replace(buffer_sources, "private_buffer_sources=FAIL") for line in original]):
                with self.subTest(mode=mode), self.assertRaises(probe.RunError):
                    self.inspect(changed, mode, resource_facade=True)
            lifetime = next(line for line in original if line.startswith("private_lifetime_"))
            for changed in ([line for line in original if line != lifetime], original + [lifetime],
                            [line.replace(lifetime, lifetime.replace("=PASS", "=FAIL")) for line in original]):
                with self.subTest(mode=mode), self.assertRaises(probe.RunError):
                    self.inspect(changed, mode, resource_facade=True)
            translated = "private_translated_lifetime=PASS"
            for changed in ([line for line in original if line != translated], original + [translated],
                            [line.replace(translated, "private_translated_lifetime=FAIL") for line in original]):
                with self.subTest(mode=mode), self.assertRaises(probe.RunError):
                    self.inspect(changed, mode, resource_facade=True)
            buffer_reset = "private_buffer_sources_reset=PASS"
            for changed in ([line for line in original if line != buffer_reset], original + [buffer_reset],
                            [line.replace(buffer_reset, "private_buffer_sources_reset=FAIL") for line in original]):
                with self.subTest(mode=mode), self.assertRaises(probe.RunError):
                    self.inspect(changed, mode, resource_facade=True)
            external = next(line for line in original if line.startswith("private_external_"))
            for changed in ([line for line in original if line != external], original + [external],
                            [line.replace(external, external.replace("=PASS", "=FAIL")) for line in original]):
                with self.subTest(mode=mode), self.assertRaises(probe.RunError):
                    self.inspect(changed, mode, resource_facade=True)
            eager = next(line for line in original if line.startswith("private_external_eager_") and
                         not line.startswith(("private_external_eager_reacquire_provenance=", "private_external_eager_stateblock_and_resource=")))
            provenance = next(line for line in original if line.startswith("private_external_eager_reacquire_provenance="))
            stateblock = "private_external_eager_stateblock_and_resource=PASS"
            for marker in (eager, provenance, stateblock):
                for changed in ([line for line in original if line != marker], original + [marker],
                                [line.replace(marker, marker + "bad") for line in original]):
                    with self.subTest(mode=mode), self.assertRaises(probe.RunError):
                        self.inspect(changed, mode, resource_facade=True)
            eager_buffer = next(line for line in original if line.startswith("eager_buffer_lifetime="))
            for changed in ([line for line in original if line != eager_buffer], original + [eager_buffer],
                            [line.replace(eager_buffer, eager_buffer + "bad") for line in original]):
                with self.subTest(mode=mode), self.assertRaises(probe.RunError):
                    self.inspect(changed, mode, resource_facade=True)
            persistent = next(line for line in original if line.startswith("persistent_eager_buffer="))
            for changed in ([line for line in original if line != persistent], original + [persistent],
                            [line.replace(persistent, persistent + "bad") for line in original]):
                with self.subTest(mode=mode), self.assertRaises(probe.RunError):
                    self.inspect(changed, mode, resource_facade=True)
            if mode == "facade":
                for prepared in ("prepared_eager_buffer=PASS", "prepared_capture_draw=PASS", "combined_capture_shader_pixel=PASS",
                                 "combined_external_shader_invalidation=PASS", "combined_reset_shader_and_fresh_buffer_epoch=PASS",
                                 "combined_stateblock_shader_buffer_reacquisition=PASS", "combined_stateblock_shader_reuse_invalidates=PASS",
                                 "combined_unknown_stateblocks_invalidate=PASS"):
                    for changed in ([line for line in original if line != prepared], original + [prepared],
                                    [line.replace(prepared, prepared + "bad") for line in original]):
                        with self.subTest(mode=mode), self.assertRaises(probe.RunError):
                            self.inspect(changed, mode, resource_facade=True)
            else:
                for prepared in ("prepared_eager_buffer=PASS", "prepared_capture_draw=PASS", "combined_capture_shader_pixel=PASS",
                                 "combined_external_shader_invalidation=PASS", "combined_reset_shader_and_fresh_buffer_epoch=PASS",
                                 "combined_stateblock_shader_buffer_reacquisition=PASS", "combined_stateblock_shader_reuse_invalidates=PASS",
                                 "combined_unknown_stateblocks_invalidate=PASS"):
                    with self.subTest(mode=mode), self.assertRaises(probe.RunError):
                        self.inspect(original + [prepared], mode, resource_facade=True)
            cross_device = "private_cross_device_cycle=PASS"
            for changed in ([line for line in original if line != cross_device], original + [cross_device],
                            [line.replace(cross_device, "private_cross_device_cycle=FAIL") for line in original]):
                with self.subTest(mode=mode), self.assertRaises(probe.RunError):
                    self.inspect(changed, mode, resource_facade=True)

    def test_private_blob_hresult_differences_are_compared(self):
        native = fixture_log("baseline", resource_facade=True)
        facade = fixture_log("facade", resource_facade=True)
        baseline = self.inspect(native, "baseline", resource_facade=True)
        changed = [line.replace("small=0x88760867", "small=0x80004005") if line.startswith("private_data kind=vertex ") else line
                   for line in facade]
        self.assertNotEqual(baseline, self.inspect(changed, resource_facade=True))

    def test_resource_facade_requires_each_roundtrip_and_final_close(self):
        baseline = self.inspect(fixture_log("baseline", resource_facade=True), "baseline", resource_facade=True)
        wrapped = fixture_log("facade", resource_facade=True)
        self.assertEqual(baseline, self.inspect(wrapped, resource_facade=True))
        for marker in ("resource_facade_textured_pixels=PASS", "resource_facade_stateblock_texture_identities=PASS",
                       "resource_facade_volume_roundtrips=PASS",
                       "resource_facade_swapchain_roundtrips=PASS",
                       "resource_facade_reset_pixel=0xe43b3b", "resource_facade_reset_roundtrips=PASS",
                       "resource_facade_live_reset_roundtrips=PASS",
                       "resource_facade_surface_copy_and_target=PASS", "resource_facade_rejects_unknown_inputs=PASS",
                       "resource_facade_post_reset_buffer_epoch=PASS", "resource_facade_recovery_buffer_epoch=PASS",
                       "resource_facade_stateblock_buffer_shadows=PASS", "resource_facade_failed_stateblock_invalidates=PASS",
                       "resource_facade_2d_cube_surface_roundtrips=PASS", "resource_facade_cube_binding=PASS",
                       "resource_facade_device_unwraps_buffers=PASS", "resource_facade_final_close=PASS"):
            for changed in ([line for line in wrapped if line != marker], wrapped + [marker],
                            wrapped + [marker.replace("=PASS", "=FAIL")]):
                with self.subTest(marker=marker, changed=changed[-1]):
                    with self.assertRaises(probe.RunError):
                        self.inspect(changed, resource_facade=True)

    def test_swapchain_container_hresult_is_required_and_compared(self):
        baseline = fixture_log("baseline", resource_facade=True)
        wrapped = fixture_log("facade", resource_facade=True)
        for name in ("container", "device", "container_device"):
            prefix = f"resource_facade_swapchain_{name}_hr="
            marker = prefix + ("0x00000000" if name == "container_device" else "0x80004002")
            for changed in ([line for line in wrapped if line != marker], wrapped + [marker],
                            [line.replace(marker, prefix + "bad") for line in wrapped]):
                with self.assertRaises(probe.RunError):
                    self.inspect(changed, resource_facade=True)
            baseline_values = self.inspect(baseline, "baseline", resource_facade=True)
            other = "0x80004002" if name == "container_device" else "0x00000000"
            alternate = [line.replace(marker, prefix + other) for line in wrapped]
            self.assertNotEqual(baseline_values, self.inspect(alternate, resource_facade=True))

    def test_reset_hresult_is_required_successful_and_compared(self):
        baseline = fixture_log("baseline", resource_facade=True)
        wrapped = fixture_log("facade", resource_facade=True)
        marker = "resource_facade_reset_hr=0x00000000"
        for changed in ([line for line in wrapped if line != marker], wrapped + [marker],
                        [line.replace(marker, "resource_facade_reset_hr=0x80004005") for line in wrapped]):
            with self.assertRaises(probe.RunError):
                self.inspect(changed, resource_facade=True)
        other = [line.replace(marker, "resource_facade_reset_hr=0x00000001") for line in wrapped]
        self.assertNotEqual(self.inspect(baseline, "baseline", resource_facade=True),
                            self.inspect(other, resource_facade=True))

    def test_live_and_recovery_reset_results_are_distinct(self):
        baseline = fixture_log("baseline", resource_facade=True)
        for name, value in (("live", "0x80004005"), ("recovery", "0x00000000")):
            marker = f"resource_facade_{name}_reset_hr={value}"
            for changed in ([line for line in baseline if line != marker], baseline + [marker],
                            [line.replace(marker, f"resource_facade_{name}_reset_hr=bad") for line in baseline]):
                with self.assertRaises(probe.RunError):
                    self.inspect(changed, "baseline", resource_facade=True)
        failed_recovery = [line.replace("resource_facade_recovery_reset_hr=0x00000000",
                                        "resource_facade_recovery_reset_hr=0x80004005") for line in baseline]
        with self.assertRaises(probe.RunError):
            self.inspect(failed_recovery, "baseline", resource_facade=True)

    def test_buffer_facade_requires_parent_and_writeonly_evidence(self):
        baseline = self.inspect(fixture_log("baseline"), "baseline", True)
        wrapped = fixture_log("facade", True)
        self.assertEqual(baseline, self.inspect(wrapped, buffer_facade=True))
        for marker in ("buffer_parent_facade_roundtrip=PASS", "buffer_shadow_writeonly_and_draw_window=PASS",
                       "buffer_stateblock_retained_identity=PASS",
                       "buffer_facade_final_close=PASS",
                       "resource_get_device_boundary=native_nonbuffer; wrapped_buffers=facade"):
            for replacement in ([], [marker, marker], [marker + "_INVALID"]):
                with self.subTest(marker=marker, replacement=replacement):
                    lines = [line for line in wrapped if line != marker] + replacement
                    with self.assertRaises(probe.RunError):
                        self.inspect(lines, buffer_facade=True)

    def test_baseline_and_facade_require_the_same_state_and_results(self):
        self.assertEqual(self.inspect(fixture_log("baseline"), "baseline"), self.inspect(fixture_log("facade")))

    def test_incomplete_failed_or_conflicting_results_are_rejected(self):
        for mutation in (
            lambda lines: lines.pop(0),
            lambda lines: lines.insert(0, lines[0]),
            lambda lines: lines.__setitem__(0, "parity GetAdapterDisplayMode=0x80004005"),
            lambda lines: lines.remove("parity DrawPrimitiveUP=0x00000000"),
            lambda lines: lines.__setitem__(next(i for i, line in enumerate(lines) if line.startswith("parity World=")),
                                             "parity World=" + "0" * 128),
            lambda lines: lines.__setitem__(next(i for i, line in enumerate(lines) if line.startswith("parity FVF=")), "parity FVF=0"),
            lambda lines: lines.remove("resource_surface_native_device_alias=PASS hr=0x00000000"),
            lambda lines: lines.append("resource_surface_native_device_alias=PASS hr=0x00000000"),
            lambda lines: lines.append("resource_surface_native_device_alias=FAIL hr=0x80004005"),
            lambda lines: lines.append("device_facade_fixture=FAIL mode=facade"),
        ):
            with self.subTest(mutation=mutation):
                lines = fixture_log("facade")
                mutation(lines)
                with self.assertRaises(probe.RunError):
                    self.inspect(lines)

    def test_ppm_requires_expected_dimensions_and_triangle_samples(self):
        valid = fixture_ppm()
        self.ppm.write_bytes(valid)
        self.assertEqual(probe.read_ppm(self.ppm), valid)
        for data in (valid[:-1], valid + b"extra", b"X" + valid[1:],
                     valid[:len(probe.HEADER)] + bytes(len(valid) - len(probe.HEADER))):
            self.ppm.write_bytes(data)
            with self.assertRaises(probe.RunError):
                probe.read_ppm(self.ppm)

    def test_resource_ppm_requires_2d_and_cube_texture_samples(self):
        valid = fixture_ppm(True)
        self.ppm.write_bytes(valid)
        self.assertEqual(probe.read_ppm(self.ppm, True), valid)
        for x, y in ((230, 210), (160, 210)):
            modified = bytearray(valid)
            offset = len(probe.HEADER) + (y * 256 + x) * 3
            modified[offset:offset + 3] = b"\0\0\0"
            self.ppm.write_bytes(modified)
            with self.assertRaises(probe.RunError):
                probe.read_ppm(self.ppm, True)


if __name__ == "__main__":
    unittest.main()
