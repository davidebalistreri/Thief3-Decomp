#!/usr/bin/env python3
"""Coordinate a same-session flat/remaster capture and the runtime candle switch."""
import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import time

import compare_live_light_diagnostics as diagnostics
import compare_live_present
import run_tds_bedroom_batch as batch
import trace_summary
import trigger_live_m1 as m1

MAX_POSE_DISTANCE = .5  # T3 units; the exact measured difference is retained
FRAMING_MARGIN = .02   # normalized viewport, on each edge


def framed(row, require_other=False):
    # These are projected geometry bounds, not a visibility/occlusion test.
    for name in ('bed_framing', 'candle_framing') + (('other_candle_framing',) if require_other else ()):
        bounds = row.get(name)
        if (not isinstance(bounds, list) or len(bounds) != 4 or
                any(type(v) not in (int, float) or not FRAMING_MARGIN <= v <= 1-FRAMING_MARGIN for v in bounds)):
            return False
        left, top, right, bottom = bounds
        if not (FRAMING_MARGIN <= left < right <= 1-FRAMING_MARGIN and
                FRAMING_MARGIN <= top < bottom <= 1-FRAMING_MARGIN):
            return False
    return True


def framing_data_available(row, require_other=False):
    for name in ('bed_framing', 'candle_framing') + (('other_candle_framing',) if require_other else ()):
        bounds = row.get(name)
        if (not isinstance(bounds, list) or len(bounds) != 4 or
                any(type(v) not in (int, float) or not -1e8 <= v <= 1e8 for v in bounds)):
            return False
        if bounds[0] >= bounds[2] or bounds[1] >= bounds[3]:
            return False
    return True


def same_pose(a, b):
    if (a.get('session_id') != b.get('session_id') or
            any(a.get(k) != b.get(k) for k in ('map', 'map_sha256', 'map_epoch', 'device_epoch', 'continuity', 'viewport', 'bsp_blocks')) or
            compare_live_present._float_bits(a.get('projection')) != compare_live_present._float_bits(b.get('projection')) or
            compare_live_present._float_bits(a.get('projection')) is None):
        return None
    try:
        pa, pb = diagnostics.camera_position(a.get('view')), diagnostics.camera_position(b.get('view'))
        fa, fb = m1.forward(a.get('view')), m1.forward(b.get('view'))
        va, vb = compare_live_present._float_bits(a.get('view')), compare_live_present._float_bits(b.get('view'))
        if pa is None or pb is None or fa is None or fb is None or va is None or vb is None:
            return None
        # Compare all three view axes at recorded float32 precision; matching
        # only the forward vector misses camera roll. Treat signed zero alike.
        for index in (0, 1, 2, 4, 5, 6, 8, 9, 10):
            x, y = va[index], vb[index]
            if x != y and not ((x & 0x7fffffff) == 0 and (y & 0x7fffffff) == 0):
                return None
        distance = math.dist(pa, pb)
        if distance >= MAX_POSE_DISTANCE:
            return None
        return {'distance_t3': distance, 'rotation_degrees': 0.0}
    except (ValueError, TypeError, OverflowError):
        return None


def candle(row, other=False):
    if row.get('light_raw_available') is not True:
        return None
    raw = row.get('light_raw')
    link = raw.get('other_candle_link' if other else 'candle_link') if isinstance(raw, dict) else None
    if not isinstance(link, dict) or link.get('status') not in ('candidate', 'no_link'):
        return None
    actor_number = link.get('actor_number') if other else link.get('actor_number', 6)  # historical logs
    if type(actor_number) is not int or actor_number not in (6, 14):
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
    return {'enabled': enabled, 'factor': factor, 'link_status': link['status'], 'source': source,
            'actor_number': actor_number}


def candle_geometry(row, on, other=False):
    # Historical logs predate holder rendering in toggle mode. New logs must
    # show both holder sections and exactly the expected flame state.
    if 'bed_source_instances' not in row and not other:
        return True
    selected = (('preview_other_candlestick_draws', 'preview_other_flame_draws') if other else
                ('preview_selected_candlestick_draws', 'preview_selected_flame_draws'))
    if any(k in row for k in selected):
        holder, flame = (row.get(k) for k in selected)
    elif other:
        return False
    else:
        holder, flame = row.get('preview_candlestick_draws'), row.get('preview_flame_draws')
    return type(holder) is int and type(flame) is int and holder == 2 and flame == (1 if on else 0)


def both_candles(row, selected_on, require_geometry=True):
    selected, other = candle(row), candle(row, other=True)
    if (not selected or not other or selected['enabled'] is not selected_on or not other['enabled'] or
            selected['actor_number'] == other['actor_number'] or other['factor'] is None or other['factor'] <= 0 or
            (selected_on and (selected['factor'] is None or selected['factor'] <= 0))):
        return False
    return not require_geometry or (candle_geometry(row, selected_on) and candle_geometry(row, True, other=True))


