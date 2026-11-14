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
    if (frame.diagnostic) {
        std::cout << "diagnostic " << frame.width << ' ' << frame.height << ' ' << frame.min_depth << ' '
                  << frame.max_depth << ' ' << frame.scene.size() << ' ' << frame.scene[1].vertices[2].x << '\n';
        return 0;
    }
    if (!frame.scene.empty()) {
        std::cout << "scene " << frame.scene.size() << ' ' << frame.clear_argb << ' '
                  << frame.scene[0].matrices[0][12] << ' ' << frame.scene[1].matrices[0][12] << ' '
                  << frame.scene[0].matrices[1][12] << ' ' << frame.scene[1].matrices[1][12] << ' '
                  << frame.scene[0].matrices[2][0] << ' ' << frame.scene[1].matrices[2][0] << ' '
                  << frame.scene[0].vertices[0].argb << ' ' << frame.scene[1].vertices[0].argb << '\n';
        return 0;
    }
    if (frame.camera) {
        std::cout << "camera " << frame.matrices[0][12] << ' ' << frame.matrices[1][0] << ' '
                  << frame.matrices[2][11] << ' ' << frame.vertices[0].x << ' '
                  << frame.vertices[0].y << ' ' << frame.vertices[0].z << ' '
                  << frame.vertices[0].argb << '\n';
        return 0;
    }
    std::cout << frame.clear_argb << ' ' << frame.vertices[0].x << ' ' << frame.vertices[0].y << ' '
              << frame.vertices[0].z << ' ' << frame.vertices[0].rhw << ' ' << frame.vertices[0].argb << '\n';
    return 0;
}
'''
HEADER = b"T3R_UP_V1\n256 256\n4278190080\n"
VERTICES = b"12.5 -3.25 0 1 4294901760\n116 12 0 1 4294901760\n12 116 0 1 4294901760\n"
REST = b"116 12 0 1 4294901760\n12 116 0 1 4294901760\n"
CAMERA_HEADER = b"T3R_XYZ_V1\n256 256\n4278190080\n"
WORLD = b"0 1 0 0 -1 0 0 0 0 0 1 0 0.1 -0.05 2 1\n"
VIEW = b"0.9 0 0 0 0 1.1 0 0 0 0 1 0 -0.05 0.05 0.5 1\n"
PROJECTION = b"1.8 0 0 0 0 1.8 0 0 0 0 1.06666667 1 0 0 -0.53333336 0\n"
CAMERA_VERTICES = b"-0.7 0.8 0 4294901760\n0.8 0 0.2 4294901760\n-0.7 -0.8 0.1 4294901760\n"
DIAGNOSTIC = (b"T3R_DIAGNOSTIC_XYZ_V1\n1024 768\n0 0.5\n4278190080\n" + WORLD + VIEW + PROJECTION +
              CAMERA_VERTICES + CAMERA_VERTICES)


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

    def test_camera_packet_preserves_captured_matrices_and_vertices(self):
        data = CAMERA_HEADER + WORLD + VIEW + PROJECTION + CAMERA_VERTICES
        result = self.parse(data)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.assertEqual(result.stdout, b"camera 0.1 0.9 1 -0.7 0.8 0 4294901760\n")

    def test_camera_packet_rejects_unsupported_or_invalid_input(self):
        for data in (
            CAMERA_HEADER + b"0 1\n" + VIEW + PROJECTION + CAMERA_VERTICES,
            CAMERA_HEADER + WORLD + VIEW + b"nan " + PROJECTION + CAMERA_VERTICES,
            CAMERA_HEADER + WORLD + VIEW + PROJECTION + CAMERA_VERTICES + b"extra",
            CAMERA_HEADER + WORLD + VIEW + PROJECTION + b"-0.7 0.8 0 4294901760\n" * 3,
            CAMERA_HEADER + WORLD + VIEW + PROJECTION + CAMERA_VERTICES.replace(b"-0.7 0.8", b"100 100"),
            CAMERA_HEADER + WORLD + VIEW + PROJECTION + CAMERA_VERTICES.replace(b"4294901760", b"4278255360", 1),
            CAMERA_HEADER + WORLD + VIEW + PROJECTION + CAMERA_VERTICES.replace(b"4294901760", b"16711680"),
            CAMERA_HEADER.replace(b"4278190080", b"0") + WORLD + VIEW + PROJECTION + CAMERA_VERTICES,
        ):
            with self.subTest(data=data): self.assert_rejected(data)

    def test_scene_preserves_draw_order_transforms_and_colors(self):
        first = WORLD + VIEW + PROJECTION + CAMERA_VERTICES
        second = WORLD.replace(b"0.1 -0.05", b"0.4 -0.05") + VIEW.replace(b"-0.05 0.05", b"-0.13 0.05") + \
            PROJECTION.replace(b"1.8", b"1.71") + \
            CAMERA_VERTICES.replace(b"4294901760", b"4278255360")
        data = b"T3R_MULTI_XYZ_V1\n256 256\n4278190080\n2\n" + first + second
        result = self.parse(data)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.assertEqual(result.stdout, b"scene 2 4278190080 0.1 0.4 -0.05 -0.13 1.8 1.71 4294901760 4278255360\n")
        for bad in (data.replace(b"\n2\n", b"\n1\n", 1),
                    data.replace(b"\n2\n", b"\n33\n", 1),
                    data.replace(b"\n2\n", b"\n2 extra\n", 1),
                    data + b"extra", data[:-30],
                    data.replace(b"4294901760", b"0", 1)):
            with self.subTest(data=bad[:80]): self.assert_rejected(bad)

    def test_diagnostic_packet_keeps_original_viewport_and_depth(self):
        result = self.parse(DIAGNOSTIC)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.assertEqual(result.stdout, b"diagnostic 1024 768 0 0.5 2 -0.7\n")
        for bad in (DIAGNOSTIC.replace(b"1024 768", b"2049 768"),
                    DIAGNOSTIC.replace(b"1024 768", b"0 768"),
                    DIAGNOSTIC.replace(b"0 0.5\n", b"0.8 0.5\n", 1),
                    DIAGNOSTIC.replace(b"0 0.5\n", b"nan 1\n", 1),
                    DIAGNOSTIC.replace(b"4294901760", b"4278255360", 1),
                    DIAGNOSTIC.replace(b"4294901760", b"16711680", 1),
                    DIAGNOSTIC.replace(b"-0.7 0.8", b"10001 0.8", 1),
                    DIAGNOSTIC[:-30], DIAGNOSTIC + b"extra"):
            with self.subTest(data=bad[:80]): self.assert_rejected(bad)


if __name__ == "__main__":
    unittest.main()
