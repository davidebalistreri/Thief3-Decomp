from pathlib import Path
import hashlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class BedroomFramingTests(unittest.TestCase):
    def test_projection_and_snapshot_validation(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('host C++ compiler required')
        with tempfile.TemporaryDirectory(prefix='t3render-framing-') as directory:
            binary = Path(directory) / 'fixture'
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            str(ROOT/'tests/scene_live_framing_test.cpp'),
                            str(ROOT/'src/scene_live_framing.cpp'), str(ROOT/'src/scene_live_materials.cpp'),
                            str(ROOT/'src/scene_live_lighting.cpp'), str(ROOT/'ir/scene_live.cpp'),
                            str(ROOT/'ir/scene_live_window.cpp'), str(ROOT/'src/scene_live_catalog.cpp'),
                            '-o', str(binary)], check=True, capture_output=True, timeout=60)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
            self.assertIn('scene_live_framing=ok', result.stdout)
            catalog = ROOT.parent/'build/remaster-v01/bedroom-v01-review.bin'
            if not catalog.is_file():
                catalog = ROOT.parent/'build/remaster-v01/bedroom-v01-originals-v2.bin'
            if not catalog.is_file():
                catalog = ROOT.parent/'build/remaster-v01/bedroom-v01-props-v2.bin'
            if catalog.is_file():
                data = catalog.read_bytes()
                digest = hashlib.sha256(data[:-32]).hexdigest()
                result = subprocess.run([str(binary), digest], input=data, check=True, capture_output=True, timeout=30)
                self.assertIn(b'capture_1084_bed_px=', result.stdout)
                self.assertIn(b'capture_885_candle_6_px=', result.stdout)