def bedroom_props_ready(row, require_door_crates=True):
    # These are selection counts, not proof of visible pixels or GPU bindings.
    names = ('bedroom_crate_instances', 'bedroom_cabinet_instances',
             'bedroom_bread_instances', 'bedroom_target_instances')
    return (all(type(row.get(name)) is int and row[name] == 1 for name in names) and
            (not require_door_crates or (type(row.get('bedroom_door_crate_instances')) is int and
                                         row['bedroom_door_crate_instances'] == 3)))


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


def start_guidance(row, sample, moved, observation):
    if row.get('lighting_mode') != 'unlit':
        return None
    if not moved:
        return 'Muoviti per scegliere la posa, poi resta fermo.'
    if sample['bed'] != 1:
        return 'Il letto non è ancora rilevato. Inquadralo, poi resta fermo.'
    if observation is None or observation['factor'] is None:
        return 'Attendo i dati della candela. La cattura non è ancora partita.'
    if not observation['enabled'] or observation['factor'] <= 0:
        return 'La candela selezionata risulta spenta. La cattura non è ancora partita.'
    other = candle(row, other=True)
    if not other or other['actor_number'] == observation.get('actor_number') or other['factor'] is None:
        return 'Attendo i dati della seconda candela. La cattura non è ancora partita.'
    if not other['enabled'] or other['factor'] <= 0:
        return 'La seconda candela risulta spenta. La cattura non è ancora partita.'
    if not bedroom_props_ready(row):
        return 'Attendo il riconoscimento di pane e mobili della bedroom. La cattura non è ancora partita.'
    if not framed(row, require_other=True):
        return ('Letto o candele sono troppo vicini al bordo. Regola la visuale, poi resta fermo.'
                if framing_data_available(row, require_other=True) else
                'Non riesco ancora a verificare l’inquadratura. La cattura non è partita.')
    return None


class StartGuidance:
    # Speak only after a sustained stationary observation and rate-limit cues.
    def __init__(self, now):
        self.last_spoken = now
        self.reset()

    def reset(self):
        self.row = self.reason = self.host_start = None

    def feed(self, row, reason, now):
        if reason is None:
            self.reset()
            return None
        if self.row is None or self.reason != reason or same_pose(self.row, row) is None:
            self.row, self.reason, self.host_start = row, reason, now
        if (row['elapsed_ms']-self.row['elapsed_ms'] >= 4000 and now-self.host_start >= 4 and
                now-self.last_spoken >= 20):
            self.last_spoken = now
            return reason
        return None


class SpokenPrompt:
    def __init__(self):
        self.process = None

    def __call__(self, message):
        if self.process is not None and self.process.poll() is None:
            try:
                self.process.terminate()
            except ProcessLookupError:
                pass
        self.process = subprocess.Popen(['/usr/bin/say', '-v', 'Alice', message], stdin=subprocess.DEVNULL,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)


def wait_for_start(directory, deadline, announce=None):
    tails = {}
    baseline = stable = None
    stable_host = None
    moved = False
    guidance = StartGuidance(time.monotonic()) if announce else None
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
            if guidance: guidance.reset()
        else:
            path = active[0]
            session = tails[path].detector.session
            for row in updates.get(path, []):
                sample = m1.sample(row, session)
                if sample is None or row.get('remaster_mode') is not False:
                    baseline = stable = stable_host = None
                    moved = False
                    if guidance: guidance.reset()
                    continue
                if baseline is None:
                    baseline = sample
                if (sample['epoch'] != baseline['epoch'] or sample['frame'] <= baseline['frame']):
                    baseline = sample
                    stable = stable_host = None
                    moved = False
                    if guidance: guidance.reset()
                if (math.dist(baseline['position'], sample['position']) >= 4 or
                        m1.angle(baseline['forward'], sample['forward']) >= 5):
                    moved = True
                c = candle(row)
                eligible = (moved and sample['bed'] == 1 and row.get('lighting_mode') == 'unlit' and
                            both_candles(row, True, require_geometry=False) and bedroom_props_ready(row) and
                            framed(row, require_other=True))
                if not eligible:
                    stable = stable_host = None
                    if guidance:
                        message = guidance.feed(row, start_guidance(row, sample, moved, c), now)
                        if message: announce(message)
                    continue
                if guidance: guidance.reset()
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


