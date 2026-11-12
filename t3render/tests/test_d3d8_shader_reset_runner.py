"""Host checks for the isolated D3D8 shader Reset fixture; never runs Wine."""
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import run_d3d8_shader_reset_fixture as runner


def operation(name, phase, hr="0x00000000", **other):
    return {"event": name, "phase": phase, "hr": hr, **other}


def events(scenario="recovery", *, bad_hr="0x8876086c", fallback=False):
    values = [operation("create_shader", 0)]
    for phase in range(3):
        if phase == 1:
            if scenario == "recovery": values += [operation("failed_reset", 1, bad_hr), operation("recovery_reset", 1)]
            else: values += [operation("first_reset", 1)]
        if phase == 2: values += [operation("successful_reset", 2)]
        values += [operation("binding_before", phase, kind="other", other=0x42)]
        for name, words in (("declaration", runner.WORDS), ("function", runner.PROGRAM)):
            size = len(words) * 4
            values += [operation(name + "_size", phase, size=size),
                       operation(name, phase, requested_bytes=size, returned_bytes=size, second_hr="0x00000000",
                                 second_returned_bytes=size, verify_hr="0x00000000", verify_size=size,
                                 tail_intact=True, reads_match=True, words=words[:])]
        failed = fallback and phase == 1
        values.append(operation("set_shader", phase, "0x8876086c" if failed else "0x00000000"))
        if failed: values.append(operation("fallback_fvf", phase))
        values.append(operation("binding_after", phase, original=not failed, matches_selected=True))
    return values + [operation("unbind_shader", 2), operation("delete_shader", 2)]


def log_text(records):
    return ("".join("reset-json " + json.dumps(value) + "\n" for value in records) +
            "".join(f"parity {name} hr=00000000\n" for name in runner.PARITY_CALLS) +
            "shader-handle f0000001\nreset-fixture result=PASS\n")


def trace_records(scenario="recovery", *, enabled=True, fallback=False, bad_hr=-2005530516):
    records = [{"event": "header", "format": "t3render-d3d8-diagnostic", "version": 1,
                "operation_results": 1, "clear_rect_limit": 256, "requested_frames": 3,
                "declaration_capture": {"requested": enabled}}]
    for phase in range(3):
        records.append({"event": "frame_begin", "frame": phase})
        if phase == 1 and scenario == "recovery":
            records += [{"event": "reset", "frame": phase, "reset_hr": bad_hr},
                        {"event": "reset", "frame": phase, "reset_hr": 0}]
        elif phase: records.append({"event": "reset", "frame": phase, "reset_hr": 0})
        clear_id, draw_id = phase * 2 + 1, phase * 2 + 2
        records += [{"event": "clear", "frame": phase, "op_id": clear_id, "rect_count": 0, "rects_status": "none",
                     "rects": [], "flags": 1, "color": 0xff000000, "depth": 1.0, "depth_bits": 0x3f800000, "stencil": 0},
                    {"event": "clear_result", "frame": phase, "op_id": clear_id, "hr": 0}]
        original = not (fallback and phase == 1)
        draw = {"event": "draw", "frame": phase, "op_id": draw_id, "call": "DrawPrimitiveUP", "primitive_type": 4,
                "primitive_count": 1, "vertex_stride": 16, "vertex_shader": 0xf0000001 if original else runner.FVF}
        result = {"event": "draw_result", "frame": phase, "op_id": draw_id, "hr": 0}
        if enabled:
            if not original: declaration = {"status": "unknown", "generation": None, "words": None}
            elif not phase: declaration = {"status": "candidate", "generation": 1, "words": runner.WORDS[:]}
            else: declaration = {"status": "unavailable", "generation": 1, "words": None,
                                 "reason": "reset_attempt", "scan_status": "complete"}
            draw["vertex_declaration"] = declaration
            result["shader_stable"] = True
        records += [draw, result, {"event": "frame_end", "frame": phase, "present_hr": 0, "draws": 1,
                                   "had_reset": bool(phase), "concurrent_operations": False}]
    return records + [{"event": "capture_end", "frames": 3}]


class ShaderResetRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3-shader-reset-test-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "data"

    def log(self, records, scenario="recovery"):
        self.path.write_text(log_text(records), encoding="utf-8")
        return runner.inspect_log(self.path, scenario)

    def trace(self, records, observation, enabled=True, scenario="recovery"):
        self.path.write_text("".join(json.dumps(record) + "\n" for record in records), encoding="utf-8")
        runner.inspect_trace(self.path, observation, enabled, scenario)

    def test_valid_and_failed_recovery_scenarios_are_separate(self):
        for scenario in runner.SCENARIOS:
            observation = self.log(events(scenario), scenario)
            self.assertFalse(observation["partial"])
            for enabled in (True, False): self.trace(trace_records(scenario, enabled=enabled), observation, enabled, scenario)

    def test_failure_not_reproduced_and_shader_fallback_are_partial(self):
        self.assertTrue(self.log(events(bad_hr="0x00000000"))["partial"])
        observation = self.log(events(fallback=True))
        self.assertTrue(observation["partial"])
        self.trace(trace_records(fallback=True), observation)

    def test_failed_recovery_and_device_calls_between_resets_are_rejected(self):
        changed = events(); next(item for item in changed if item["event"] == "recovery_reset")["hr"] = "0x8876086c"
        with self.assertRaises(runner.RunError): self.log(changed)
        changed = events(); index = next(i for i, item in enumerate(changed) if item["event"] == "recovery_reset")
        changed.insert(index, operation("binding_before", 1, kind="handle", other=None))
        with self.assertRaises(runner.RunError): self.log(changed)
        lines = log_text(events()).splitlines()
        parity = next(line for line in lines if line.startswith("parity "))
        lines.remove(parity)
        recovery = next(i for i, line in enumerate(lines) if '"event": "recovery_reset"' in line)
        lines.insert(recovery, parity)
        self.path.write_text("\n".join(lines), encoding="utf-8")
        with self.assertRaises(runner.RunError): runner.inspect_log(self.path, "recovery")

    def test_lost_or_changed_declaration_or_function_is_partial(self):
        for name in ("declaration", "function"):
            for missing in (False, True):
                records = events()
                item = next(x for x in records if x["event"] == name and x["phase"] == 2)
                if missing: item.update(hr="0x8876086c", second_hr=None, second_returned_bytes=None,
                                        verify_hr=None, verify_size=None, reads_match=None, words=None)
                else: item["words"] = [0] * len(item["words"])
                with self.subTest(name=name, missing=missing): self.assertTrue(self.log(records)["partial"])

    def test_query_and_copy_sizes_are_validated_without_exposing_partial_words(self):
        for name in ("declaration", "function"):
            for invalid_size in (None, 0, 3, 4097):
                records = events()
                query = next(item for item in records if item["event"] == name + "_size" and item["phase"] == 1)
                copy_item = next(item for item in records if item["event"] == name and item["phase"] == 1)
                query.update(hr="0x8876086c" if invalid_size is None else "0x00000000", size=invalid_size)
                records.remove(copy_item)
                with self.subTest(name=name, invalid_size=invalid_size): self.assertTrue(self.log(records)["partial"])
            cases = (
                {"hr": "0x8876086c", "second_hr": None, "second_returned_bytes": None,
                 "verify_hr": None, "verify_size": None, "reads_match": None, "words": None},
                {"returned_bytes": 0, "second_hr": None, "second_returned_bytes": None,
                 "verify_hr": None, "verify_size": None, "reads_match": None, "words": None},
                {"second_hr": None, "second_returned_bytes": None, "tail_intact": False,
                 "verify_hr": None, "verify_size": None, "reads_match": None, "words": None},
                {"second_hr": "0x8876086c", "verify_hr": None, "verify_size": None, "reads_match": None, "words": None},
                {"second_returned_bytes": 0, "verify_hr": None, "verify_size": None, "reads_match": None, "words": None},
                {"tail_intact": False, "verify_hr": None, "verify_size": None, "reads_match": None, "words": None},
                {"reads_match": False, "verify_hr": None, "verify_size": None, "words": None},
                {"verify_hr": "0x8876086c", "verify_size": None, "words": None},
                {"verify_size": 0, "words": None},
            )
            for change in cases:
                records = events(); copy_item = next(item for item in records if item["event"] == name and item["phase"] == 1)
                copy_item.update(change)
                with self.subTest(name=name, change=change): self.assertTrue(self.log(records)["partial"])
                copy_item["words"] = [1]
                with self.assertRaises(runner.RunError): self.log(records)

    def test_trace_rejects_unpaired_wrong_reset_or_reason(self):
        observation = self.log(events())
        for mutate in (lambda r: r[8].update(reset_hr=0),
                       lambda r: r[10].update(color=1),
                       lambda r: r[12].update(primitive_count=True),
                       lambda r: r[12]["vertex_declaration"].update(generation=True),
                       lambda r: r[12]["vertex_declaration"].update(reason="handle_collision"),
                       lambda r: r[13].update(shader_stable=False)):
            records = trace_records(); mutate(records)
            with self.subTest(mutate=mutate), self.assertRaises((runner.RunError, runner.trace_summary.TraceError)):
                self.trace(records, observation)

    def test_reused_operation_ids_remain_paired_within_each_frame(self):
        observation = self.log(events())
        records = trace_records()
        for record in records:
            if record["event"] in ("clear", "clear_result"): record["op_id"] = 1
            if record["event"] in ("draw", "draw_result"): record["op_id"] = 2
        self.trace(records, observation)
        next(record for record in records if record["event"] == "draw_result" and record["frame"] == 1)["shader_stable"] = False
        with self.assertRaises(runner.RunError): self.trace(records, observation)

    def test_images_distinguish_green_shader_from_red_fvf(self):
        header = b"P6\n256 256\n255\n"
        for original, color in ((True, b"\0\xff\0"), (False, b"\xff\0\0")):
            pixels = bytearray(256 * 256 * 3)
            for y in range(80, 180):
                for x in range(80, 180):
                    offset = (y * 256 + x) * 3
                    pixels[offset:offset + 3] = color
            self.path.write_bytes(header + pixels)
            runner.inspect_image(self.path, original)
            with self.assertRaises(runner.RunError): runner.inspect_image(self.path, not original)

    def test_original_pixels_must_match_before_and_after_reset(self):
        phases = [{"original": True}] * 3
        self.assertTrue(runner.original_pixels_preserved([b"original"] * 3, phases))
        self.assertFalse(runner.original_pixels_preserved([b"original", b"changed", b"original"], phases))
        self.assertFalse(runner.original_pixels_preserved([b"original"] * 3, phases[:2] + [{"original": False}]))

    def test_i686_fixture_cross_compiles(self):
        compiler = shutil.which("i686-w64-mingw32-g++")
        if not compiler: self.skipTest("i686 MinGW-w64 compiler required")
        result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-municode", "-static",
                                 "-static-libgcc", "-static-libstdc++", str(ROOT / "tests/d3d8_shader_reset_fixture.cpp"),
                                 "-ld3d8", "-ldxguid", "-luser32", "-o", str(Path(self.temp.name) / "fixture.exe")],
                                text=True, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__": unittest.main()
