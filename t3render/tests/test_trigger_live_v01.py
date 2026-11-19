import math
from pathlib import Path
import json
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import trigger_live_v01 as v01
import compare_live_v01 as compare_v01


def frame():
    return {'session_id': 'live:'+'a'*32, 'map': 'SouthQuarter1_int1', 'map_sha256': 'a'*64,
            'map_epoch': 1, 'device_epoch': 1, 'continuity': 1, 'bsp_blocks': 9,
            'viewport': {'x': 0, 'y': 0, 'width': 1024, 'height': 768},
            'view': [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],
            'projection': [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}


class LiveV01Tests(unittest.TestCase):
    def test_same_pose_rejects_different_session_viewport_or_large_motion(self):
        a, b = frame(), frame()
        b['view'][12] = .1
        self.assertAlmostEqual(v01.same_pose(a,b)['distance_t3'], .1)
        b['view'][12] = 2
        self.assertIsNone(v01.same_pose(a,b))
        b = frame(); b['session_id'] = 'live:'+'b'*32
        self.assertIsNone(v01.same_pose(a,b))
        b = frame(); b['viewport']['width'] = 615
        self.assertIsNone(v01.same_pose(a,b))
        b = frame(); b['projection'][0] = 2
        self.assertIsNone(v01.same_pose(a,b))

    def test_candle_requires_a_known_runtime_enabled_observation(self):
        bits = struct.unpack('<I',struct.pack('<f',.8))[0]
        row = {'light_raw_available': True, 'light_raw': {'candle_link': {
            'status': 'candidate', 'enabled_candidate': True, 'field_8c_bits': bits}}}
        self.assertTrue(v01.candle(row)['enabled'])
        self.assertAlmostEqual(v01.candle(row)['factor'], .8)
        row['light_raw']['candle_link']['enabled_candidate'] = False
        self.assertFalse(v01.candle(row)['enabled'])
        row['light_raw']['candle_link']['enabled_candidate'] = None
        self.assertEqual(v01.candle(row)['source'], 'modulation_candidate')
        self.assertTrue(v01.candle(row)['enabled'])
        row['light_raw']['candle_link']['field_8c_bits'] = 0
        self.assertFalse(v01.candle(row)['enabled'])
        row['light_raw']['candle_link']['enabled_candidate'] = True
        self.assertEqual(v01.candle(row)['source'], 'modulation_candidate')
        self.assertFalse(v01.candle(row)['enabled'])
        row['light_raw']['candle_link']['field_8c_bits'] = struct.unpack('<I',struct.pack('<f',math.nan))[0]
        self.assertIsNone(v01.candle(row))

    def test_coordinator_records_flat_lit_and_extinguished_in_one_session(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            session = 'live:' + 'a'*32
            viewer = root / ('scene-live-' + session.replace(':', '-') + '.jsonl')
            viewer.touch()
            flat = frame()
            flat.update(frame=10, remaster_mode=False, image=True, mesh_instances=1)
            on = frame()
            on.update(frame=11, remaster_mode=True, image=True, diagnostic_variant='remaster',
                      lighting_mode='preview_candle_candidate', bed_override_instances=1,
                      pbr_draws=25, bedroom_material_runs=2, stone_wall_override_draws=1, wood_floor_override_draws=1,
                      light_raw_available=True, light_raw={'candle_link': {
                          'status': 'candidate', 'enabled_candidate': True, 'field_8c_bits': 0x3f800000}})
            off = frame()
            off.update(frame=12, remaster_mode=True, image=False, diagnostic_variant='remaster',
                       lighting_mode='preview_candle_zero_candidate', light_raw_available=True,
                       light_raw={'candle_link': {'status': 'candidate', 'enabled_candidate': False, 'field_8c_bits': 0}})
            off_image = dict(off, frame=13, image=True, diagnostic_variant='remaster_off')
            for name in ('bedroom.jsonl.frame-10.ppm', f'{viewer.stem}-frame-10.ppm',
                         f'{viewer.stem}-remaster-frame-11.ppm', f'{viewer.stem}-remaster-off-frame-13.ppm'):
                (root/name).touch()
            def frames(_):
                return [flat] if not (root/'remaster.enable').exists() else [flat,on,off,off_image]
            summary = {'live_session_id': session, 'frames': [{'frame': 10, 'scene_live_frame': 10}]}
            with mock.patch.object(v01, 'wait_for_start', return_value=(viewer, session)), \
                 mock.patch.object(v01.batch, 'complete_trace', return_value=True), \
                 mock.patch.object(v01.trace_summary, 'summarize', return_value=summary), \
                 mock.patch.object(v01, 'read_frames', side_effect=frames):
                result = v01.coordinate(root, 5)
            self.assertEqual((result['flat_frame'], result['remaster_frame'], result['off_frame']), (10,11,13))
            self.assertTrue((root/'remaster.enable').is_file())
            self.assertTrue((root/'remaster.on.ready').is_file())
            self.assertTrue((root/'remaster.off.capture').is_file())
            self.assertTrue((root/'remaster.complete').is_file())

    def test_watcher_requests_capture_after_movement_and_stable_pose(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            session = 'live:' + 'a'*32
            viewer = root/('scene-live-' + session.replace(':', '-') + '.jsonl')
            process = subprocess.Popen([sys.executable, '-B', str(Path(v01.__file__)), '--watch', temp,
                                        '--timeout', '10'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.addCleanup(lambda: process.poll() is None and process.kill())
            try:
                self.assertEqual(process.stdout.readline().strip(), 'Watcher v0.1 pronto.')
                with viewer.open('w') as stream:
                    stream.write(json.dumps({'event': 'viewer_started', 'session_id': session})+'\n')
                    stream.flush()
                    time.sleep(.5)
                    for i in range(65):
                        row = frame()
                        row.update(event='frame', source='live_process', frame=i, elapsed_ms=i*200,
                                   map_sha256=v01.m1.MAP_SHA, render_pass=0, stage=1,
                                   identity_scope='visible_list', mesh_instances=1, remaster_mode=False,
                                   lighting_mode='unlit', light_raw_available=True,
                                   light_raw={'candle_link': {'status': 'candidate', 'enabled_candidate': True,
                                                              'field_8c_bits': 0x3f800000}})
                        row['viewport']['source'] = 'scoped_draw'
                        row['view'][12] = -6 if i >= 16 else 0
                        stream.write(json.dumps(row)+'\n'); stream.flush()
                        if (root/'capture.request').exists(): break
                        if process.poll() is not None:
                            self.fail(process.stderr.read())
                        time.sleep(.15)
                self.assertTrue((root/'capture.request').is_file())
                request = json.loads((root/'capture.request').read_text())
                self.assertEqual(request['session_id'], session)
                self.assertGreaterEqual(request['duration_ms'], 2500)
            finally:
                if process.poll() is None: process.terminate()
                process.communicate(timeout=5)

    def test_comparison_verifies_linked_images_and_rejects_changed_original(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); session = 'live:'+'a'*32
            prefix = 'scene-live-'+session.replace(':','-')
            (root/'remaster.enable').write_text(json.dumps({'session_id': session, 'flat_frame': 10}))
            (root/'remaster.on.ready').write_text(json.dumps({'session_id': session, 'frame': 11}))
            (root/'remaster.complete').write_text(json.dumps({'session_id': session, 'flat_frame': 10,
                                                             'remaster_frame': 11, 'off_frame': 12}))
            (root/'batch.json').write_text(json.dumps({'status': 'PARTIAL', 'original_saves_unchanged': True}))
            (root/'bedroom.jsonl').touch(); (root/f'{prefix}.jsonl').touch()
            def image(name, pixel):
                path = root/name; path.write_bytes(b'P6\n2 1\n255\n'+pixel*2); return path
            original = image('bedroom.jsonl.frame-10.ppm', b'\x10\x20\x30')
            image(f'{prefix}-frame-10.ppm', b'\x10\x20\x30')
            image(f'{prefix}-remaster-frame-11.ppm', b'\x20\x30\x40')
            image(f'{prefix}-remaster-off-frame-12.ppm', b'\x00\x00\x00')
            summary = {'trace_sha256': 'trace', 'frames': [{'frame': 10, 'frame_image': {
                'status': 'captured', 'stage': 'complete', 'present_source_rect': False, 'present_dest_rect': False,
                'sha256': compare_v01.digest(original)}}]}
            (root/'summary.json').write_text(json.dumps(summary))
            flat = frame(); flat.update(frame=10, remaster_mode=False, image=True)
            on = frame(); on.update(frame=11, diagnostic_variant='remaster', image=True,
                                    lighting_mode='preview_candle_candidate', bed_override_instances=1,
                                    pbr_draws=25, bedroom_material_runs=2, stone_wall_override_draws=1,
                                    wood_floor_override_draws=1, preview_gi_enabled=False, wall_displacement_triangles=0,
                                    preview_candlestick_draws=0, preview_flame_draws=0, shadowed_lights=1, light_raw_available=True,
                                    light_raw={'candle_link': {'status': 'candidate', 'enabled_candidate': True,
                                                               'field_8c_bits': 0x3f800000}})
            off = frame(); off.update(frame=12, diagnostic_variant='remaster_off', image=True,
                                      lighting_mode='preview_candle_zero_candidate', light_raw_available=True,
                                      light_raw={'candle_link': {'status': 'candidate', 'enabled_candidate': False,
                                                                 'field_8c_bits': 0}})
            matched = {'session_id': session, 'trace_sha256': 'trace',
                       'matches': [{'trace_frame': 10, 'live_frame': 10}]}
            with mock.patch.object(compare_v01.compare_live_present, 'compare', return_value=matched), \
                 mock.patch.object(compare_v01.v01, 'read_frames', return_value=[flat, on, off]):
                result, _ = compare_v01.compare(root)
                self.assertEqual(result['status'], 'images_verified')
                self.assertEqual(result['flat_vs_remaster']['different_pixels'], 2)
                self.assertFalse(result['candle_off']['candidate']['enabled'])
                (root/'summary.json').unlink()
                with mock.patch.object(compare_v01.analyze_tds_capture, 'analyze', return_value=summary):
                    self.assertEqual(compare_v01.compare(root)[0]['status'], 'images_verified')
                (root/'summary.json').write_text(json.dumps(summary))
                original.write_bytes(b'P6\n2 1\n255\n'+b'\xff'*6)
                with self.assertRaisesRegex(ValueError, 'validated pre-Present'):
                    compare_v01.compare(root)


if __name__ == '__main__': unittest.main()
