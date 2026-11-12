"""Host checks of test-only state-block experiment evidence."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_d3d8_stateblock_runner import trace_fixture
import run_d3d8_stateblock_experiment as experiment


def records(case, experimental):
    events = trace_fixture()
    draw, result = events[4], events[5]
    stable = case == "control" or (experimental and case != "reacquire")
    status = "captured" if stable and case != "lifetime" else "unknown" if stable else "window_unavailable"
    if case in ("lifetime", "reacquire"):
        draw["streams"][0]["description"]["size"] = 80
        draw["indices"]["description"]["size"] = 8
    vertices = b"".join(struct.pack("<4fI", *v) for v in (
        (25.6, 217.6, 0, 1, 0xffff0000), (128, 25.6, 0, 1, 0xffff0000),
        (230.4, 217.6, 0, 1, 0xffff0000)))
    draw["buffer_data"]["vertex"].update(status=status, data=vertices.hex() if status == "captured" else None)
    draw["buffer_data"]["index"].update(status=status, data=struct.pack("<3H", 0, 1, 2).hex() if status == "captured" else None)
    result["buffer_stable"] = stable
    return events


class StateBlockExperimentRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-stateblock-experiment-test-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "trace.jsonl"

    def inspect(self, events, case, experimental):
        self.path.write_text("".join(json.dumps(e) + "\n" for e in events), encoding="utf-8")
        return experiment.inspect_trace(self.path, case, experimental)

    def test_expected_states_for_all_cases_and_both_policies(self):
        for case in experiment.CASES:
            for experimental in (False, True):
                with self.subTest(case=case, experimental=experimental):
                    expected = records(case, experimental)
                    observation = self.inspect(expected, case, experimental)
                    self.assertEqual(observation["buffer_stable"], expected[5]["buffer_stable"])
                    self.assertEqual(observation["vertex_status"], expected[4]["buffer_data"]["vertex"]["status"])

    def test_rejects_inconsistent_stability_ranges_or_bytes(self):
        for case in ("stable", "vertex", "index", "binding", "missing", "desc", "null_identity"):
            events = records("apply", True)
            if case == "stable": events[5]["buffer_stable"] = False
            if case == "vertex": events[4]["buffer_data"]["vertex"]["data"] = "00" * 60
            if case == "index": events[4]["buffer_data"]["index"]["data"] = "000000000000"
            if case == "binding": events[4]["buffer_data"]["index"]["resource"] = "0x999"
            if case == "missing": del events[4]["buffer_data"]
            if case == "desc": events[4]["streams"][0]["description"]["size"] = 80
            if case == "null_identity":
                events[4]["streams"][0]["buffer"] = "0x0"
                events[4]["buffer_data"]["vertex"]["resource"] = "0x0"
            with self.subTest(case=case), self.assertRaises(experiment.RunError):
                self.inspect(events, "apply", True)

    def test_rejects_invalid_range_metadata_for_each_shadow_status(self):
        fields = (("vertex", "stream"), ("vertex", "first_vertex"), ("vertex", "vertex_count"),
                  ("vertex", "stride"), ("vertex", "byte_offset"), ("vertex", "byte_count"),
                  ("index", "index_count"), ("index", "index_format"),
                  ("index", "byte_offset"), ("index", "byte_count"))
        # Captured, unavailable and unknown observations must all describe
        # the same fixture ranges, including the oversized retained buffers.
        for case, experimental in (("apply", True), ("apply", False), ("lifetime", True)):
            for component, field in fields:
                for bad in (999, None, True, "0", "missing"):
                    events = records(case, experimental)
                    part = events[4]["buffer_data"][component]
                    if bad == "missing": del part[field]
                    else: part[field] = bad
                    with self.subTest(case=case, component=component, field=field, bad=bad), self.assertRaises(experiment.RunError):
                        self.inspect(events, case, experimental)

    def test_loader_accepts_only_the_requested_file_via_bottle_drives(self):
        root = Path(self.temp.name)
        drives = root / "bottle" / "dosdevices"
        drives.mkdir(parents=True)
        (drives / "z:").symlink_to("/")
        (drives / "d:").symlink_to(root)
        expected = root / "fixture" / "d3d8.dll"
        expected.parent.mkdir()
        expected.write_bytes(b"proxy")
        wrong = root / "other" / "d3d8.dll"
        wrong.parent.mkdir()
        wrong.write_bytes(b"proxy")  # Same contents do not establish file identity.

        def line(name, kind="native"):
            quoted = name.replace("\\", "\\\\")
            return f'0024:trace:loaddll:build_module Loaded L"{quoted}" at 12345678: {kind}\n'

        for name in (r"D:\fixture\d3d8.dll", experiment.windows_path(expected)):
            self.path.write_text(line(name), encoding="utf-8")
            self.assertEqual(experiment.check_stage_loader(self.path, root / "bottle", expected)[0]["kind"], "native")
        builtin = line(r"C:\windows\system32\d3d8.dll", "builtin")
        self.path.write_text(builtin, encoding="utf-8")
        experiment.check_stage_loader(self.path, root / "bottle")
        for contents in (builtin, line(r"D:\other\d3d8.dll"), line(r"E:\fixture\d3d8.dll"),
                         line(r"D:fixture\d3d8.dll"), line(r"D:\fixture\..\fixture\d3d8.dll"),
                         line(r"D:\fixture/../fixture\d3d8.dll"),
                         line(r"D:\fixture\d3d8.dll") * 2):
            self.path.write_text(contents, encoding="utf-8")
            with self.subTest(contents=contents), self.assertRaises(experiment.RunError):
                experiment.check_stage_loader(self.path, root / "bottle", expected)

    def test_case_markers_are_exact_and_ordered(self):
        self.path.write_text("state stateblock_retains_released_buffers=PASS\n"
                             "state case_reacquire=PASS\nstate result=PASS\n", encoding="utf-8")
        experiment.inspect_log(self.path, "reacquire")
        for content in ("state case_reacquire=PASS\nstate result=PASS\n",
                        "state stateblock_retains_released_buffers=PASS\nstate case_reacquire=FAIL\nstate result=PASS\n",
                        "state stateblock_retains_released_buffers=PASS\nstate case_reacquire=PASS\nstate result=PASS\nstate result=PASS\n"):
            self.path.write_text(content, encoding="utf-8")
            with self.assertRaises(experiment.RunError): experiment.inspect_log(self.path, "reacquire")


if __name__ == "__main__": unittest.main()
