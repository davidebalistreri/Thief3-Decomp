import copy
import contextlib
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests'))
import t3bench
import run_tds_bedroom_batch as batch
import test_scene_live_catalog as catalog_fixture
import test_scene_pbr_bed as bed_fixture
import scene_ir


def bits(value): return struct.unpack('<I', struct.pack('<f', value))[0]


def catalog_bytes():
    body = b'T3LCAT10' + bytes(16)
    return body + hashlib.sha256(body).digest()


def fixture():
    identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    identity_bits = [bits(x) for x in identity]
    def link(actor, enabled):
        return {'actor_number': actor, 'status': 'candidate', 'enabled_candidate': enabled,
                'enabled_inherited': False, 'field_8c_bits': bits(1 if enabled else 0), 'transform_bits': identity_bits}
    def light(actor):
        item = {'id': f'light-actor:{t3bench.MAP}:{t3bench.MAP_SHA}:D_384:{actor}', 'generation': 1,
                'field_24_bits': [0]*3, 'field_34_bits': [0]*3, 'field_68_bits': [bits(.5)]*3}
        item.update({f'field_{field}': 0 for field in ('58', '5c', '60', '64')})
        item.update({f'field_{field}_bits': 0 for field in ('74', '78', '7c', '88', '90')})
        item['field_8c_bits'] = bits(1)
        return item
    def row(on):
        items = [light(14), light(6)] if on else [light(6)]
        return {'session_id': 'live:'+'a'*32, 'map': t3bench.MAP, 'map_sha256': t3bench.MAP_SHA,
                'map_epoch': 1, 'device_epoch': 1, 'continuity': 1, 'bsp_blocks': 1,
                'viewport': {'x': 0, 'y': 0, 'width': 2, 'height': 2, 'min_depth': 0, 'max_depth': 1},
                'view': identity, 'projection': identity, 'light_raw_available': True,
                'light_raw': {'listed': len(items), 'skipped': 0, 'truncated': False, 'items': items,
                              'candle_link': link(14, on), 'other_candle_link': link(6, True)}}
    selected = {'result': 'published', 'map': t3bench.MAP, 'map_sha256': t3bench.MAP_SHA,
                'bsp': [{'id': 'bsp:wall', 'sections': [0]}],
                'meshes': [{'id': f'actor:{t3bench.MAP}:{t3bench.MAP_SHA}:StaticMeshActor__1520',
                            'transform': identity, 'sections': [0]}]}
    actors = {'StaticMeshActor__1520': (identity_bits, {0})}
    return selected, actors, row(True), row(False)


class BenchmarkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler: raise unittest.SkipTest('C++ compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='t3bench-test-')
        cls.binary = Path(cls.temp.name)/'scene-test'
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                        *(str(ROOT/path) for path in ('tests/scene_benchmark_test.cpp', 'src/scene_benchmark.cpp',
                          'src/scene_live_lighting.cpp', 'ir/scene_live.cpp')), '-o', str(cls.binary)],
                       check=True, capture_output=True, timeout=90)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def load(self, packet):
        return subprocess.run([str(self.binary)], input=packet, capture_output=True, check=True, timeout=30).stdout.strip()

    def test_round_trip_and_corrupt_scene_rejected(self):
        selected, actors, on, off = fixture()
        packet = t3bench.encode_scene(selected, actors, on, off)
        self.assertEqual(self.load(packet), b'accepted')
        self.assertEqual(self.load(packet.replace(b'B bsp:wall 1 0', b'B bsp:wall 1 2')), b'rejected')
        self.assertEqual(self.load(packet + b'extra'), b'rejected')
        self.assertEqual(self.load(packet.replace(b'STATE OFF', b'STATE BAD')), b'rejected')
        self.assertEqual(self.load(packet.replace(b'StaticMeshActor__1520', b'StaticMeshActor__999')), b'rejected')
        self.assertEqual(t3bench.packet_camera(packet), t3bench.camera_metadata(on))
        with self.assertRaisesRegex(ValueError, 'camera values'):
            t3bench.packet_camera(packet.replace(b'CAMERA 1065353216', b'CAMERA 2143289344', 1))

    def test_rejects_unverified_states_and_incomplete_light_list(self):
        selected, actors, on, off = fixture()
        changed = copy.deepcopy(off); changed['light_raw']['other_candle_link']['enabled_candidate'] = False
        with self.assertRaises(ValueError): t3bench.encode_scene(selected, actors, on, changed)
        changed = copy.deepcopy(on); changed['light_raw']['skipped'] = 1
        with self.assertRaises(ValueError): t3bench.encode_scene(selected, actors, changed, off)
        changed = copy.deepcopy(off); changed['view'][12] = 5
        with self.assertRaises(ValueError): t3bench.encode_scene(selected, actors, on, changed)

    def test_host_validates_a_real_synthetic_catalog_and_selection(self):
        data = json.loads(json.dumps(catalog_fixture.static_fixture()).replace('a'*64, t3bench.MAP_SHA))
        data['conventions']['meters_per_unit'] = .01905
        data['materials'][0]['name'] = 'STMwall512D_D'
        data['bsp_blocks'][0]['geometry']['positions'] = [[0, 0, 0], [0, 1, 0], [0, 0, 1]]
        scene = scene_ir.SceneSnapshot.from_dict(data)
        catalog, _ = catalog_fixture.catalog.export(json.dumps(data).encode(), bed_fixture.glb(scene),
                                                    catalog_fixture.TinyTextures(), True)
        selected, actors, on, off = fixture()
        selected['bsp'][0]['id'] = data['bsp_blocks'][0]['id']
        packet = t3bench.encode_scene(selected, actors, on, off)
        t3bench.validate_scene(catalog, packet)
        actors['StaticMeshActor__5'] = (actors['StaticMeshActor__1520'][0], {0, 8})
        actors['Uncatalogued__9'] = (actors['StaticMeshActor__1520'][0], {0})
        scene, selection = t3bench.prepare_scene(catalog, selected, actors, on, off)
        self.assertIn(b'StaticMeshActor__5', scene)
        self.assertNotIn(b'Uncatalogued__9', scene)
        self.assertEqual(selection, {'observed_candidates': 3, 'included_candidates': 2,
                                     'excluded_candidates': 1, 'excluded_sections': 2})
        different = copy.deepcopy(selected)
        different['meshes'][0]['transform'][12] = 1
        with self.assertRaisesRegex(ValueError, 'bed transform'):
            t3bench.prepare_scene(catalog, different, actors, on, off)
        different = copy.deepcopy(selected)
        different['meshes'][0]['sections'] = [1]
        with self.assertRaisesRegex(ValueError, 'bed sections'):
            t3bench.match_replayed_bed(different, actors)
        actors['StaticMeshActor__1520'][1].add(1)
        self.assertEqual(t3bench.match_replayed_bed(selected, actors)['StaticMeshActor__1520'][1], {0})
        prefix = f'actor:{t3bench.MAP}:{t3bench.MAP_SHA}:'
        def mesh(name, resource):
            return {'id': prefix+name, 'resource_id': resource, 'generation': 1, 'binding': 'candidate',
                    'scope': 'visible_list', 'transform_bits': actors['StaticMeshActor__1520'][0], 'sections': [0]}
        block = {'id': selected['bsp'][0]['id'], 'binding': 'candidate', 'scope': 'visible_list', 'sections': [0]}
        flat = copy.deepcopy(on)
        flat.update(mesh_instances=1, bsp_blocks=1,
                    scene_selection={'truncated': False,
                                     'meshes': [mesh('StaticMeshActor__1520', data['static_mesh_resources'][0]['id'])],
                                     'bsp': [block]})
        captured_on = copy.deepcopy(flat)
        captured_on['mesh_instances'] = 2
        captured_on['scene_selection']['meshes'].append(mesh('StaticMeshActor__5', data['static_mesh_resources'][1]['id']))
        observed, details = t3bench.prepare_observed_scene(catalog, selected, flat, captured_on, off)
        self.assertIn(b'StaticMeshActor__5', observed)
        self.assertEqual(details['source'], 'viewer_snapshot_on')
        self.assertEqual((details['flat_candidates'], details['included_candidates']), (1, 2))
        invalid = copy.deepcopy(flat)
        invalid['scene_selection']['bsp'][0]['sections'] = [1]
        with self.assertRaisesRegex(ValueError, 'BSP selection'):
            t3bench.prepare_observed_scene(catalog, selected, invalid, captured_on, off)
        invalid = copy.deepcopy(flat)
        invalid['scene_selection']['truncated'] = True
        with self.assertRaisesRegex(ValueError, 'missing or truncated'):
            t3bench.recorded_selection(invalid)
        invalid = copy.deepcopy(captured_on)
        invalid['scene_selection']['meshes'][1]['resource_id'] = data['static_mesh_resources'][0]['id']
        with self.assertRaisesRegex(ValueError, 'differs from the catalog'):
            t3bench.prepare_observed_scene(catalog, selected, flat, invalid, off)
        selected['camera'] = {'view': flat['view'], 'projection': flat['projection'], 'viewport': flat['viewport']}
        with tempfile.TemporaryDirectory(prefix='t3bench-observed-capture-') as temp:
            root = Path(temp); capture = root/'capture'; capture.mkdir()
            (capture/'bedroom.jsonl').write_text('{}\n')
            session = on['session_id']
            viewer = capture/('scene-live-'+session.replace(':', '-')+'.jsonl')
            records = [dict(flat, event='frame', frame=3), dict(captured_on, event='frame', frame=4),
                       dict(off, event='frame', frame=5)]
            viewer.write_text(''.join(json.dumps(row)+'\n' for row in records))
            images = []
            for i in range(4):
                image = capture/f'{i}.ppm';image.write_bytes(b'P6\n2 2\n255\n'+bytes([i, i, i])*4);images.append(image)
            hashes = [t3bench.sha(path.read_bytes()) for path in images]
            report = {'status': 'images_verified', 'two_candles': 'candidate_states_and_draws_checked',
                      'framing': 'projected_bounds_checked', 'session_id': session, 'trace_frame': 2, 'flat_frame': 3,
                      'remaster_frame': 4, 'candle_off': {'frame': 5, 'image_sha256': hashes[3], 'pose_delta': {}},
                      'original_sha256': hashes[0], 'flat_sha256': hashes[1], 'remaster_sha256': hashes[2],
                      'pose_delta': {}, 'limitations': []}
            catalog_path = root/'catalog.bin';catalog_path.write_bytes(catalog)
            (capture/'batch.json').write_text(json.dumps({'live_catalog_sha256_before': t3bench.sha(catalog),
                                                          'live_catalog_sha256_after': t3bench.sha(catalog)}))
            comparison_image = capture/'remaster-v0.1-comparison.png';comparison_image.write_bytes(b'comparison')
            review = {'schema': 1, 'session_id': session, 'trace_frame': 2, 'on_frame': 4, 'off_frame': 5,
                      'original_sha256': hashes[0], 'on_sha256': hashes[2], 'off_sha256': hashes[3],
                      'comparison_sha256': t3bench.sha(comparison_image.read_bytes()),
                      'user_confirmed_selected_candle_off': True,
                      'image_review': {'bed_visible': True, 'both_candlesticks_visible': True,
                                       'selected_flame_on_then_off': True, 'other_flame_remains_on': True}}
            review_path = capture/'t3bench-review.json';review_path.write_text(json.dumps(review))
            (capture/'texture-state.json').write_text(json.dumps({'original_bundle_sha256': 'a'*64,
                'patched_bundle_sha256': 'b'*64, 'original_sha256_after_capture': 'a'*64,
                'catalog_sha256': t3bench.sha(catalog), 'patched_restored': True, 'runner_exit': 0,
                'watcher_exit': 0, 'sidecar_retained': None, 'error': None}))
            replayed = {'trace_sha256': 'a'*64, 'catalog_sha256': t3bench.sha(catalog),
                        'frames': [selected], 'limitations': []}
            with mock.patch.object(t3bench.comparison, 'compare', return_value=(report, images)), \
                 mock.patch.object(t3bench.replay, 'read_capture', return_value=({2: []}, 'a'*64, 1)), \
                 mock.patch.object(t3bench.replay, 'replay', return_value=replayed), \
                 mock.patch.object(t3bench.reconstruction, 'actor_candidates', side_effect=AssertionError('unexpected legacy selection')):
                package = root/'package'
                manifest = t3bench.create(capture, catalog_path, package)
                self.assertEqual(manifest['capture']['actor_candidates']['source'], 'viewer_snapshot_on')
                self.assertEqual(manifest['capture']['actor_candidates']['included_candidates'], 2)
                self.assertTrue(manifest['capture']['review']['user_confirmed_selected_candle_off'])
                self.assertEqual(manifest['capture']['texture_state']['original_bundle_sha256'], 'a'*64)
                self.assertIn(b'StaticMeshActor__5', (package/'scene.txt').read_bytes())
                self.assertEqual(t3bench.verify(package), manifest)
                tampered = copy.deepcopy(manifest);tampered['capture']['review']['off_sha256'] = 'f'*64
                (package/'manifest.json').write_text(json.dumps(tampered))
                with self.assertRaisesRegex(ValueError, 'review does not match'):
                    t3bench.verify(package)
                tampered = copy.deepcopy(manifest);tampered['capture']['texture_state']['patched_restored'] = False
                (package/'manifest.json').write_text(json.dumps(tampered))
                with self.assertRaisesRegex(ValueError, 'texture state'):
                    t3bench.verify(package)
                (package/'manifest.json').write_text(json.dumps(manifest))
                review_path.write_text(json.dumps({**review, 'off_sha256': 'f'*64}))
                with self.assertRaisesRegex(ValueError, 'review does not match'):
                    t3bench.create(capture, catalog_path, root/'bad-review')
                review_path.write_text(json.dumps(review))
                del records[1]['scene_selection']
                viewer.write_text(''.join(json.dumps(row)+'\n' for row in records))
                with self.assertRaisesRegex(ValueError, 'selection is incomplete'):
                    t3bench.create(capture, catalog_path, root/'incomplete')
        with self.assertRaisesRegex(ValueError, 'catalog and benchmark selection'):
            t3bench.validate_scene(catalog, packet.replace(b'StaticMeshActor__1520', b'StaticMeshActor__999'))

    def test_verify_detects_tampering_and_metrics_compare_pixels(self):
        with tempfile.TemporaryDirectory(prefix='t3bench-package-test-') as temp:
            package = Path(temp); ppm = b'P6\n2 1\n255\n' + bytes([0, 0, 0, 10, 20, 30])
            selected, actors, on, off = fixture()
            on['viewport']['height'] = off['viewport']['height'] = 1
            payload = {name: (ppm if name.endswith('.ppm') else catalog_bytes() if name == 'catalog.bin' else b'test')
                       for name in t3bench.PAYLOAD_FILES}
            payload['scene.txt'] = t3bench.encode_scene(selected, actors, on, off)
            payload['integrity.txt'] = ''.join(f'{name} {t3bench.sha(payload[name])}\n' for name in t3bench.PAYLOAD_FILES).encode()
            for name, data in payload.items(): (package/name).write_bytes(data)
            manifest = {'format': 't3bench-bedroom-v1', 'map': t3bench.MAP, 'map_sha256': t3bench.MAP_SHA,
                        'camera': t3bench.camera_metadata(on),
                        'files': {name: t3bench.sha(data) for name, data in payload.items()}}
            (package/'manifest.json').write_text(json.dumps(manifest))
            self.assertEqual(t3bench.verify(package), manifest)
            manifest['camera']['view_bits'][0] ^= 1
            (package/'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, 'camera differ'): t3bench.verify(package)
            manifest['camera']['view_bits'][0] ^= 1
            (package/'manifest.json').write_text(json.dumps(manifest))
            other = package/'other.ppm'; other.write_bytes(b'P6\n2 1\n255\n'+bytes([0, 0, 0, 20, 20, 30]))
            result = t3bench.metrics(package/'d3d8.ppm', other)
            self.assertEqual(result['different_pixels'], 1)
            self.assertAlmostEqual(result['mean_absolute_rgb'], 10/6)
            t3bench.sheet([package/'d3d8.ppm', other], package/'sheet.ppm')
            self.assertEqual(t3bench.comparison.read_ppm(package/'sheet.ppm')[:2], (4, 1))
            t3bench.bitmap_sheet([package/'d3d8.ppm', other], package/'sheet.bmp')
            bitmap = (package/'sheet.bmp').read_bytes()
            self.assertEqual(bitmap[:2], b'BM')
            self.assertEqual(struct.unpack_from('<ii', bitmap, 18), (4, 1))
            self.assertEqual(bitmap[54:60], bytes([0, 0, 0, 30, 20, 10]))
            (package/'catalog.bin').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'changed'): t3bench.verify(package)

    def test_package_and_run_with_simulated_renderer(self):
        selected, actors, on, off = fixture()
        selected['camera'] = {'view': on['view'], 'projection': on['projection'], 'viewport': on['viewport']}
        with tempfile.TemporaryDirectory(prefix='t3bench-workflow-test-') as temp:
            root = Path(temp); capture = root/'capture'; capture.mkdir()
            (capture/'bedroom.jsonl').write_text('{}\n')
            session = on['session_id']
            viewer = capture/('scene-live-'+session.replace(':', '-')+'.jsonl')
            records = [dict(on, event='frame', frame=3), dict(on, event='frame', frame=4),
                       dict(off, event='frame', frame=5)]
            viewer.write_text(''.join(json.dumps(row)+'\n' for row in records))
            paths = []
            for i in range(4):
                image = capture/f'{i}.ppm';image.write_bytes(b'P6\n2 2\n255\n'+bytes([i, i, i])*4);paths.append(image)
            hashes = [t3bench.sha(path.read_bytes()) for path in paths]
            report = {'status': 'images_verified', 'two_candles': 'candidate_states_and_draws_checked',
                      'framing': 'projected_bounds_checked', 'session_id': session, 'trace_frame': 2, 'flat_frame': 3,
                      'remaster_frame': 4, 'candle_off': {'frame': 5, 'image_sha256': hashes[3], 'pose_delta': {}},
                      'original_sha256': hashes[0], 'flat_sha256': hashes[1], 'remaster_sha256': hashes[2],
                      'pose_delta': {}, 'limitations': []}
            catalog = root/'catalog.bin';catalog.write_bytes(catalog_bytes())
            (capture/'batch.json').write_text(json.dumps({'live_catalog_sha256_before': t3bench.sha(catalog_bytes()),
                                                          'live_catalog_sha256_after': t3bench.sha(catalog_bytes())}))
            replayed = {'trace_sha256': 'a'*64, 'catalog_sha256': t3bench.sha(catalog_bytes()),
                        'frames': [selected], 'limitations': []}
            selection = {'observed_candidates': 1, 'included_candidates': 1,
                         'excluded_candidates': 0, 'excluded_sections': 0}
            with mock.patch.object(t3bench.comparison, 'compare', return_value=(report, paths)), \
                 mock.patch.object(t3bench.replay, 'read_capture', return_value=({2: []}, 'a'*64, 1)), \
                 mock.patch.object(t3bench.replay, 'replay', return_value=replayed), \
                 mock.patch.object(t3bench, 'prepare_scene', return_value=(t3bench.encode_scene(selected, actors, on, off), selection)), \
                 mock.patch.object(t3bench.reconstruction, 'actor_candidates', return_value=actors):
                package = root/'package';manifest = t3bench.create(capture, catalog, package)
                second = root/'package-2';manifest2 = t3bench.create(capture, catalog, second)
                (capture/'batch.json').write_text(json.dumps({'live_catalog_sha256_before': t3bench.sha(catalog_bytes()),
                                                              'live_catalog_sha256_after': 'b'*64}))
                with self.assertRaisesRegex(ValueError, 'attest the catalog'):
                    t3bench.create(capture, catalog, root/'not-verified')
            self.assertEqual(t3bench.verify(package), manifest)
            self.assertEqual(manifest, manifest2)
            self.assertEqual(manifest['capture']['camera_frame'], 4)
            (root/'renderer').touch()
            def render(command, **kwargs):
                for i, path in enumerate(command[-3:]):
                    Path(path).write_bytes(b'P6\n2 2\n255\n'+bytes([i, i, i])*4)
                return subprocess.CompletedProcess(command, 0, '{"cpu_readback_ms":[1,2,3]}\n', '')
            with mock.patch.object(t3bench.subprocess, 'run', side_effect=render):
                result = t3bench.run(package, root/'renderer', root/'result')
            self.assertEqual(result['differences']['on_vs_off']['different_pixels'], 4)
            self.assertTrue((root/'result'/'comparison.ppm').is_file())
            self.assertTrue((root/'result'/'metrics.json').is_file())
            with self.assertRaisesRegex(ValueError, 'output already exists'):
                t3bench.run(package, root/'renderer', root/'result')

    def test_live_environment_is_opt_in(self):
        root = Path('/tmp/t3bench-example')
        _, plain = batch.env_for(root, root, root, 1, scene_bridge=True, submission_scope=True,
                                 live_catalog=root/'catalog', live_static_meshes=True)
        self.assertNotIn('T3RENDER_T3BENCH_PACKAGE', plain)
        _, enabled = batch.env_for(root, root, root, 1, scene_bridge=True, submission_scope=True,
                                   live_catalog=root/'catalog', live_static_meshes=True,
                                   t3bench_package=root/'package')
        self.assertEqual(enabled['T3RENDER_T3BENCH_PACKAGE'], batch.windows_path(root/'package'))
        self.assertEqual(enabled['T3RENDER_T3BENCH_OUTPUT'], batch.windows_path(root))

    def test_cli_selects_highball_only_with_a_complete_pair(self):
        args = ['run', '/tmp/package', '--renderer', '/tmp/renderer.exe', '--output', '/tmp/result',
                '--highball-engine', '/tmp/engine', '--highball-bottle', '/tmp/bottle']
        with mock.patch.object(t3bench, 'run_highball', return_value={'status': 'ok'}) as run, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(t3bench.main(args), 0)
        self.assertEqual(run.call_args.args, tuple(Path(p) for p in
                         ('/tmp/package', '/tmp/renderer.exe', '/tmp/result', '/tmp/engine', '/tmp/bottle')))
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(t3bench.main(args[:-2]), 1)
            self.assertEqual(t3bench.main(args + ['--wine-paths']), 1)


if __name__ == '__main__': unittest.main()
