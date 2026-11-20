import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class SceneSelectionDiagnosticsTest(unittest.TestCase):
    def test_selection_records_exact_transform_and_evidence(self):
        source = Path(__file__).resolve().parent / 'scene_live_selection_diagnostics_fixture.cpp'
        with tempfile.TemporaryDirectory(prefix='t3scene-selection-diagnostics-') as temp:
            binary = Path(temp) / 'test'
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            str(source), '-o', str(binary)], check=True)
            selection = json.loads(subprocess.check_output([str(binary)], text=True))['scene_selection']
        self.assertFalse(selection['truncated'])
        mesh = selection['meshes'][0]
        self.assertEqual(mesh['id'], 'actor:"\\\n')
        self.assertEqual(mesh['transform_bits'][0], 0x80000000)
        self.assertEqual(mesh['transform_bits'][5], 0x3f800000)
        self.assertEqual((mesh['binding'], mesh['scope'], mesh['sections']), ('candidate', 'visible_list', [0, 2]))
        self.assertEqual(selection['bsp'][0], {'id': 'bsp:wall', 'binding': 'verified',
                                               'scope': 'full_object_scan', 'sections': [1]})


if __name__ == '__main__': unittest.main()
