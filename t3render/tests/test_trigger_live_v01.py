import copy
import math
from pathlib import Path
import json
import shutil
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


def add_framing(row, other=True):
    row.update(bed_framing=[.1, .1, .9, .9], candle_framing=[.2, .2, .3, .5])
    if other:
        row['other_candle_framing'] = [.6, .2, .7, .5]
    return row


def add_other_candle(row, actor=6):
    row.setdefault('light_raw', {})['other_candle_link'] = {
        'actor_number': actor, 'status': 'candidate', 'enabled_candidate': True, 'field_8c_bits': 0x3f800000}
    return row


def add_bedroom_props(row):
    row.update(bedroom_crate_instances=1, bedroom_door_crate_instances=3, bedroom_cabinet_instances=1,
               bedroom_bread_instances=1, bedroom_target_instances=1)
    return row


class LiveV01Tests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('magick'), 'ImageMagick is required')
    def test_sheet_preserves_uniform_captured_colors(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp);images = []
            pixel = bytes((32, 64, 96))
            for i in range(4):
                path = root/f'frame-{i}.ppm';path.write_bytes(b'P6\n2 1\n255\n'+pixel*2);images.append(path)
            sheet = root/'sheet.png';compare_v01.write_sheet(images, sheet)
            for i in range(4):
                actual = subprocess.run(['magick', str(sheet), '-crop', f'1x1+{i*640+320}+270',
                                         '+repage', '-depth', '8', 'RGB:-'], check=True, capture_output=True).stdout
                self.assertEqual(actual, pixel)

    def test_framing_requires_both_finite_ordered_bounds_and_margin(self):
        row = add_framing(frame())
        self.assertTrue(v01.framed(row))
        self.assertTrue(v01.framed(row, require_other=True))
        row['other_candle_framing'] = [0, .2, .3, .5]
        self.assertFalse(v01.framed(row, require_other=True))
        row['other_candle_framing'] = [.6, .2, .7, .5]
        for value in (None, [0, .1, .9, .9], [.1, .1, 1.01, .9],
                      [.3, .1, .2, .9], [.1, .1, math.nan, .9],
                      [.1, .1, math.inf, .9], [.1, .1, True, .9],
                      [.1, .1, 10**1000, .9], [.1, .1, .9]):
            row['bed_framing'] = value
            self.assertFalse(v01.framed(row))
        del row['bed_framing']
        self.assertFalse(v01.framed(row))

    def test_spoken_guidance_distinguishes_edges_from_missing_data(self):
        row = add_bedroom_props(add_framing(frame())); row['lighting_mode'] = 'unlit'
        sample = {'bed': 1}
        row['light_raw_available'] = True; add_other_candle(row)
        on = {'enabled': True, 'factor': 1, 'actor_number': 14}
        self.assertIsNone(v01.start_guidance(row, sample, True, on))
        self.assertIn('Muoviti', v01.start_guidance(row, sample, False, on))
        self.assertIn('letto', v01.start_guidance(row, {'bed': 0}, True, on))
        self.assertIn('dati', v01.start_guidance(row, sample, True, None))
        self.assertIn('spenta', v01.start_guidance(row, sample, True, {'enabled': False, 'factor': 0}))
        row['light_raw']['other_candle_link']['enabled_candidate'] = False
        self.assertIn('seconda candela', v01.start_guidance(row, sample, True, on))
        row['light_raw']['other_candle_link']['enabled_candidate'] = True
        row['bedroom_bread_instances'] = 0
        self.assertIn('pane', v01.start_guidance(row, sample, True, on))
        row['bedroom_bread_instances'] = 1
        row['bed_framing'] = [.1, .1, 1.01, .9]
        self.assertIn('bordo', v01.start_guidance(row, sample, True, on))
        row['bed_framing'] = None
        self.assertIn('verificare', v01.start_guidance(row, sample, True, on))
        row['bed_framing'] = [.1, .1, math.nan, .9]
        self.assertIn('verificare', v01.start_guidance(row, sample, True, on))

    def test_spoken_guidance_waits_for_stillness_and_limits_repetition(self):
        voice = v01.StartGuidance(0)
        row = frame(); row['elapsed_ms'] = 0
        self.assertIsNone(voice.feed(row, 'bordo', 0))
        row = dict(row, elapsed_ms=21000)
        self.assertEqual(voice.feed(row, 'bordo', 21), 'bordo')
        row = dict(row, elapsed_ms=22000)
        self.assertIsNone(voice.feed(row, 'bordo', 22))
        moved = dict(row, elapsed_ms=41000, view=row['view'].copy()); moved['view'][12] = 2
        self.assertIsNone(voice.feed(moved, 'bordo', 41))
        moved = dict(moved, elapsed_ms=45000)
        self.assertEqual(voice.feed(moved, 'bordo', 45), 'bordo')
        self.assertIsNone(voice.feed(moved, None, 46))

    def test_new_spoken_prompt_interrupts_previous_prompt(self):
        earlier = mock.Mock(); earlier.poll.return_value = None
        later = mock.Mock(); later.poll.return_value = 0
        with mock.patch.object(v01.subprocess, 'Popen', side_effect=[earlier, later]) as launch:
            voice = v01.SpokenPrompt()
            voice('Preparazione')
            voice('Spegni la candela')
        earlier.terminate.assert_called_once()
        self.assertEqual(launch.call_args_list[1].args[0][-1], 'Spegni la candela')
        later.terminate.assert_not_called()

    def test_waiter_speaks_on_persistent_clipping_then_captures_after_correction(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); session = 'live:'+'a'*32
            viewer = root/('scene-live-'+session.replace(':','-')+'.jsonl'); viewer.touch()
            clock = [100.0]
            class Tail:
                def __init__(self, _):
                    self.detector = type('Detector', (), {'session': session})()
                    self.last_seen = None
                    self.index = 0

                def read_new(self, now):
                    i = self.index; self.index += 1; self.last_seen = now
                    row = add_bedroom_props(add_framing(frame()))
                    row.update(event='frame', source='live_process', frame=i, elapsed_ms=i*200,
                               map_sha256=v01.m1.MAP_SHA, render_pass=0, stage=1,
                               identity_scope='visible_list', mesh_instances=3, bed_source_instances=1,
                               remaster_mode=False, lighting_mode='unlit', light_raw_available=True,
                               light_raw={'candle_link': {'status': 'candidate', 'enabled_candidate': True,
                                                          'field_8c_bits': 0x3f800000}})
                    add_other_candle(row, actor=14)
                    row['viewport']['source'] = 'scoped_draw'
                    row['view'][12] = -6 if i >= 1 else 0
                    if i < 115: row['bed_framing'] = [.1, .1, 1.01, .9]
                    return [row]

            def sleep(seconds): clock[0] += seconds
            prompts = []
            with mock.patch.object(v01.m1, 'Tail', Tail), \
                 mock.patch.object(v01.time, 'monotonic', side_effect=lambda: clock[0]), \
                 mock.patch.object(v01.time, 'sleep', side_effect=sleep):
                found = v01.wait_for_start(root, 160, prompts.append)
            self.assertEqual(found, (viewer, session))
            self.assertEqual(len(prompts), 1)
            self.assertIn('bordo', prompts[0])
            self.assertTrue((root/'capture.request').is_file())

    def test_bed_evidence_accepts_other_meshes_without_guessing(self):
        self.assertEqual(v01.m1.bed_count({'mesh_instances': 3, 'bed_source_instances': 1}), 1)
        self.assertEqual(v01.m1.bed_count({'mesh_instances': 3, 'bed_source_instances': 0}), 0)
        self.assertIsNone(v01.m1.bed_count({'mesh_instances': 3}))
        self.assertIsNone(v01.m1.bed_count({'mesh_instances': 0, 'bed_source_instances': 1}))
        self.assertIsNone(v01.m1.bed_count({'mesh_instances': 1, 'bed_source_instances': True}))
        self.assertEqual(v01.m1.bed_count({'mesh_instances': 1}), 1)  # historical log

    def test_new_toggle_frames_require_holder_and_correct_flame_state(self):
        row = {'bed_source_instances': 1, 'preview_candlestick_draws': 2, 'preview_flame_draws': 1}
        self.assertTrue(v01.candle_geometry(row, True))
        self.assertFalse(v01.candle_geometry(row, False))
        row['preview_flame_draws'] = 0
        self.assertTrue(v01.candle_geometry(row, False))
        row['preview_candlestick_draws'] = 0
        self.assertFalse(v01.candle_geometry(row, False))
        self.assertTrue(v01.candle_geometry({}, True))  # historical log
        row.update(preview_candlestick_draws=4, preview_flame_draws=2,
                   preview_selected_candlestick_draws=2, preview_selected_flame_draws=1)
        self.assertTrue(v01.candle_geometry(row, True))
        row.update(preview_flame_draws=1, preview_selected_flame_draws=0)
        self.assertTrue(v01.candle_geometry(row, False))
        row.pop('preview_selected_flame_draws')
        self.assertFalse(v01.candle_geometry(row, False))

    def test_same_pose_rejects_different_session_viewport_or_large_motion(self):
        a, b = frame(), frame()
        b['view'][12] = .1
        self.assertAlmostEqual(v01.same_pose(a,b)['distance_t3'], .1)
        b['view'][12] = 2
        self.assertIsNone(v01.same_pose(a,b))
        b['view'][12] = .5
        self.assertIsNone(v01.same_pose(a,b))
        b = frame();angle = math.radians(.1)
        b['view'][0], b['view'][1] = math.cos(angle), math.sin(angle)
        b['view'][4], b['view'][5] = -math.sin(angle), math.cos(angle)
        self.assertIsNone(v01.same_pose(a,b))  # roll changes even with the same forward axis
        b = frame();b['view'][1] = -0.0
        self.assertEqual(v01.same_pose(a,b)['rotation_degrees'], 0.0)
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
        row['light_raw']['candle_link']['actor_number'] = 14
        self.assertEqual(v01.candle(row)['actor_number'], 14)
        row['light_raw']['candle_link']['actor_number'] = 15
        self.assertIsNone(v01.candle(row))
        row['light_raw']['candle_link']['actor_number'] = 14
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

    def test_two_candles_need_distinct_states_and_draws(self):
        row = {'bed_source_instances': 1, 'light_raw_available': True,
               'light_raw': {'candle_link': {'actor_number': 14, 'status': 'candidate',
                                             'enabled_candidate': True, 'field_8c_bits': 0x3f800000}},
               'preview_selected_candlestick_draws': 2, 'preview_selected_flame_draws': 1,
               'preview_other_candlestick_draws': 2, 'preview_other_flame_draws': 1}
        add_other_candle(row)
        self.assertTrue(v01.both_candles(row, True))
        row['light_raw']['candle_link'].update(enabled_candidate=False, field_8c_bits=0)
        row['preview_selected_flame_draws'] = 0
        self.assertTrue(v01.both_candles(row, False))
        row['preview_other_flame_draws'] = 0
        self.assertFalse(v01.both_candles(row, False))
        row['preview_other_flame_draws'] = 1
        row['light_raw']['other_candle_link']['actor_number'] = 14
        self.assertFalse(v01.both_candles(row, False))

    def test_bedroom_prop_counts_require_all_candidates(self):
        row = add_bedroom_props({})
        self.assertTrue(v01.bedroom_props_ready(row))
        historical = dict(row);del historical['bedroom_door_crate_instances']
        self.assertFalse(v01.bedroom_props_ready(historical))
        self.assertTrue(v01.bedroom_props_ready(historical, require_door_crates=False))
        for value in (0, 1, 2, 4, None, True):
            row['bedroom_door_crate_instances'] = value
            self.assertFalse(v01.bedroom_props_ready(row))
        row['bedroom_door_crate_instances'] = 3
        for value in (0, 2, None, True):
            row['bedroom_bread_instances'] = value
            self.assertFalse(v01.bedroom_props_ready(row))

    def test_coordinator_records_flat_lit_and_extinguished_in_one_session(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            session = 'live:' + 'a'*32
            viewer = root / ('scene-live-' + session.replace(':', '-') + '.jsonl')
            viewer.touch()
            flat = add_bedroom_props(add_framing(frame()))
            flat.update(frame=10, remaster_mode=False, image=True, mesh_instances=3, bed_source_instances=1)
            flat.update(light_raw_available=True, light_raw={'candle_link': {
                'actor_number': 14, 'status': 'candidate', 'enabled_candidate': True, 'field_8c_bits': 0x3f800000}})
            add_other_candle(flat)
            on = add_bedroom_props(add_framing(frame()))
            on.update(frame=11, remaster_mode=True, image=True, diagnostic_variant='remaster',
                      bed_source_instances=1, preview_candlestick_draws=2, preview_flame_draws=1,
                      lighting_mode='preview_candle_candidate', bed_override_instances=1,
                      pbr_draws=25, bedroom_material_runs=2, stone_wall_override_draws=1, wood_floor_override_draws=1,
                      light_raw_available=True, light_raw={'candle_link': {
                          'actor_number': 14, 'status': 'candidate', 'enabled_candidate': True, 'field_8c_bits': 0x3f800000}})
            on.update(preview_selected_candlestick_draws=2, preview_selected_flame_draws=1,
                      preview_other_candlestick_draws=2, preview_other_flame_draws=1)
            add_other_candle(on)
            off = add_bedroom_props(add_framing(frame()))
            off.update(frame=12, remaster_mode=True, image=False, diagnostic_variant='remaster',
                       bed_source_instances=1, preview_candlestick_draws=2, preview_flame_draws=0,
                       lighting_mode='preview_candle_zero_candidate', light_raw_available=True,
                       light_raw={'candle_link': {'actor_number': 14, 'status': 'candidate', 'enabled_candidate': False, 'field_8c_bits': 0}})
            off.update(preview_selected_candlestick_draws=2, preview_selected_flame_draws=0,
                       preview_other_candlestick_draws=2, preview_other_flame_draws=1)
            add_other_candle(off)
            off_image = dict(off, frame=13, image=True, diagnostic_variant='remaster_off')
            bad_flat = dict(flat, frame=9, bed_framing=[.1, .1, 1.01, .9])
            bad_on = dict(on, frame=14, bed_framing=[.1, .1, 1.01, .9])
            bad_off = dict(off_image, frame=15, candle_framing=None)
            for name in ('bedroom.jsonl.frame-9.ppm', f'{viewer.stem}-frame-9.ppm',
                         'bedroom.jsonl.frame-10.ppm', f'{viewer.stem}-frame-10.ppm',
                         f'{viewer.stem}-remaster-frame-14.ppm', f'{viewer.stem}-remaster-frame-11.ppm',
                         f'{viewer.stem}-remaster-off-frame-15.ppm', f'{viewer.stem}-remaster-off-frame-13.ppm'):
                (root/name).touch()
            def frames(_):
                return [flat,bad_flat] if not (root/'remaster.enable').exists() else [flat,bad_on,on,off,bad_off,off_image]
            summary = {'live_session_id': session, 'frames': [{'frame': 9, 'scene_live_frame': 9},
                                                               {'frame': 10, 'scene_live_frame': 10}]}
            with mock.patch.object(v01, 'wait_for_start', return_value=(viewer, session)), \
                 mock.patch.object(v01.batch, 'complete_trace', return_value=True), \
                 mock.patch.object(v01.trace_summary, 'summarize', return_value=summary), \
                 mock.patch.object(v01, 'read_frames', side_effect=frames):
                prompts = []
                result = v01.coordinate(root, 5, prompts.append)
            self.assertEqual((result['flat_frame'], result['remaster_frame'], result['off_frame']), (10,11,13))
            self.assertEqual((result['on']['actor_number'], result['off']['actor_number']), (14,14))
            self.assertEqual(len(prompts), 4)
            self.assertIn('sinistra', prompts[2])
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
                                        '--timeout', '18'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.addCleanup(lambda: process.poll() is None and process.kill())
            try:
                self.assertEqual(process.stdout.readline().strip(), 'Watcher v0.1 pronto.')
                with viewer.open('w') as stream:
                    stream.write(json.dumps({'event': 'viewer_started', 'session_id': session})+'\n')
                    stream.flush()
                    time.sleep(.5)
                    for i in range(100):
                        row = add_bedroom_props(add_framing(frame()))
                        if i < 36:
                            row['bed_framing'] = [.1, .1, 1.01, .9]
                        row.update(event='frame', source='live_process', frame=i, elapsed_ms=i*200,
                                   map_sha256=v01.m1.MAP_SHA, render_pass=0, stage=1,
                                   identity_scope='visible_list', mesh_instances=3, bed_source_instances=1, remaster_mode=False,
                                   lighting_mode='unlit', light_raw_available=True,
                                   light_raw={'candle_link': {'status': 'candidate', 'enabled_candidate': True,
                                                              'field_8c_bits': 0x3f800000}})
                        add_other_candle(row, actor=14)
                        row['viewport']['source'] = 'scoped_draw'
                        row['view'][12] = -6 if i >= 16 else 0
                        stream.write(json.dumps(row)+'\n'); stream.flush()
                        if i == 35:
                            self.assertFalse((root/'capture.request').exists())
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
            (root/'batch.json').write_text(json.dumps({'status': 'PARTIAL', 'original_saves_unchanged': True,
                                                       'candle_actor_number': 14}))
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
                                    light_raw={'candle_link': {'actor_number': 14, 'status': 'candidate', 'enabled_candidate': True,
                                                               'field_8c_bits': 0x3f800000}})
            off = frame(); off.update(frame=12, diagnostic_variant='remaster_off', image=True,
                                      lighting_mode='preview_candle_zero_candidate', light_raw_available=True,
                                      light_raw={'candle_link': {'actor_number': 14, 'status': 'candidate', 'enabled_candidate': False,
                                                                 'field_8c_bits': 0}})
            matched = {'session_id': session, 'trace_sha256': 'trace',
                       'matches': [{'trace_frame': 10, 'live_frame': 10}]}
            with mock.patch.object(compare_v01.compare_live_present, 'compare', return_value=matched), \
                 mock.patch.object(compare_v01.v01, 'read_frames', return_value=[flat, on, off]):
                result, images = compare_v01.compare(root)
                self.assertEqual(result['status'], 'images_verified_two_candles_unverified')
                self.assertEqual(result['framing'], 'unavailable_historical_capture')
                self.assertEqual(len(images), 4)
                self.assertEqual(result['flat_vs_remaster']['different_pixels'], 2)
                self.assertFalse(result['candle_off']['candidate']['enabled'])
                dual_on = dict(on, bed_source_instances=1, preview_candlestick_draws=4, preview_flame_draws=2,
                               preview_selected_candlestick_draws=2, preview_selected_flame_draws=1, shadowed_lights=2)
                dual_off = dict(off, bed_source_instances=1, preview_candlestick_draws=4, preview_flame_draws=1,
                                preview_selected_candlestick_draws=2, preview_selected_flame_draws=0)
                with mock.patch.object(compare_v01.v01, 'read_frames', return_value=[flat, dual_on, dual_off]):
                    self.assertEqual(compare_v01.compare(root)[0]['status'], 'images_verified_two_candles_unverified')
                    dual_on['preview_selected_flame_draws'] = 0
                    with self.assertRaisesRegex(ValueError, 'remaster image'):
                        compare_v01.compare(root)
                    dual_on['preview_selected_flame_draws'] = 1
                    dual_on['shadowed_lights'] = 3
                    with self.assertRaisesRegex(ValueError, 'remaster image'):
                        compare_v01.compare(root)
                flat2, on2, off2 = (add_bedroom_props(add_framing(copy.deepcopy(row))) for row in (flat, on, off))
                flat2['light_raw_available'] = True
                flat2['light_raw'] = copy.deepcopy(on2['light_raw'])
                add_other_candle(flat2); add_other_candle(on2); add_other_candle(off2)
                on2.update(bed_source_instances=1, preview_selected_candlestick_draws=2,
                           preview_selected_flame_draws=1, preview_other_candlestick_draws=2,
                           preview_other_flame_draws=1, shadowed_lights=2)
                off2.update(bed_source_instances=1, preview_selected_candlestick_draws=2,
                            preview_selected_flame_draws=0, preview_other_candlestick_draws=2,
                            preview_other_flame_draws=1)
                with mock.patch.object(compare_v01.v01, 'read_frames', return_value=[flat2, on2, off2]):
                    checked = compare_v01.compare(root)[0]
                    self.assertEqual(checked['status'], 'images_verified')
                    self.assertEqual(checked['two_candles'], 'candidate_states_and_draws_checked')
                    self.assertEqual((checked['other_candle_on']['actor_number'],
                                      checked['other_candle_off']['actor_number']), (6, 6))
                    self.assertTrue(checked['other_candle_off']['enabled'])
                    off2['preview_other_flame_draws'] = 0
                    with self.assertRaisesRegex(ValueError, 'candle off image'):
                        compare_v01.compare(root)
                    off2['preview_other_flame_draws'] = 1
                    off2['bedroom_bread_instances'] = 0
                    with self.assertRaisesRegex(ValueError, 'candle off image'):
                        compare_v01.compare(root)
                    off2['bedroom_bread_instances'] = 1
                    on2['view'][12] = .3; off2['view'][12] = -.3
                    with self.assertRaisesRegex(ValueError, 'candle off image'):
                        compare_v01.compare(root)
                    on2['view'][12] = 0; off2['view'][12] = 0
                    on2['other_candle_framing'] = None
                    with self.assertRaisesRegex(ValueError, 'remaster image'):
                        compare_v01.compare(root)
                off['light_raw']['candle_link']['actor_number'] = 6
                with self.assertRaisesRegex(ValueError, 'candle off image'):
                    compare_v01.compare(root)
                off['light_raw']['candle_link']['actor_number'] = 14
                add_framing(flat, other=False); add_framing(on, other=False); add_framing(off, other=False)
                self.assertEqual(compare_v01.compare(root)[0]['framing'], 'projected_bounds_checked')
                off['bed_framing'] = [.1, .1, 1.01, .9]
                with self.assertRaisesRegex(ValueError, 'candle off image'):
                    compare_v01.compare(root)
                add_framing(off, other=False)
                del flat['candle_framing']
                with self.assertRaisesRegex(ValueError, 'remaster image'):
                    compare_v01.compare(root)
                add_framing(flat, other=False)
                off['view'][12] = 2
                with self.assertRaisesRegex(ValueError, 'candle off image'):
                    compare_v01.compare(root)
                off['view'][12] = 0
                (root/'summary.json').unlink()
                with mock.patch.object(compare_v01.analyze_tds_capture, 'analyze', return_value=summary):
                    self.assertEqual(compare_v01.compare(root)[0]['status'], 'images_verified_two_candles_unverified')
                (root/'summary.json').write_text(json.dumps(summary))
                original.write_bytes(b'P6\n2 1\n255\n'+b'\xff'*6)
                with self.assertRaisesRegex(ValueError, 'validated pre-Present'):
                    compare_v01.compare(root)


if __name__ == '__main__': unittest.main()
