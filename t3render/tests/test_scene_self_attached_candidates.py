"""Self-attached assets are fingerprinted offline candidates, never fake transforms."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'tools'))
import scene_self_attached_candidates as candidates
from test_import_t3_scene import mesh


class FakeBundle:
    resources = []
    def __init__(self, path, data):
        self.path, self.data = path, data
    def __enter__(self): return self
    def __exit__(self, *args): pass
    def of_type(self, kind): return self.resources
    def split(self, resource): return []


class SelfAttachedCandidatesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='t3render-self-asset-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.actor_path = self.root / 'actors.json'
        self.bundle_path = self.root / 'map.ibt'
        self.bundle_path.write_bytes(b'fingerprinted bundle')
        self.export = {'level': 'TestMap', 'actor_count': 1, 'actors': [
            {'name': 'Dummy', 'attached_to': 'Dummy', 'mesh': 'Bed', 'skin': 'Default'}]}
        self.records = [{'event': 'scene_bridge_observation', 'entries': [{'owner_name': 'Dummy'}]}]
        self.source = SimpleNamespace(kind='offline', map='TestMap', fingerprints={
            'map': 'a'*64, 'bundle': hashlib.sha256(self.bundle_path.read_bytes()).hexdigest()})
        self.scene = SimpleNamespace(source=self.source, mesh_instances=[])
        self.write_export()
        FakeBundle.resources = [SimpleNamespace(name='Bed')]

    def write_export(self):
        raw = json.dumps(self.export).encode()
        self.actor_path.write_bytes(raw)
        self.source.fingerprints['actors'] = hashlib.sha256(raw).hexdigest()

    def load(self):
        with mock.patch.object(candidates, 'resolve_map', return_value=self.bundle_path), \
             mock.patch.object(candidates, 'IBT', FakeBundle), \
             mock.patch.object(candidates, 'parse_static_mesh', return_value=mesh()), \
             mock.patch.object(candidates.bridge, 'fullname', side_effect=lambda name, _: name):
            return candidates.load(self.scene, self.records, self.actor_path, self.root)

    def test_asset_and_skin_are_resolved_without_an_offline_instance_or_transform(self):
        result = self.load()
        item = result['Dummy']
        self.assertEqual(item['target_id'], 'actor:TestMap:' + 'a'*64 + ':Dummy')
        self.assertEqual(item['resource'].name, 'Bed')
        self.assertEqual(len(item['resource'].geometry.sections), 2)
        self.assertEqual([item['materials'][x] for x in item['material_ids']], ['A', 'B'])
        self.assertNotIn('transform', item)
        self.assertNotIn('skin', item)

    def test_changed_fingerprints_ambiguous_names_or_invalid_skin_fail_closed(self):
        self.actor_path.write_bytes(self.actor_path.read_bytes() + b' ')
        with self.assertRaisesRegex(ValueError, 'actor fingerprint'):
            self.load()
        self.write_export()
        self.bundle_path.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'bundle fingerprint'):
            self.load()
        self.bundle_path.write_bytes(b'fingerprinted bundle')
        self.export['actors'].append(copy.deepcopy(self.export['actors'][0]))
        self.export['actor_count'] = 2
        self.write_export()
        with self.assertRaisesRegex(ValueError, 'names are ambiguous'):
            self.load()
        self.export['actors'].pop(); self.export['actor_count'] = 1
        self.export['actors'][0]['skin'] = 'missing'
        self.write_export()
        with self.assertRaisesRegex(ValueError, 'skin'):
            self.load()

    def test_nonself_attachment_and_unobserved_actor_are_not_selected(self):
        self.export['actors'][0]['attached_to'] = 'Parent'
        self.write_export()
        self.assertEqual(self.load(), {})
        self.export['actors'][0]['attached_to'] = 'Dummy'
        self.records[0]['entries'][0]['owner_name'] = 'Different'
        self.write_export()
        self.assertEqual(self.load(), {})

    def test_duplicate_or_missing_mesh_is_rejected(self):
        FakeBundle.resources = [SimpleNamespace(name='Bed'), SimpleNamespace(name='BED')]
        with self.assertRaisesRegex(ValueError, 'mesh is ambiguous'):
            self.load()
        FakeBundle.resources = []
        with self.assertRaisesRegex(ValueError, 'mesh is missing'):
            self.load()


if __name__ == '__main__':
    unittest.main()
