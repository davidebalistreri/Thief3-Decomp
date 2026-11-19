#!/usr/bin/env python3
"""Coordinate a same-session flat/remaster capture and the runtime candle switch."""
import argparse
import json
import math
from pathlib import Path
import struct
import sys
import time

import compare_live_light_diagnostics as diagnostics
import compare_live_present
import run_tds_bedroom_batch as batch
import trace_summary
import trigger_live_m1 as m1

MAX_POSE_DISTANCE = .5  # T3 units; the exact measured difference is retained
MAX_POSE_ANGLE = .2    # degrees


def same_pose(a, b):
    if (a.get('session_id') != b.get('session_id') or
            any(a.get(k) != b.get(k) for k in ('map', 'map_sha256', 'map_epoch', 'device_epoch', 'continuity', 'viewport', 'bsp_blocks')) or
            compare_live_present._float_bits(a.get('projection')) != compare_live_present._float_bits(b.get('projection')) or
            compare_live_present._float_bits(a.get('projection')) is None):
        return None
    try:
        pa, pb = diagnostics.camera_position(a.get('view')), diagnostics.camera_position(b.get('view'))
        fa, fb = m1.forward(a.get('view')), m1.forward(b.get('view'))
        if pa is None or pb is None or fa is None or fb is None:
            return None
        distance, rotation = math.dist(pa, pb), m1.angle(fa, fb)
        if distance > MAX_POSE_DISTANCE or rotation > MAX_POSE_ANGLE:
            return None
        return {'distance_t3': distance, 'rotation_degrees': rotation}
    except (ValueError, TypeError, OverflowError):
        return None


def candle(row):
    if row.get('light_raw_available') is not True:
        return None
    raw = row.get('light_raw')
    link = raw.get('candle_link') if isinstance(raw, dict) else None
    if not isinstance(link, dict) or link.get('status') not in ('candidate', 'no_link'):
        return None
    enabled = link.get('enabled_candidate')
    bits = link.get('field_8c_bits')
    factor = None
    if type(bits) is int and 0 <= bits <= 0xffffffff:
        factor = struct.unpack('<f', struct.pack('<I', bits))[0]
        if not math.isfinite(factor) or not 0 <= factor <= 1:
            return None
    if enabled is True and factor == 0:
        enabled, source = False, 'modulation_candidate'
    elif type(enabled) is bool:
        source = 'enabled_candidate'
    elif enabled is None and link['status'] == 'candidate' and factor is not None:
        enabled, source = factor > 0, 'modulation_candidate'
    else:
        return None
    return {'enabled': enabled, 'factor': factor, 'link_status': link['status'], 'source': source}


def read_frames(path):
    if path.stat().st_size > 64 * 1024 * 1024:
        raise ValueError('viewer log exceeds diagnostic limit')
    rows = []
    with path.open('rb') as source:
        for raw in source:
            if len(raw) > m1.MAX_LINE:
                raise ValueError('oversized viewer log line')
            if not raw.endswith(b'\n'):
                break  # a frame can still be in flight
            row = diagnostics.strict_json(raw)
            if isinstance(row, dict) and row.get('event') == 'frame':
                rows.append(row)
    return rows


def marker(path, payload):
    with path.open('x', encoding='utf-8') as stream:
        json.dump(payload, stream, indent=2, allow_nan=False)
        stream.write('\n')


def wait_for_start(directory, deadline):
    tails = {}
    baseline = stable = None
    stable_host = None
    moved = False
    while time.monotonic() < deadline:
        if (directory/'batch.json').exists():
            raise ValueError('runner finished before a stationary bedroom pose')
        now = time.monotonic()
        paths = sorted(directory.glob('scene-live-live-*.jsonl')) if directory.is_dir() else []
        if len(paths) > 16:
            raise ValueError('too many viewer sessions')
        updates = {}
        for path in paths:
            if path not in tails:
                try: tails[path] = m1.Tail(path)
                except (OSError, ValueError): pass
                continue
            updates[path] = tails[path].read_new(now)
        active = [p for p in paths if p in tails and tails[p].last_seen is not None and
                  now-tails[p].last_seen <= m1.FRESH_SECONDS and 0 <= time.time()-p.stat().st_mtime <= m1.FRESH_SECONDS]
        if len(active) != 1:
            baseline = stable = stable_host = None
            moved = False
        else:
            path = active[0]
            session = tails[path].detector.session
            for row in updates.get(path, []):
                sample = m1.sample(row, session)
                if sample is None or row.get('remaster_mode') is not False:
                    baseline = stable = stable_host = None
                    moved = False
                    continue
                if baseline is None:
                    baseline = sample
                if (sample['epoch'] != baseline['epoch'] or sample['frame'] <= baseline['frame']):
                    baseline = sample
                    stable = stable_host = None
                    moved = False
                if (math.dist(baseline['position'], sample['position']) >= 4 or
                        m1.angle(baseline['forward'], sample['forward']) >= 5):
                    moved = True
                c = candle(row)
                eligible = (moved and row.get('mesh_instances') == 1 and row.get('lighting_mode') == 'unlit' and
                            c is not None and c['enabled'] and c['factor'] is not None and c['factor'] > 0)
                if not eligible:
                    stable = stable_host = None
                    continue
                if stable is None or same_pose(stable, row) is None:
                    stable, stable_host = row, time.monotonic()
                if (row['elapsed_ms']-stable['elapsed_ms'] >= 2500 and time.monotonic()-stable_host >= 2.5):
                    result = {'session_id': session, 'first_frame': stable['frame'], 'last_frame': row['frame'],
                              'duration_ms': row['elapsed_ms']-stable['elapsed_ms'], 'camera': same_pose(stable,row),
                              'note': 'Requests capture only; the final comparison checks Present and camera independently.'}
                    marker(directory/'capture.request', result)
                    return path, session
        time.sleep(.2)
    raise ValueError('no stationary bedroom pose before timeout')


