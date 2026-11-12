"""Fail-closed host tests for the controlled indexed buffer snapshot."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools/buffer_snapshot_to_up_packet.py"
spec = importlib.util.spec_from_file_location("buffer_snapshot_to_up_packet", SCRIPT)
converter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(converter)

VERTICES = [
    (25.6, 217.6, 0.0, 1.0, 0xffff0000),
    (128.0, 25.6, 0.0, 1.0, 0xffff0000),
    (230.4, 217.6, 0.0, 1.0, 0xffff0000),
]
DUMMY = (100.0, 150.0, 0.0, 1.0, 0xffff0000)


def snapshot(vertices=None, indices=(1, 2, 3)):
    vertices = [DUMMY, *VERTICES] if vertices is None else vertices
    vb = b"".join(struct.pack("<4fI", *vertex) for vertex in vertices)
    ib = struct.pack("<3H", *indices)
    return ["T3R_BUFFER_V1", "256 256", "4278190080", "1 1 3 2", "20 20 80", "4 6", vb.hex(), ib.hex()]


class BufferSnapshotToUpPacketTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-buffer-packet-test-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "snapshot.txt"

    def write(self, lines):
        self.path.write_text("\n".join(lines) + "\n", encoding="ascii")

    def convert(self, lines, attested=True):
        self.write(lines)
        return converter.convert(self.path, synthetic_fixture=attested)

    def assert_invalid(self, lines, message):
        with self.assertRaisesRegex(converter.SnapshotError, message):
            self.convert(lines)

    def test_indices_resolve_base_and_captured_offset_in_draw_order(self):
        for indices in ((1, 2, 3), (3, 1, 2), (2, 3, 1)):
            with self.subTest(indices=indices):
                lines = self.convert(snapshot(indices=indices)).splitlines()
                self.assertEqual(lines[:3], ["T3R_UP_V1", "256 256", "4278190080"])
                self.assertEqual(len(lines), 6)
                for line, index in zip(lines[3:], indices):
                    fields = line.split()
                    actual = struct.pack("<4fI", *map(float, fields[:4]), int(fields[4]))
                    self.assertEqual(actual, struct.pack("<4fI", *VERTICES[index - 1]))

    def test_attestation_is_required_and_output_is_exclusive(self):
        self.write(snapshot())
        with self.assertRaisesRegex(converter.SnapshotError, "--synthetic-fixture"):
            converter.convert(self.path)
        output = Path(self.temp.name) / "packet.txt"
        cmd = [sys.executable, "-B", str(SCRIPT), str(self.path), "-o", str(output)]
        result = subprocess.run(cmd, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())
        result = subprocess.run([*cmd, "--synthetic-fixture"], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(output.read_text().startswith("T3R_UP_V1\n"))
        self.assertIn("render state and snapshot provenance are not verified", result.stderr)
        output.write_text("keep\n")
        result = subprocess.run([*cmd, "--synthetic-fixture"], text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_text(), "keep\n")

    def test_invalid_snapshot_does_not_create_output(self):
        lines = snapshot(indices=(0, 2, 3))
        self.write(lines)
        output = Path(self.temp.name) / "packet.txt"
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), str(self.path), "--synthetic-fixture",
                                 "-o", str(output)], text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())
        self.assertIn("vertex range", result.stderr)

    def test_rejects_wrong_header_viewport_and_draw_layout(self):
        cases = [
            (0, "T3R_UP_V1", "header"), (1, "257 256", "viewport"),
            (1, "256 256 1", "viewport"), (2, "0", "clear"), (2, "4294967296", "uint32"),
            (3, "0 1 3 2", "draw parameters"), (3, "1 0 3 2", "draw parameters"),
            (3, "1 1 2 2", "draw parameters"), (3, "1 1 3 1", "draw parameters"),
            (4, "24 20 80", "vertex buffer"), (4, "20 0 80", "vertex buffer"),
            (4, "20 20 79", "vertex buffer"), (5, "0 6", "index buffer"),
            (5, "4 5", "index buffer"), (5, "-1 6", "integer"),
        ]
        for position, value, error in cases:
            with self.subTest(position=position, value=value):
                lines = snapshot(); lines[position] = value
                self.assert_invalid(lines, error)

    def test_rejects_truncated_corrupted_or_extra_bytes(self):
        for pos in (6, 7):
            for change in (lambda text: text[:-2], lambda text: text[:-1], lambda text: text + "00",
                           lambda text: "zz" + text[2:], lambda text: " " + text[1:]):
                with self.subTest(position=pos, change=change):
                    lines = snapshot(); lines[pos] = change(lines[pos])
                    self.assert_invalid(lines, "hex-encoded bytes")
        self.assert_invalid(snapshot()[:-1], "eight lines")
        self.assert_invalid(snapshot() + ["extra"], "eight lines")
        self.path.write_bytes(b"T3R_BUFFER_V1\n" + b"0" * 4096)
        with self.assertRaisesRegex(converter.SnapshotError, "size limit"):
            converter.convert(self.path, synthetic_fixture=True)
        self.path.write_bytes(b"\xff")
        with self.assertRaises(UnicodeDecodeError):
            converter.convert(self.path, synthetic_fixture=True)

    def test_rejects_out_of_range_or_degenerate_indices(self):
        for indices in ((0, 2, 3), (1, 2, 4), (1, 2, 256), (65535, 2, 3)):
            with self.subTest(indices=indices):
                self.assert_invalid(snapshot(indices=indices), "vertex range")
        self.assert_invalid(snapshot(indices=(1, 1, 1)), "degenerate")

    def test_rejects_unsupported_or_corrupted_vertex_values(self):
        cases = [
            ((float("nan"), 217.6, 0, 1, 0xffff0000), "non-finite"),
            ((25.6, float("inf"), 0, 1, 0xffff0000), "non-finite"),
            ((-1, 217.6, 0, 1, 0xffff0000), "fixture space"),
            ((25.6, 257, 0, 1, 0xffff0000), "fixture space"),
            ((25.6, 217.6, 0.1, 1, 0xffff0000), "fixture space"),
            ((25.6, 217.6, 0, 0.5, 0xffff0000), "fixture space"),
            ((25.6, 217.6, 0, 1, 0x00ff0000), "opaque"),
            ((25.6, 217.6, 0, 1, 0xff00ff00), "uniform"),
        ]
        for replacement, error in cases:
            with self.subTest(replacement=replacement):
                vertices = [DUMMY, *VERTICES]; vertices[1] = replacement
                self.assert_invalid(snapshot(vertices=vertices), error)
        black = [(x, y, z, rhw, 0xff000000) for x, y, z, rhw, _ in VERTICES]
        self.assert_invalid(snapshot(vertices=[DUMMY, *black]), "differ from clear")
        collinear = [(0, 0, 0, 1, 0xffff0000), (1, 1, 0, 1, 0xffff0000), (2, 2, 0, 1, 0xffff0000)]
        self.assert_invalid(snapshot(vertices=[DUMMY, *collinear]), "degenerate")

    def test_crlf_and_stdout(self):
        self.path.write_bytes(("\r\n".join(snapshot()) + "\r\n").encode("ascii"))
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), str(self.path), "--synthetic-fixture"],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(result.stdout.splitlines()), 6)
        self.assertTrue(result.stdout.startswith("T3R_UP_V1\n"))


if __name__ == "__main__":
    unittest.main()