def coordinate(directory, timeout, announce=None):
    deadline = time.monotonic()+timeout
    if announce:
        announce('Muoviti per scegliere la posa. Inquadra letto e entrambe le candele, poi resta fermo.')
    viewer, session = wait_for_start(directory, deadline, announce)
    if announce:
        announce('Inquadratura acquisita. Rimani fermo mentre preparo il confronto.')
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
                    m1.bed_count(row) == 1 and bedroom_props_ready(row) and framed(row, require_other=True) and
                    both_candles(row, True, require_geometry=False) and
                    (directory/f"bedroom.jsonl.frame-{linked[frame]}.ppm").is_file() and
                    (directory/f"{viewer.stem}-frame-{frame}.ppm").is_file()):
                flat=row
        if flat is None: time.sleep(.2)
    if flat is None: raise ValueError('no linked original and flat images')
    marker(directory/'remaster.enable', {'flat_frame': flat['frame'], 'session_id': session})
    on = None
    on_deadline = min(deadline, time.monotonic()+45)
    while time.monotonic()<on_deadline and on is None:
        if (directory/'batch.json').exists(): raise ValueError('runner finished before remaster image')
        for row in read_frames(viewer):
            c=candle(row)
            if (row.get('diagnostic_variant') == 'remaster' and row.get('image') is True and
                    row.get('lighting_mode') == 'preview_candle_candidate' and c and c['enabled'] and
                    c['factor'] is not None and c['factor'] > 0 and row.get('bed_override_instances') == 1 and
                    row.get('pbr_draws', 0) > 0 and row.get('bedroom_material_runs', 0) > 0 and
                    row.get('stone_wall_override_draws', 0) > 0 and row.get('wood_floor_override_draws', 0) > 0 and
                    bedroom_props_ready(row) and both_candles(row, True) and framed(row, require_other=True) and
                    same_pose(flat,row) is not None and
                    (directory/f"{viewer.stem}-remaster-frame-{row['frame']}.ppm").is_file()):
                on=row
                break
        if on is None: time.sleep(.2)
    if on is None: raise ValueError('no lit remaster image at the captured pose')
    on_candle = candle(on)
    marker(directory/'remaster.on.ready', {'frame': on['frame'], 'session_id': session})
    print('Remaster acceso acquisito; attendo lo spegnimento della candela.', flush=True)
    if announce:
        location = 'sul mobile a sinistra' if on_candle['actor_number'] == 14 else 'accanto al letto'
        announce(f'Ora spegni la candela {location} senza muoverti o ruotare. Poi rimani fermo.')
    off = None
    while time.monotonic()<deadline and off is None:
        if (directory/'batch.json').exists(): raise ValueError('runner finished before candle switch')
        for row in read_frames(viewer):
            c=candle(row)
            if (row.get('frame',0)>on['frame'] and row.get('remaster_mode') is True and
                    row.get('lighting_mode') == 'preview_candle_zero_candidate' and c and not c['enabled'] and
                    c['actor_number'] == on_candle['actor_number'] and both_candles(row, False)):
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
                    c and not c['enabled'] and c['actor_number'] == on_candle['actor_number'] and
                    bedroom_props_ready(row) and both_candles(row, False) and framed(row, require_other=True) and
                    same_pose(flat,row) is not None and same_pose(on,row) is not None and
                    (directory/f"{viewer.stem}-remaster-off-frame-{row['frame']}.ppm").is_file()):
                off_image=row
                break
        if off_image is None: time.sleep(.2)
    if off_image is None: raise ValueError('no remaster image of the off state')
    result={'session_id': session, 'trace_frame': linked[flat['frame']], 'flat_frame': flat['frame'],
            'remaster_frame': on['frame'], 'off_frame': off_image['frame'],
            'flat_to_remaster_pose': same_pose(flat,on), 'on': candle(on), 'off': candle(off_image),
            'on_to_off_pose': same_pose(on,off_image), 'other_on': candle(on, other=True),
            'other_off': candle(off_image, other=True),
            'limitations': ['The enabled property and light identity remain runtime candidates until validated against the user action.',
                            'Projected bounds do not establish visibility, occlusion, interaction range or correct UVs.',
                            'The final report must independently check the Present link and captured image pixels.']}
    marker(directory/'remaster.complete', result)
    if announce:
        announce('Acquisizione completata. Il gioco verrà chiuso automaticamente.')
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--watch', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=360)
    parser.add_argument('--speak', action='store_true', help='speak capture prompts using the macOS Italian voice')
    args=parser.parse_args()
    announce = SpokenPrompt()
    try:
        if not math.isfinite(args.timeout) or args.timeout <= 0: raise ValueError('invalid timeout')
        if args.speak and (sys.platform != 'darwin' or not Path('/usr/bin/say').is_file()):
            raise ValueError('spoken prompts require macOS say')
        print('Watcher v0.1 pronto.', flush=True)
        result=coordinate(args.watch,args.timeout,announce if args.speak else None)
        print(json.dumps({'status':'completed','comparison':result},indent=2,allow_nan=False))
        return 0
    except (OSError, ValueError, TypeError, KeyError, OverflowError, trace_summary.TraceError) as exc:
        if args.watch.is_dir() and not (args.watch/'batch.json').exists():
            try: marker(args.watch/'remaster.failed', {'error': str(exc)})
            except OSError: pass
        print(f'error: {exc}',file=sys.stderr)
        if args.speak:
            try: announce('La prova si è interrotta. Puoi tornare a Codex.')
            except OSError: pass
        return 1


if __name__=='__main__': sys.exit(main())