def coordinate(directory, timeout):
    deadline = time.monotonic()+timeout
    viewer, session = wait_for_start(directory, deadline)
    trace = directory/'bedroom.jsonl'
    while time.monotonic()<deadline and not batch.complete_trace(trace, 3):
        if (directory/'batch.json').exists(): raise ValueError('runner finished before flat capture')
        time.sleep(.2)
    if not batch.complete_trace(trace, 3): raise ValueError('flat capture timed out')
    summary = trace_summary.summarize(trace)
    if summary.get('live_session_id') != session: raise ValueError('trace and viewer sessions differ')
    linked = {r['scene_live_frame']: r['frame'] for r in summary['frames'] if 'scene_live_frame' in r}
    flat = None
    while time.monotonic()<deadline and flat is None:
        for row in read_frames(viewer):
            frame=row.get('frame')
            if (frame in linked and row.get('remaster_mode') is False and row.get('image') is True and
                    row.get('mesh_instances') == 1 and
                    (directory/f"bedroom.jsonl.frame-{linked[frame]}.ppm").is_file() and
                    (directory/f"{viewer.stem}-frame-{frame}.ppm").is_file()):
                flat=row
        if flat is None: time.sleep(.2)
    if flat is None: raise ValueError('no linked original and flat images')
    marker(directory/'remaster.enable', {'flat_frame': flat['frame'], 'session_id': session})
    on = None
    on_deadline = min(deadline, time.monotonic()+20)
    while time.monotonic()<on_deadline and on is None:
        if (directory/'batch.json').exists(): raise ValueError('runner finished before remaster image')
        for row in read_frames(viewer):
            c=candle(row)
            if (row.get('diagnostic_variant') == 'remaster' and row.get('image') is True and
                    row.get('lighting_mode') == 'preview_candle_candidate' and c and c['enabled'] and
                    c['factor'] is not None and c['factor'] > 0 and row.get('bed_override_instances') == 1 and
                    row.get('pbr_draws', 0) > 0 and row.get('bedroom_material_runs', 0) > 0 and
                    row.get('stone_wall_override_draws', 0) > 0 and row.get('wood_floor_override_draws', 0) > 0 and
                    same_pose(flat,row) is not None and
                    (directory/f"{viewer.stem}-remaster-frame-{row['frame']}.ppm").is_file()):
                on=row
                break
        if on is None: time.sleep(.2)
    if on is None: raise ValueError('no lit remaster image at the captured pose')
    marker(directory/'remaster.on.ready', {'frame': on['frame'], 'session_id': session})
    print('Remaster acceso acquisito; attendo lo spegnimento della candela.', flush=True)
    off = None
    while time.monotonic()<deadline and off is None:
        if (directory/'batch.json').exists(): raise ValueError('runner finished before candle switch')
        for row in read_frames(viewer):
            c=candle(row)
            if (row.get('frame',0)>on['frame'] and row.get('remaster_mode') is True and
                    row.get('lighting_mode') == 'preview_candle_zero_candidate' and c and not c['enabled']):
                off=row
                break
        if off is None: time.sleep(.2)
    if off is None: raise ValueError('candle off state was not observed')
    marker(directory/'remaster.off.capture', {'first_off_frame': off['frame'], 'session_id': session})
    off_image = None
    while time.monotonic()<deadline and off_image is None:
        if (directory/'batch.json').exists(): raise ValueError('runner finished before off image')
        for row in read_frames(viewer):
            c=candle(row)
            if (row.get('frame',0)>=off['frame'] and row.get('diagnostic_variant') == 'remaster_off' and
                    row.get('image') is True and row.get('lighting_mode') == 'preview_candle_zero_candidate' and
                    c and not c['enabled'] and
                    (directory/f"{viewer.stem}-remaster-off-frame-{row['frame']}.ppm").is_file()):
                off_image=row
                break
        if off_image is None: time.sleep(.2)
    if off_image is None: raise ValueError('no remaster image of the off state')
    result={'session_id': session, 'trace_frame': linked[flat['frame']], 'flat_frame': flat['frame'],
            'remaster_frame': on['frame'], 'off_frame': off_image['frame'],
            'flat_to_remaster_pose': same_pose(flat,on), 'on': candle(on), 'off': candle(off_image),
            'limitations': ['The enabled property and light identity remain runtime candidates until validated against the user action.',
                            'The final report must independently check the Present link and captured image pixels.']}
    marker(directory/'remaster.complete', result)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--watch', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=360)
    args=parser.parse_args()
    try:
        if not math.isfinite(args.timeout) or args.timeout <= 0: raise ValueError('invalid timeout')
        print('Watcher v0.1 pronto.', flush=True)
        result=coordinate(args.watch,args.timeout)
        print(json.dumps({'status':'completed','comparison':result},indent=2,allow_nan=False))
        return 0
    except (OSError, ValueError, TypeError, KeyError, OverflowError, trace_summary.TraceError) as exc:
        if args.watch.is_dir() and not (args.watch/'batch.json').exists():
            try: marker(args.watch/'remaster.failed', {'error': str(exc)})
            except OSError: pass
        print(f'error: {exc}',file=sys.stderr)
        return 1


if __name__=='__main__': sys.exit(main())
