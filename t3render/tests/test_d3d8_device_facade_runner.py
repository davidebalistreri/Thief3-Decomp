"""Host validation for the synthetic device-facade comparison runner."""
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import run_d3d8_device_facade as probe


def fixture_log(mode, buffer_facade=False):
    lines = [f"parity {key}={'0x00000000' if value is None else '22' if value == 'format' else value}"
             for key, value in probe.expected_parity()]
    suffix = "PASS" if mode == "facade" else "not_applicable"
    lines.append(f"factory_and_device_QI_roundtrip=PASS factory_initial_release={suffix}")
    if mode == "facade":
        names = ("surface", "texture") if buffer_facade else ("surface", "texture", "vertex_buffer", "index_buffer")
        lines.extend(f"resource_{name}_native_device_alias=PASS hr=0x00000000" for name in names)
        if buffer_facade:
            lines.extend(("resource_get_device_boundary=native_nonbuffer; wrapped_buffers=facade",
                          "buffer_parent_facade_roundtrip=PASS", "buffer_shadow_writeonly_and_draw_window=PASS",
                          "buffer_stateblock_retained_fail_closed=PASS"))
        else:
            lines.append("resource_get_device_boundary=native_device; wrapped_buffers=not_tested")
    lines.append(f"device_facade_fixture=PASS mode={mode}")
    return lines


def fixture_ppm():
    data = bytearray(probe.HEADER + bytes(256 * 256 * 3))
    for x, y, value in ((30, 30, (228, 59, 59)), (225, 30, (53, 198, 101)),
                        (30, 225, (67, 138, 240)), (225, 160, (230, 201, 72))):
        index = len(probe.HEADER) + (y * 256 + x) * 3
        data[index:index + 3] = bytes(value)
    return bytes(data)


class DeviceFacadeRunnerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="t3render-device-facade-test-")
        self.addCleanup(self.directory.cleanup)
        self.log = Path(self.directory.name) / "fixture.log"
        self.ppm = Path(self.directory.name) / "fixture.ppm"

    def inspect(self, lines, mode="facade", buffer_facade=False):
        self.log.write_text("noise from Wine\n" + "\n".join(lines) + "\n", encoding="utf-8")
        return probe.inspect_log(self.log, mode, buffer_facade)

    def test_buffer_facade_requires_parent_and_writeonly_evidence(self):
        baseline = self.inspect(fixture_log("baseline"), "baseline", True)
        wrapped = fixture_log("facade", True)
        self.assertEqual(baseline, self.inspect(wrapped, buffer_facade=True))
        for marker in ("buffer_parent_facade_roundtrip=PASS", "buffer_shadow_writeonly_and_draw_window=PASS",
                       "buffer_stateblock_retained_fail_closed=PASS",
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


if __name__ == "__main__":
    unittest.main()
