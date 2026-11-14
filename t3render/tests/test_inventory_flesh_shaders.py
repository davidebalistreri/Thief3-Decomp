"""Inventory shader framing without shipping or exposing game programs."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import inventory_flesh_shaders as inventory


def parts(version=0xfffe0100):
    words = [version, 1, 0xc00f0000, 0x90e40000, 0xffff]
    code = struct.pack("<5I", *words)
    return [struct.pack("<I", 0), b"\0" * 4, struct.pack("<I", 4), b"foo\0",
            struct.pack("<I", len(code)), code]


def bundle(parts):
    header = bytearray(0x34)
    entry = bytearray(0x134)
    data_start = len(header) + len(entry) + 4 * len(parts)
    data = b"".join(parts)
    struct.pack_into("<8I", header, 0, 0xc0000001, 1, data_start, len(data), len(data),
                     max(map(len, parts)), 1, len(parts))
    struct.pack_into("<5I", entry, 0, data_start, len(data), 0, 0, len(parts))
    entry[0x28] = 29
    entry[0x29:0x2d] = b"foo\0"
    return bytes(header + entry) + struct.pack(f"<{len(parts)}I", *map(len, parts)) + data


class ShaderInventoryTests(unittest.TestCase):
    def test_counts_framed_10_without_emitting_words(self):
        with tempfile.TemporaryDirectory(prefix="t3render-shader-inventory-") as directory:
            path = Path(directory) / "synthetic.ibt"
            path.write_bytes(bundle(parts()))
            result = inventory.inventory(path)
            self.assertEqual((result["resources"], result["malformed"]), (1, 0))
            self.assertEqual(result["vertex_versions"], {"0xfffe0100": 1})
            self.assertEqual(result["vertex_framed"], {"0xfffe0100": 1})
            self.assertNotIn("c00f0000", json.dumps(result))
            output = Path(directory) / "inventory.json"
            cmd = [sys.executable, "-B", str(ROOT / "tools/inventory_flesh_shaders.py"), str(path), "-o", str(output)]
            self.assertEqual(subprocess.run(cmd, capture_output=True).returncode, 0)
            before = output.read_bytes()
            self.assertNotEqual(subprocess.run(cmd, capture_output=True).returncode, 0)
            self.assertEqual(output.read_bytes(), before)

    def test_malformed_parts_and_unknown_versions_are_not_framed(self):
        data = parts()
        data[4] = struct.pack("<I", 1)
        self.assertEqual(inventory.classify(data)[0], "malformed")
        self.assertEqual(inventory.classify(parts(0xfffe0200)), ("vertex", 0xfffe0200, False))
        self.assertEqual(inventory.classify(parts(0xfffe0101)), ("vertex", 0xfffe0101, True))


if __name__ == "__main__":
    unittest.main()
