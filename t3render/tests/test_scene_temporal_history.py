"""Portable GPU-history parser rejects incompatible or invalid frame data."""
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import scene_temporal_resolve as temporal
import test_scene_pbr_packet as packet_tests
from test_scene_temporal_resolve import raw_history

HARNESS = r'''
#include "scene_temporal_history.hpp"
#include <iostream>
#include <iterator>
#include <string>
int main() {
    std::string input((std::istreambuf_iterator<char>(std::cin)), {});
    t3render::scene::Reader reader(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
    std::uint32_t a=0,b=0,c=0,flags=0; std::uint8_t digest[32];
    if (!reader.u32(a) || !reader.u32(b) || !reader.u32(c) || !reader.u32(flags) || !reader.bytes(digest,32) ||
        std::size_t(a)+b+c != reader.remaining()) return 3;
    std::string old(a,'\0'), now(b,'\0'), history(c,'\0');
    if (!reader.bytes(old.data(),a) || !reader.bytes(now.data(),b) || !reader.bytes(history.data(),c)) return 3;
    t3render::pbr::Frame previous, current;
    if (!t3render::pbr::ParsePacket(reinterpret_cast<const std::uint8_t*>(old.data()), old.size(), previous) ||
        !t3render::pbr::ParsePacket(reinterpret_cast<const std::uint8_t*>(now.data()), now.size(), current)) return 4;
    t3render::temporal::History result;
    if (!t3render::temporal::ParseHistory(reinterpret_cast<const std::uint8_t*>(history.data()), history.size(),
                                          previous, current, digest, flags, result)) return 2;
    std::cout << result.width << " " << result.height << " " << result.jitter[0] << "\n";
}
'''


class GpuHistoryParserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler: raise unittest.SkipTest("host C++ compiler required")
        cls.temp = tempfile.TemporaryDirectory(prefix="t3render-temporal-parser-")
        cls.binary = Path(cls.temp.name)/"parser"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(HERE.parent/"src"),
                        "-x", "c++", "-o", str(cls.binary), "-"], input=HARNESS, text=True,
                       check=True, capture_output=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def parse(self, previous, current, history, flags=0, digest=None):
        digest = hashlib.sha256(previous).digest() if digest is None else digest
        payload = struct.pack("<IIII", len(previous), len(current), len(history), flags) + digest + previous + current + history
        return subprocess.run([str(self.binary)], input=payload, capture_output=True, timeout=10)

    def test_accepts_camera_motion_and_rejects_changed_lighting(self):
        previous = packet_tests.gpu_fixture.fixture()
        history = raw_history(previous, jitter=(.25, 0))
        current = bytearray(previous)
        struct.pack_into("<f", current, 24+12*4, -1/32)
        struct.pack_into("<f", current, 152, 1/32)
        self.assertEqual(self.parse(previous, bytes(current), history).returncode, 0)
        struct.pack_into("<f", current, 196+28, 1)
        self.assertEqual(self.parse(previous, bytes(current), history).returncode, 2)
        current = bytearray(previous)
        struct.pack_into("<f", current, 20, .5)
        self.assertEqual(self.parse(previous, bytes(current), history).returncode, 2)
        current = bytearray(previous)
        current[240] ^= 1  # First texture byte changes while all geometry remains valid.
        self.assertEqual(self.parse(previous, bytes(current), history).returncode, 2)

    def test_rejects_bad_digest_metadata_and_nonfinite_pixels(self):
        packet = packet_tests.gpu_fixture.fixture()
        history = raw_history(packet)
        self.assertEqual(self.parse(packet, packet, history).returncode, 0)
        self.assertEqual(self.parse(packet, packet, history, digest=bytes(32)).returncode, 2)
        self.assertEqual(self.parse(packet, packet, history, flags=1).returncode, 2)
        corruptions = ((0, b"wrong123"), (temporal.HEADER.size, struct.pack("<H", 0x7c00)),
                       (temporal.HEADER.size, struct.pack("<H", 0xbc00)),
                       (temporal.HEADER.size+64*64*8+6, struct.pack("<e", .5)))
        for offset, value in corruptions:
            with self.subTest(offset=offset, value=value):
                bad = history[:offset]+value+history[offset+len(value):]
                self.assertEqual(self.parse(packet, packet, bad).returncode, 2)
        self.assertEqual(self.parse(packet, packet, history[:-1]).returncode, 2)


if __name__ == "__main__":
    unittest.main()
