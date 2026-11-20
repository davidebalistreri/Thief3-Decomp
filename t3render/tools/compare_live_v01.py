#!/usr/bin/env python3
"""Verify and assemble the same-session D3D8, flat and remaster images."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import analyze_tds_capture
import compare_live_present
import trigger_live_v01 as v01


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_ppm(path):
    with path.open('rb') as source:
        if source.readline() != b'P6\n':
            raise ValueError(f'{path.name}: expected P6 image')
        dimensions = source.readline().split()
        if len(dimensions) != 2 or source.readline().strip() != b'255':
            raise ValueError(f'{path.name}: invalid PPM header')
        width, height = map(int, dimensions)
        if not 1 <= width <= 2048 or not 1 <= height <= 2048:
            raise ValueError(f'{path.name}: invalid dimensions')
        pixels = source.read(width*height*3+1)
        if len(pixels) != width*height*3:
            raise ValueError(f'{path.name}: invalid image length')
        return width, height, pixels


def image_difference(a, b):
    wa, ha, pa = read_ppm(a)
    wb, hb, pb = read_ppm(b)
    if (wa, ha) != (wb, hb):
        raise ValueError('flat and remaster images have different viewports')
    changed = sum(pa[i:i+3] != pb[i:i+3] for i in range(0, len(pa), 3))
    mean = sum(abs(x-y) for x,y in zip(pa,pb))/len(pa)
    return {'width': wa, 'height': ha, 'different_pixels': changed, 'mean_absolute_rgb': mean}


def compare(directory, version='0.1'):
    if version not in ('0.1', '0.2'):
        raise ValueError('unsupported remaster comparison version')
    directory = Path(directory)
    enabled = json.loads((directory/'remaster.enable').read_text())
    on_ready = json.loads((directory/'remaster.on.ready').read_text())
    batch = json.loads((directory/'batch.json').read_text())
    session = enabled['session_id']
    if on_ready.get('session_id') != session or not isinstance(session, str) or not session.startswith('live:'):
        raise ValueError('remaster markers have different or invalid session identities')
    viewer = directory/('scene-live-'+session.replace(':','-')+'.jsonl')
    trace = directory/'bedroom.jsonl'
    comparison = compare_live_present.compare(trace, viewer)
    flat_frame, on_frame = enabled['flat_frame'], on_ready['frame']
    matched = [m for m in comparison['matches'] if m['live_frame'] == flat_frame]
    if comparison['session_id'] != session or len(matched) != 1:
        raise ValueError('flat image does not have one verified Present/camera match')
    summary_path = directory/'summary.json'
    summary = json.loads(summary_path.read_text()) if summary_path.is_file() else analyze_tds_capture.analyze(trace)
    captured = [r for r in summary.get('frames', []) if r.get('frame') == matched[0]['trace_frame']]
    if summary.get('trace_sha256') != comparison['trace_sha256'] or len(captured) != 1:
        raise ValueError('capture summary does not match the linked trace frame')
    rows = v01.read_frames(viewer)
    def one(frame):
        result = [r for r in rows if r.get('frame') == frame]
        if len(result) != 1: raise ValueError(f'ambiguous viewer frame {frame}')
        return result[0]
    flat, on = one(flat_frame), one(on_frame)
    # Old captures predate framing diagnostics. If any selected frame has
    # them, every selected frame must pass the new gate.
    uses_framing = any(k in row for row in (flat, on) for k in ('bed_framing', 'candle_framing'))
    uses_two_candle_framing = any('other_candle_framing' in row for row in (flat, on))
    uses_door_crates = any('bedroom_door_crate_instances' in row for row in (flat, on))
    pose = v01.same_pose(flat, on)
    state = v01.candle(on)
    actor_number = batch.get('candle_actor_number', 6)
    max_shadowed_lights = 3 if version == '0.2' else 2 if 'preview_selected_candlestick_draws' in on else 1
    if (pose is None or flat.get('remaster_mode') is not False or flat.get('image') is not True or
            on.get('diagnostic_variant') != 'remaster' or on.get('image') is not True or
            on.get('lighting_mode') != 'preview_candle_candidate' or not state or not state['enabled'] or
            type(actor_number) is not int or actor_number not in (6, 14) or state['actor_number'] != actor_number or
            state['factor'] is None or state['factor'] <= 0 or on.get('bed_override_instances') != 1 or
            on.get('pbr_draws', 0) <= 0 or on.get('bedroom_material_runs', 0) <= 0 or
            on.get('stone_wall_override_draws', 0) <= 0 or on.get('wood_floor_override_draws', 0) <= 0 or
            on.get('preview_gi_enabled') is not (version == '0.2') or on.get('wall_displacement_triangles') != 0 or
            (uses_framing and (not v01.framed(flat) or not v01.framed(on))) or
            (uses_two_candle_framing and (not v01.framed(flat, require_other=True) or
                 not v01.framed(on, require_other=True) or
                 not v01.bedroom_props_ready(flat, uses_door_crates) or not v01.bedroom_props_ready(on, uses_door_crates) or
                 not v01.both_candles(flat, True, require_geometry=False) or not v01.both_candles(on, True))) or
            not v01.candle_geometry(on, True) or
            type(on.get('shadowed_lights')) is not int or not 0 <= on['shadowed_lights'] <= max_shadowed_lights):
        raise ValueError('remaster image lacks the required pose, overrides or live candle candidate')
    prefix = viewer.stem
    original = directory/f"bedroom.jsonl.frame-{matched[0]['trace_frame']}.ppm"
    base = directory/f'{prefix}-frame-{flat_frame}.ppm'
    remaster = directory/f'{prefix}-remaster-frame-{on_frame}.ppm'
    for path in (original, base, remaster):
        read_ppm(path)
    image_record = captured[0].get('frame_image')
    if (not isinstance(image_record, dict) or image_record.get('status') != 'captured' or
            image_record.get('stage') != 'complete' or image_record.get('present_source_rect') is not False or
            image_record.get('present_dest_rect') is not False or image_record.get('sha256') != digest(original)):
        raise ValueError('D3D8 image is not the validated pre-Present capture')
    difference = image_difference(base, remaster)
    if not difference['different_pixels']:
        raise ValueError('flat and remaster images are identical')
    off = None
    other_off = None
    images = [original, base, remaster]
    complete_path = directory/'remaster.complete'
    if complete_path.is_file():
        complete = json.loads(complete_path.read_text())
        if (complete.get('session_id') != session or complete.get('flat_frame') != flat_frame or
                complete.get('remaster_frame') != on_frame):
            raise ValueError('completed comparison marker contradicts the selected frames')
        off_row = one(complete['off_frame'])
        uses_framing = uses_framing or any(k in off_row for k in ('bed_framing', 'candle_framing'))
        uses_two_candle_framing = uses_two_candle_framing or 'other_candle_framing' in off_row
        off_state = v01.candle(off_row)
        other_off = v01.candle(off_row, other=True) if uses_two_candle_framing else None
        off_image = directory/f"{prefix}-remaster-off-frame-{complete['off_frame']}.ppm"
        off_pose = v01.same_pose(flat, off_row)
        on_off_pose = v01.same_pose(on, off_row)
        if (off_row.get('diagnostic_variant') != 'remaster_off' or off_row.get('image') is not True or
                off_row.get('lighting_mode') != 'preview_candle_zero_candidate' or not off_state or off_state['enabled'] or
                off_state['actor_number'] != actor_number or
                (version == '0.2' and (off_row.get('preview_gi_enabled') is not True or
                                       off_row.get('wall_displacement_triangles') != 0)) or
                off_pose is None or on_off_pose is None or not v01.candle_geometry(off_row, False) or
                (uses_two_candle_framing and (not v01.framed(flat, require_other=True) or
                 not v01.framed(on, require_other=True) or not v01.framed(off_row, require_other=True) or
                 ('bedroom_door_crate_instances' in off_row) != uses_door_crates or
                 not v01.bedroom_props_ready(flat, uses_door_crates) or not v01.bedroom_props_ready(on, uses_door_crates) or
                 not v01.bedroom_props_ready(off_row, uses_door_crates) or
                 not v01.both_candles(flat, True, require_geometry=False) or
                 not v01.both_candles(on, True) or not v01.both_candles(off_row, False))) or
                (uses_framing and (not v01.framed(flat) or not v01.framed(on) or not v01.framed(off_row)))):
            raise ValueError('candle off image lacks a matching runtime candidate')
        read_ppm(off_image)
        off = {'frame': complete['off_frame'], 'candidate': off_state, 'image_sha256': digest(off_image),
               'pose_delta': off_pose, 'on_to_off_pose': on_off_pose}
        images.append(off_image)
    status = ('images_verified_candle_off_missing' if not off else
              'images_verified' if uses_two_candle_framing else 'images_verified_two_candles_unverified')
    return {'status': status,
            'session_id': session, 'trace_frame': matched[0]['trace_frame'], 'flat_frame': flat_frame,
            'remaster_frame': on_frame, 'pose_delta': pose,
            'original_sha256': digest(original), 'flat_sha256': digest(base), 'remaster_sha256': digest(remaster),
            'remaster_counts': {k: on.get(k) for k in ('mesh_instances','bsp_blocks','draws','triangles',
                                                        'bed_override_instances','bedroom_material_runs','pbr_draws',
                                                        'stone_wall_override_draws','wood_floor_override_draws',
                                                        'bed_source_instances','bedroom_crate_instances','bedroom_door_crate_instances','bedroom_cabinet_instances',
                                                        'bedroom_bread_instances','bedroom_target_instances',
                                                        'preview_candlestick_draws','preview_flame_draws',
                                                        'preview_selected_candlestick_draws','preview_selected_flame_draws',
                                                        'preview_other_candlestick_draws','preview_other_flame_draws')},
            'candle_on': state, 'candle_off': off, 'flat_vs_remaster': difference,
            'other_candle_on': v01.candle(on, other=True) if uses_two_candle_framing else None,
            'other_candle_off': other_off,
            'two_candles': ('candidate_states_and_draws_checked' if uses_two_candle_framing and off else
                            'unverified'),
            'framing': 'projected_bounds_checked' if uses_framing else 'unavailable_historical_capture',
            'cleanup': {k: batch.get(k) for k in ('status','error','original_saves_unchanged','sandbox_removed',
                                                   'temporary_proxy_removed')},
            'limitations': ['The candle identity and enabled property are runtime candidates; user action must be confirmed.',
                            'Flat and remaster images are different frames from the same session and their pose delta is reported.',
                            'The D3D8 image is captured before Present; display color transformations are unknown.',
                            'The viewer is partial; screenshots do not prove GPU resource bindings or total scene parity.',
                            'Projected bounds do not establish visibility, occlusion, interaction range or correct UVs.',
                            'Candle intensity and range are artistic calibration.']}, tuple(images)


def write_sheet(images, output):
    labels = ('Original D3D8', 'DX12 Flat', 'Remaster ON', 'Remaster + Candle OFF')
    command = ['magick']
    for path, label in zip(images, labels):
        command += ['(', str(path)]
        command += ['-resize','640x480!', '-background','#202832','-gravity','north','-splice','0x30',
                    '-font','/System/Library/Fonts/Supplemental/Arial.ttf','-fill','white','-pointsize','18',
                    '-annotate','+0+5',label,')']
    command += ['+append', str(output)]
    subprocess.run(command, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--version', choices=('0.1', '0.2'), default='0.1')
    args = parser.parse_args()
    try:
        report, images = compare(args.directory, args.version)
        image = args.directory/f'remaster-v{args.version}-comparison.png'
        data = args.directory/f'remaster-v{args.version}-comparison.json'
        if image.exists() or data.exists(): raise ValueError('comparison output already exists')
        write_sheet(images, image)
        data.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
        print(json.dumps(report,indent=2,allow_nan=False))
    except (OSError, ValueError, TypeError, KeyError, subprocess.SubprocessError) as exc:
        print(f'error: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__': sys.exit(main())
