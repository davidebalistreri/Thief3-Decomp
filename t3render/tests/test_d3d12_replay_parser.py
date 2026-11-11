"""Test replay input validation on the host without a GPU or Wine."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
HARNESS = r'''
#define T3RENDER_REPLAY_PARSER_TEST
#include "d3d12_replay_up.cpp"
#include <iostream>
#include <iterator>
int main() {
    const std::string bytes((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    Frame frame{};
    if (!ParseFrameBytes(bytes.data(), bytes.size(), frame)) return 2;
    std::cout << frame.clear_argb << ' ' << frame.vertices[0].x << ' ' << frame.vertices[0].y << ' '
              << frame.vertices[0].z << ' ' << frame.vertices[0].rhw << ' ' << frame.vertices[0].argb << '\n';
    return 0;
}
'''
HEADER = b"T3R_UP_V1\n256 256\n4278190080\n"
VERTICES = b"12.5 -3.25 0 1 4294901760\n116 12 0 1 4294901760\n12 116 0 1 4294901760\n"
REST = b"116 12 0 1 4294901760\n12 116 0 1 4294901760\n"


class D3D12ReplayParserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("a host C++ compiler is required")
        cls.temp = tempfile.TemporaryDirectory(prefix="t3render-replay-parser-")
        cls.binary = Path(cls.temp.name) / "parser"
        result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(HERE),
                                 "-x", "c++", "-o", str(cls.binary), "-"], input=HARNESS,
                                text=True, capture_output=True, timeout=60)
        if result.returncode:
            cls.temp.cleanup()
            raise RuntimeError("parser compilation failed:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def parse(self, data):
        return subprocess.run([str(self.binary)], input=data, capture_output=True, timeout=10)

    def assert_rejected(self, data):
        result = self.parse(data)
        self.assertEqual(result.returncode, 2, result.stderr.decode(errors="replace"))
        self.assertEqual(result.stdout, b"")

    def test_valid_input_preserves_fields(self):
        for data in (HEADER + VERTICES, (HEADER + VERTICES).replace(b"\n", b"\r\n"),
                     (HEADER + VERTICES).rstrip(b"\n"), HEADER + VERTICES + b" \n\t"):
            with self.subTest(data=data):
                result = self.parse(data)
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                self.assertEqual(result.stdout, b"4278190080 12.5 -3.25 0 1 4294901760\n")

    def test_header_viewport_and_unsigned_colors_are_strict(self):
        for data in (b"T3R_UP_V2\n256 256\n0\n" + VERTICES,
                     b"T3R_UP_V1\n257 256\n0\n" + VERTICES,
                     b"T3R_UP_V1\n256 256 extra\n0\n" + VERTICES,
                     b"T3R_UP_V1\n256 256\n-1\n" + VERTICES,
                     b"T3R_UP_V1\n256 256\n4294967296\n" + VERTICES,
                     HEADER + b"0 0 0 1 4294967296\n" + REST):
            with self.subTest(data=data):
                self.assert_rejected(data)

    def test_nonfinite_and_out_of_range_vertices_are_rejected(self):
        for vertex in (b"nan 0 0 1 1\n", b"inf 0 0 1 1\n", b"1e9999 0 0 1 1\n",
                       b"0 0 0 0 1\n", b"0 0 0 0.0000001 1\n", b"0 0 2 1 1\n",
                       b"1048577 0 0 1 1\n", b"0 0 0 1 1 extra\n"):
            with self.subTest(vertex=vertex):
                self.assert_rejected(HEADER + vertex + REST)

    def test_nonuniform_color_alpha_depth_or_rhw_are_rejected(self):
        for vertex in (b"12 12 0 1 4278255360\n", b"12 12 0 1 16711680\n",
                       b"12 12 0.25 1 4294901760\n", b"12 12 0 0.5 4294901760\n"):
            with self.subTest(vertex=vertex):
                self.assert_rejected(HEADER + vertex + REST)

    def test_trailing_missing_oversized_and_nul_data_are_rejected(self):
        for data in (HEADER + VERTICES + b"trailing", HEADER + b"0 0 0 1 1\n", HEADER + b"1" * 4097,
                     HEADER + b"0 0\x00 0 1 1\n" + REST):
            with self.subTest(data=data[:80]):
                self.assert_rejected(data)


if __name__ == "__main__":
    unittest.main()
