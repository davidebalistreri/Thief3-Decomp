"""Host validation for the synthetic device-facade comparison runner."""
from pathlib import Path
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
        lines.append("private_data_alias set=0x00000000 get=0x00000000 same=1 parent=1 free=0x00000000" if mode == "baseline" else
                     "private_data_alias set=0x80004001 get=0x80004001 same=0 parent=0 free=0x80004001")
        lines.append("private_lifetime_native_dependent_release=PASS" if mode == "baseline" else
                     "private_lifetime_naive_cycle_and_cleanup=PASS")
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

    def inspect(self, lines, mode="facade", buffer_facade=False, resource_facade=False):
        self.log.write_text("noise from Wine\n" + "\n".join(lines) + "\n", encoding="utf-8")
        return probe.inspect_log(self.log, mode, buffer_facade, resource_facade)

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
            lifetime = next(line for line in original if line.startswith("private_lifetime_"))
            for changed in ([line for line in original if line != lifetime], original + [lifetime],
                            [line.replace(lifetime, lifetime.replace("=PASS", "=FAIL")) for line in original]):
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
