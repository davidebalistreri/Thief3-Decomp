#!/usr/bin/env python3
"""Create and run the local, frozen bedroom v0.1 benchmark."""
import argparse
import contextlib
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

import compare_live_v01 as comparison
import reconstruct_live_benchmark as reconstruction
import replay_scene_live_capture as replay
import trigger_live_v01 as v01

MAP = 'SouthQuarter1_int1'
MAP_SHA = 'ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba'
PAYLOAD_FILES = ('catalog.bin', 'scene.txt', 'd3d8.ppm', 'live-flat.ppm', 'live-on.ppm', 'live-off.ppm')
FILES = PAYLOAD_FILES + ('integrity.txt',)
RENDER_ROOT = Path(__file__).resolve().parents[1]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def ident(value):
    if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z0-9_:.-]{1,512}', value):
        raise ValueError('invalid scene identifier')
    return value


def uint(value, bits=32):
    if type(value) is not int or not 0 <= value < 1 << bits:
        raise ValueError('invalid integer in recorded scene')
    return str(value)


def words(values, count):
    if not isinstance(values, (list, tuple)) or len(values) != count:
        raise ValueError('invalid recorded word array')
    return ' '.join(uint(v) for v in values)


def matrix(values):
    if not isinstance(values, list) or len(values) != 16:
        raise ValueError('invalid recorded matrix')
    return words([replay.float_bits(v) for v in values], 16)


def camera_metadata(row):
    viewport = row['viewport']
    view, projection = row['view'], row['projection']
    if not isinstance(view, list) or len(view) != 16 or not isinstance(projection, list) or len(projection) != 16:
        raise ValueError('invalid recorded camera')
    return {'view_bits': [replay.float_bits(v) for v in view],
            'projection_bits': [replay.float_bits(v) for v in projection],
            'viewport': {**{key: int(uint(viewport[key])) for key in ('x', 'y', 'width', 'height')},
                         'min_depth_bits': replay.float_bits(viewport['min_depth']),
                         'max_depth_bits': replay.float_bits(viewport['max_depth'])}}


def packet_camera(data):
    try:
        tokens = data.decode('ascii').split()
        if len(tokens) < 41 or tokens[:3] != ['T3BENCH', '1', 'CAMERA']:
            raise ValueError('missing benchmark camera')
        raw = tokens[3:41]
        if any(not word.isascii() or not word.isdecimal() or len(word) > 10 for word in raw):
            raise ValueError('invalid benchmark camera words')
        values = [int(word) for word in raw]
        for value in values: uint(value)
        view, projection, x, y, width, height, low, high = values[:16], values[16:32], *values[32:]
        real = lambda value: struct.unpack('<f', struct.pack('<I', value))[0]
        if (any(not math.isfinite(real(word)) for word in view + projection + [low, high]) or
                not 1 <= width <= 2048 or not 1 <= height <= 2048 or
                x + width > 0xffffffff or y + height > 0xffffffff or not 0 <= real(low) <= real(high) <= 1):
            raise ValueError('invalid benchmark camera values')
        return {'view_bits': view, 'projection_bits': projection,
                'viewport': {'x': x, 'y': y, 'width': width, 'height': height,
                             'min_depth_bits': low, 'max_depth_bits': high}}
    except (UnicodeError, OverflowError, struct.error) as exc:
        raise ValueError('invalid benchmark camera') from exc


def sections(values):
    if not isinstance(values, (list, tuple, set)) or not values or len(values) > 1024:
        raise ValueError('invalid section selection')
    values = sorted(values)
    if len(values) != len(set(values)):
        raise ValueError('duplicate sections')
    return f"{len(values)} " + ' '.join(uint(v) for v in values)


def link(value, actor):
    if not isinstance(value, dict) or value.get('actor_number') != actor:
        raise ValueError('missing candle actor observation')
    status = {'no_link': 1, 'candidate': 2}.get(value.get('status'))
    enabled = value.get('enabled_candidate')
    if not status or enabled not in (True, False, None) or type(enabled) not in (bool, type(None)):
        raise ValueError('invalid candle observation')
    inherited = value.get('enabled_inherited')
    if inherited not in (True, False, None) or type(inherited) not in (bool, type(None)):
        raise ValueError('invalid inherited candle observation')
    if enabled is None and inherited is not None:
        raise ValueError('inherited unknown candle state')
    factor = value.get('field_8c_bits')
    if status == 1 and factor is not None:
        raise ValueError('unexpected no-link modulation')
    enabled_code = 0 if enabled is None else 1 if enabled else 2
    return f"C {status} {uint(factor if status == 2 else 0)} {enabled_code} {int(inherited is True)} 1 " + words(value.get('transform_bits'), 16)


def batch(row, name):
    raw = row.get('light_raw')
    if row.get('light_raw_available') is not True or not isinstance(raw, dict):
        raise ValueError('missing recorded light batch')
    items = raw.get('items')
    if (not isinstance(items, list) or len(items) > 32 or raw.get('listed') != len(items) or
            raw.get('skipped') != 0 or raw.get('truncated') is not False):
        raise ValueError('incomplete recorded light batch')
    lines = [f'STATE {name}', f'LIGHTS {len(items)}', link(raw.get('candle_link'), 14),
             link(raw.get('other_candle_link'), 6)]
    seen = set()
    for item in items:
        name_id = ident(item['id'])
        if name_id in seen:
            raise ValueError('duplicate light')
        seen.add(name_id)
        line = [name_id, uint(item['generation'], 64)]
        for field in ('24', '34', '68'):
            line.append(words(item[f'field_{field}_bits'], 3))
        for field in ('58', '5c', '60', '64', '74', '78', '7c', '88', '8c', '90'):
            line.append(uint(item['field_' + field + ('_bits' if field in ('74', '78', '7c', '88', '8c', '90') else '')]))
        lines.append('L ' + ' '.join(line))
    return lines


def encode_scene(selected, actors, on, off):
    if selected.get('result') != 'published' or selected.get('map') != MAP or selected.get('map_sha256') != MAP_SHA:
        raise ValueError('replay did not publish the expected bedroom')
    if v01.same_pose(on, off) is None:
        raise ValueError('ON and OFF camera observations do not match')
    for row, state in ((on, True), (off, False)):
        for other, expected in ((False, state), (True, True)):
            candle = v01.candle(row, other=other)
            if not candle or candle['actor_number'] != (6 if other else 14) or candle['enabled'] is not expected:
                raise ValueError('ON/OFF candle candidates do not match')
    viewport = on['viewport']
    fields = [uint(viewport[k]) for k in ('x', 'y', 'width', 'height')]
    fields += [uint(replay.float_bits(viewport[k])) for k in ('min_depth', 'max_depth')]
    lines = ['T3BENCH 1', 'CAMERA ' + matrix(on['view']) + ' ' + matrix(on['projection']) + ' ' + ' '.join(fields)]
    prefix = f'actor:{MAP}:{MAP_SHA}:'
    if not actors or len(actors) > 4096 or 'StaticMeshActor__1520' not in actors:
        raise ValueError('missing captured bed or actor selections')
    lines.append(f'MESHES {len(actors)}')
    for name, (transform, chosen) in sorted(actors.items()):
        lines.append('M ' + ident(prefix + name) + ' ' + words(transform, 16) + ' ' + sections(chosen))
    bsp = selected['bsp']
    if not bsp or len(bsp) > 4096:
        raise ValueError('missing BSP selections')
    lines.append(f'BSP {len(bsp)}')
    for block in sorted(bsp, key=lambda item: item['id']):
        lines.append('B ' + ident(block['id']) + ' ' + sections(block['sections']))
    lines += batch(on, 'ON') + batch(off, 'OFF') + ['END']
    return ('\n'.join(lines) + '\n').encode('ascii')


@contextlib.contextmanager
def scene_validator(catalog):
    if len(catalog) < 40 or len(catalog) > replay.MAX_CATALOG:
        raise ValueError('benchmark input exceeds size limit')
    digest = hashlib.sha256(catalog[:-32]).digest()
    if digest != catalog[-32:]: raise ValueError('catalog fingerprint mismatch')
    compiler = shutil.which(os.environ.get('CXX', 'c++'))
    if not compiler: raise ValueError('C++ compiler required to validate the benchmark scene')
    sources = ('tools/t3bench_validate.cpp', 'src/scene_benchmark.cpp', 'src/scene_live_catalog.cpp',
               'src/scene_live_flat.cpp', 'src/scene_live_materials.cpp', 'src/scene_live_lighting.cpp', 'ir/scene_live.cpp')
    with tempfile.TemporaryDirectory(prefix='t3bench-validation-') as temp:
        root = Path(temp); executable = root/'validate'
        subprocess.run([compiler, '-std=c++17', '-O0', '-pthread', *(str(RENDER_ROOT/p) for p in sources),
                        '-o', str(executable)], check=True, capture_output=True, timeout=120)
        def run(argument):
            return subprocess.run([str(executable), str(argument), digest.hex()], input=catalog,
                                  capture_output=True, timeout=120)

        def inventory():
            result = run('--list-actors')
            if result.returncode:
                raise ValueError('cannot read catalog actor inventory')
            actors = {}
            for line in result.stdout.decode('ascii').splitlines():
                identifier, count, resource = line.split()
                ident(identifier); ident(resource)
                if identifier in actors or not count.isdecimal() or not 1 <= int(count) <= 1024:
                    raise ValueError('invalid catalog actor inventory')
                actors[identifier] = (int(count), resource)
            return actors

        def validate(scene):
            if len(scene) > 4*1024*1024:
                raise ValueError('benchmark input exceeds size limit')
            packet = root/'scene.txt'; packet.write_bytes(scene)
            result = run(packet)
            if result.returncode or result.stdout != b't3bench_scene_valid\n':
                detail = result.stderr.decode('utf-8', errors='replace').strip()
                raise ValueError('catalog and benchmark selection or candle geometry do not match' + (f': {detail}' if detail else ''))

        yield inventory, validate


def validate_scene(catalog, scene):
    with scene_validator(catalog) as (_, validate):
        validate(scene)


def select_actor_candidates(actors, inventory):
    prefix = f'actor:{MAP}:{MAP_SHA}:'
    selected = {}
    excluded_actors = excluded_sections = 0
    for name, (transform, chosen) in actors.items():
        ident(name)
        for section in chosen:
            uint(section)
        candidate = inventory.get(prefix + name)
        if candidate is None:
            excluded_actors += 1
            excluded_sections += len(chosen)
            continue
        count, _ = candidate
        eligible = {section for section in chosen if type(section) is int and 0 <= section < count}
        excluded_sections += len(chosen) - len(eligible)
        if eligible:
            selected[name] = (transform, eligible)
        else:
            excluded_actors += 1
    return selected, {'observed_candidates': len(actors), 'included_candidates': len(selected),
                      'excluded_candidates': excluded_actors, 'excluded_sections': excluded_sections}


def match_replayed_bed(selected, actors, *, exact_sections=False):
    name = 'StaticMeshActor__1520'
    identifier = f'actor:{MAP}:{MAP_SHA}:{name}'
    beds = [mesh for mesh in selected.get('meshes', []) if mesh.get('id') == identifier]
    if len(beds) != 1 or name not in actors:
        raise ValueError('replayed bed selection is missing or ambiguous')
    transform, chosen = actors[name]
    recorded = [replay.float_bits(value) for value in beds[0]['transform']]
    normalize_zero = lambda word: 0 if word == 0x80000000 else word
    if (len(transform) != 16 or len(recorded) != 16 or
            [normalize_zero(word) for word in transform] != [normalize_zero(word) for word in recorded]):
        raise ValueError('replayed bed transform differs from the trace')
    replayed_sections = beds[0]['sections']
    if (not replayed_sections or len(replayed_sections) != len(set(replayed_sections)) or
            not set(replayed_sections).issubset(chosen) or
            (exact_sections and set(replayed_sections) != chosen)):
        raise ValueError('replayed bed sections differ from the trace')
    result = dict(actors)
    result[name] = (transform, set(replayed_sections))
    return result


def prepare_scene(catalog, selected, actors, on, off):
    actors = match_replayed_bed(selected, actors)
    with scene_validator(catalog) as (inventory, validate):
        actors, selection = select_actor_candidates(actors, inventory())
        scene = encode_scene(selected, actors, on, off)
        validate(scene)
    return scene, selection


def recorded_selection(row):
    data = row.get('scene_selection')
    if not isinstance(data, dict) or data.get('truncated') is not False:
        raise ValueError('viewer scene selection is missing or truncated')
    meshes, blocks = data.get('meshes'), data.get('bsp')
    if (not isinstance(meshes, list) or not isinstance(blocks, list) or
            not 1 <= len(meshes) <= 4096 or not 1 <= len(blocks) <= 4096 or
            row.get('mesh_instances') != len(meshes) or row.get('bsp_blocks') != len(blocks)):
        raise ValueError('viewer scene selection counts differ')
    prefix = f'actor:{MAP}:{MAP_SHA}:'
    actors, resources, bsp = {}, {}, []
    def evidence(item):
        if (not isinstance(item, dict) or item.get('binding') not in ('candidate', 'verified') or
                item.get('scope') not in ('visible_list', 'full_object_scan')):
            raise ValueError('viewer selection lacks binding evidence')
    for item in meshes:
        evidence(item)
        identifier = ident(item['id'])
        if not identifier.startswith(prefix) or identifier[len(prefix):] in actors:
            raise ValueError('invalid or duplicate viewer actor')
        name = identifier[len(prefix):]
        generation = item['generation']; uint(generation, 64)
        if not generation: raise ValueError('viewer actor has no generation')
        transform = item['transform_bits']; words(transform, 16)
        chosen = item['sections']; sections(chosen)
        actors[name] = (transform, set(chosen))
        resources[name] = ident(item['resource_id'])
    seen = set()
    for item in blocks:
        evidence(item)
        identifier = ident(item['id']); chosen = item['sections']; sections(chosen)
        if identifier in seen: raise ValueError('duplicate viewer BSP')
        seen.add(identifier);bsp.append({'id': identifier, 'sections': chosen})
    return actors, resources, bsp


def prepare_observed_scene(catalog, replayed, reference, on, off):
    flat_actors, flat_resources, flat_bsp = recorded_selection(reference)
    actors, resources, bsp = recorded_selection(on)
    match_replayed_bed(replayed, flat_actors, exact_sections=True)
    def block_sections(blocks):
        return {item['id']: set(item['sections']) for item in blocks}
    if block_sections(flat_bsp) != block_sections(replayed['bsp']):
        raise ValueError('viewer BSP selection differs from the trace replay')
    prefix = f'actor:{MAP}:{MAP_SHA}:'
    with scene_validator(catalog) as (inventory, validate):
        available = inventory()
        for choices, bindings in ((flat_actors, flat_resources), (actors, resources)):
            for name, (_, chosen) in choices.items():
                candidate = available.get(prefix + name)
                if (candidate is None or candidate[1] != bindings[name] or
                        any(section >= candidate[0] for section in chosen)):
                    raise ValueError('viewer actor selection differs from the catalog')
        selected = {**replayed, 'bsp': bsp}
        scene = encode_scene(selected, actors, on, off)
        validate(scene)
    selection = {'source': 'viewer_snapshot_on', 'observed_candidates': len(actors),
                 'included_candidates': len(actors), 'excluded_candidates': 0, 'excluded_sections': 0,
                 'flat_candidates': len(flat_actors)}
    return scene, selection


def review_matches(review, capture, files):
    if (not isinstance(review, dict) or review.get('schema') != 1 or
            review.get('session_id') != capture.get('session') or
            any(review.get(key) != capture.get(key) for key in ('trace_frame', 'on_frame', 'off_frame')) or
            review.get('original_sha256') != files.get('d3d8.ppm') or
            review.get('on_sha256') != files.get('live-on.ppm') or
            review.get('off_sha256') != files.get('live-off.ppm') or
            review.get('user_confirmed_selected_candle_off') is not True or
            not re.fullmatch(r'[0-9a-f]{64}', str(review.get('comparison_sha256', '')))):
        raise ValueError('capture review does not match the selected images and session')
    visual = review.get('image_review')
    if not isinstance(visual, dict) or any(visual.get(key) is not True for key in
            ('bed_visible', 'both_candlesticks_visible', 'selected_flame_on_then_off', 'other_flame_remains_on')):
        raise ValueError('capture review is incomplete')


def capture_evidence(directory, capture, files):
    evidence = {}
    review_path = directory/'t3bench-review.json'
    if review_path.exists():
        if review_path.is_symlink() or review_path.stat().st_size > 65536:
            raise ValueError('invalid capture review file')
        raw = review_path.read_bytes(); review = json.loads(raw)
        review_matches(review, capture, files)
        comparison_path = directory/'remaster-v0.1-comparison.png'
        if (comparison_path.is_symlink() or not comparison_path.is_file() or comparison_path.stat().st_size > 64*1024*1024 or
                sha(comparison_path.read_bytes()) != review['comparison_sha256']):
            raise ValueError('capture review comparison image changed')
        evidence['review'] = {**review, 'sha256': sha(raw)}
    texture_path = directory/'texture-state.json'
    if texture_path.exists():
        if texture_path.is_symlink() or texture_path.stat().st_size > 65536:
            raise ValueError('invalid capture texture state file')
        raw = texture_path.read_bytes(); state = json.loads(raw)
        if not isinstance(state, dict): raise ValueError('invalid capture texture state')
        original, patched = state.get('original_bundle_sha256'), state.get('patched_bundle_sha256')
        if (not all(isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value) for value in (original, patched)) or
                state.get('original_sha256_after_capture') != original or state.get('catalog_sha256') != files['catalog.bin'] or
                state.get('patched_restored') is not True or state.get('runner_exit') != 0 or state.get('watcher_exit') != 0 or
                state.get('sidecar_retained') is not None or state.get('error') is not None):
            raise ValueError('capture texture state is incomplete or mismatched')
        evidence['texture_state'] = {'sha256': sha(raw), 'original_bundle_sha256': original,
                                     'patched_bundle_sha256': patched, 'patched_restored': True}
    return evidence


def create(capture, catalog_path, output):
    capture, catalog_path, output = Path(capture), Path(catalog_path), Path(output)
    if output.exists():
        raise ValueError('output already exists')
    report, image_paths = comparison.compare(capture, '0.1')
    if (report['status'] != 'images_verified' or report['two_candles'] != 'candidate_states_and_draws_checked' or
            report['framing'] != 'projected_bounds_checked' or not report['candle_off'] or len(image_paths) != 4):
        raise ValueError('capture does not contain a verified four-image, two-candle comparison')
    trace = capture / 'bedroom.jsonl'
    viewer = capture / ('scene-live-' + report['session_id'].replace(':', '-') + '.jsonl')
    trace_frame, on_frame, off_frame = report['trace_frame'], report['remaster_frame'], report['candle_off']['frame']
    rows, trace_sha, _ = replay.read_capture(trace, [trace_frame])
    replayed = replay.replay(trace, catalog_path, [trace_frame])
    if replayed['trace_sha256'] != trace_sha:
        raise ValueError('trace changed during replay')
    selected = replayed['frames'][0]
    telemetry_sha = sha(viewer.read_bytes())
    reference = reconstruction.telemetry_frame(viewer, report['flat_frame'])
    on = reconstruction.telemetry_frame(viewer, on_frame)
    off = reconstruction.telemetry_frame(viewer, off_frame)
    if any(row.get('session_id') != report['session_id'] or row.get('map') != MAP or row.get('map_sha256') != MAP_SHA
           for row in (reference, on, off)):
        raise ValueError('telemetry scene or session differs')
    if v01.same_pose(reference, on) is None or v01.same_pose(reference, off) is None:
        raise ValueError('telemetry camera or epoch mismatch')
    camera = selected.get('camera') or {}
    if (any(replay.float_bits(a) != replay.float_bits(b) for a, b in zip(camera.get('view', []), reference['view'])) or
            len(camera.get('view', [])) != 16 or matrix(camera['projection']) != matrix(reference['projection'])):
        raise ValueError('trace and telemetry camera differ')
    reference_viewport = reference['viewport']
    trace_viewport = camera['viewport']
    if (any(trace_viewport[k] != reference_viewport[k] for k in ('x', 'y', 'width', 'height')) or
            any(replay.float_bits(trace_viewport[k]) != replay.float_bits(reference_viewport[k])
                for k in ('min_depth', 'max_depth'))):
        raise ValueError('trace and telemetry viewport differ')
    catalog = catalog_path.read_bytes()
    if sha(catalog) != replayed['catalog_sha256']:
        raise ValueError('catalog changed during validation')
    batch = json.loads((capture/'batch.json').read_text())
    historical = (report['session_id'] == 'live:b1f4a4814fcf8530e324865b4624a0f6' and
                  (trace_frame, on_frame, off_frame) == (1406, 1411, 1713) and
                  sha(catalog) == 'e42c20c82a0c9de34e765ae0cad4b6b3b94ceaec9c7fa99e3e78d11a5bdc130b')
    if not historical and not (batch.get('live_catalog_sha256_before') == sha(catalog) ==
                               batch.get('live_catalog_sha256_after')):
        raise ValueError('capture does not attest the catalog fingerprint before and after the session')
    observed = ('scene_selection' in reference, 'scene_selection' in on)
    if all(observed):
        scene, actor_selection = prepare_observed_scene(catalog, selected, reference, on, off)
    elif any(observed):
        raise ValueError('viewer scene selection is incomplete across comparison frames')
    else:
        actors = reconstruction.actor_candidates(rows[trace_frame])
        scene, actor_selection = prepare_scene(catalog, selected, actors, on, off)
        actor_selection['source'] = 'trace_candidates'
    image_data = [path.read_bytes() for path in image_paths]
    expected = [report['original_sha256'], report['flat_sha256'], report['remaster_sha256'], report['candle_off']['image_sha256']]
    if [sha(data) for data in image_data] != expected:
        raise ValueError('captured image changed during validation')
    dimensions = [comparison.read_ppm(path)[:2] for path in image_paths]
    size = (on['viewport']['width'], on['viewport']['height'])
    if any(dimension != size for dimension in dimensions):
        raise ValueError('captured images and benchmark viewport have different sizes')
    if sha(viewer.read_bytes()) != telemetry_sha:
        raise ValueError('telemetry changed during validation')
    payload = dict(zip(PAYLOAD_FILES, (catalog, scene, *image_data)))
    payload['integrity.txt'] = ''.join(f'{name} {sha(payload[name])}\n' for name in PAYLOAD_FILES).encode('ascii')
    capture_metadata = {'session': report['session_id'], 'trace_frame': trace_frame, 'on_frame': on_frame,
                        'off_frame': off_frame, 'trace_sha256': trace_sha, 'telemetry_sha256': telemetry_sha,
                        'catalog_attestation': 'historical_review' if historical else 'runner_before_and_after',
                        'actor_candidates': actor_selection,
                        'camera_frame': on_frame, 'pose_delta': report['pose_delta'],
                        'off_pose_delta': report['candle_off']['pose_delta']}
    payload_hashes = {name: sha(data) for name, data in payload.items()}
    evidence = capture_evidence(capture, capture_metadata, payload_hashes)
    capture_metadata.update(evidence)
    limitations = report['limitations']
    if 'review' in evidence:
        limitations = [item for item in limitations if item !=
                       'The candle identity and enabled property are runtime candidates; user action must be confirmed.']
        limitations.append('The user confirmed the candle switch; its runtime identity and enabled property remain candidates.')
    manifest = {'format': 't3bench-bedroom-v1', 'map': MAP, 'map_sha256': MAP_SHA,
                'camera': camera_metadata(on),
                'capture': capture_metadata,
                'files': payload_hashes,
                'limitations': limitations + replayed['limitations'] + [
                    'Geometry and material bindings are offline catalog candidates.',
                    ('Actor selections are the viewer snapshot from the ON image, not verified GPU bindings.'
                     if all(observed) else
                     'Actor candidates are inferred from scoped trace draws; uncatalogued actors and sections are excluded.'),
                    'The original D3D8 and OFF observations use different recorded poses from the benchmark camera.',
                    'Frozen ON/OFF light observations are not synchronized with the benchmark camera.']}
    # Stage outside the checkout, then copy exclusively; never overwrite a package.
    with tempfile.TemporaryDirectory(prefix='t3bench-package-') as staging:
        stage = Path(staging)
        for name, data in payload.items(): (stage / name).write_bytes(data)
        (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2, allow_nan=False) + '\n')
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(stage, output)
    return manifest


def verify(package):
    package = Path(package)
    if (package/'manifest.json').stat().st_size > 1024*1024:
        raise ValueError('benchmark manifest exceeds size limit')
    manifest = json.loads((package / 'manifest.json').read_text())
    if (manifest.get('format') != 't3bench-bedroom-v1' or manifest.get('map') != MAP or
            manifest.get('map_sha256') != MAP_SHA or set(manifest.get('files', {})) != set(FILES)):
        raise ValueError('invalid benchmark manifest')
    for name in FILES:
        path = package / name
        limit = (replay.MAX_CATALOG if name == 'catalog.bin' else 4*1024*1024 if name == 'scene.txt' else
                 4096 if name == 'integrity.txt' else 16*1024*1024)
        if (path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= limit or
                sha(path.read_bytes()) != manifest['files'][name]):
            raise ValueError(f'package file missing or changed: {name}')
    catalog = (package/'catalog.bin').read_bytes()
    if len(catalog) < 40 or len(catalog) > replay.MAX_CATALOG or hashlib.sha256(catalog[:-32]).digest() != catalog[-32:]:
        raise ValueError('invalid catalog fingerprint')
    camera = packet_camera((package/'scene.txt').read_bytes())
    if json.dumps(manifest.get('camera'), sort_keys=True, allow_nan=False) != json.dumps(camera, sort_keys=True):
        raise ValueError('manifest and benchmark camera differ')
    expected_integrity = ''.join(f'{name} {manifest["files"][name]}\n' for name in PAYLOAD_FILES).encode('ascii')
    if (package/'integrity.txt').read_bytes() != expected_integrity:
        raise ValueError('integrity index disagrees with manifest')
    sizes = [comparison.read_ppm(package / name)[:2] for name in PAYLOAD_FILES[2:]]
    if any(size != sizes[0] for size in sizes): raise ValueError('captured image sizes differ')
    if sizes[0] != (camera['viewport']['width'], camera['viewport']['height']):
        raise ValueError('captured image and benchmark camera sizes differ')
    capture = manifest.get('capture', {})
    if 'review' in capture:
        review = capture['review']
        review_matches(review, capture, manifest['files'])
        if not re.fullmatch(r'[0-9a-f]{64}', str(review.get('sha256', ''))):
            raise ValueError('invalid capture review fingerprint')
    if 'texture_state' in capture:
        state = capture['texture_state']
        if (not isinstance(state, dict) or state.get('patched_restored') is not True or
                any(not isinstance(state.get(key), str) or not re.fullmatch(r'[0-9a-f]{64}', state[key])
                    for key in ('sha256', 'original_bundle_sha256', 'patched_bundle_sha256'))):
            raise ValueError('invalid capture texture state')
    return manifest


def metrics(a, b):
    wa, ha, pa = comparison.read_ppm(a)
    wb, hb, pb = comparison.read_ppm(b)
    if (wa, ha) != (wb, hb): raise ValueError('image sizes differ')
    total = sum(abs(x-y) for x, y in zip(pa, pb))
    square = sum((x-y)**2 for x, y in zip(pa, pb))
    return {'width': wa, 'height': ha, 'different_pixels': sum(pa[i:i+3] != pb[i:i+3] for i in range(0, len(pa), 3)),
            'mean_absolute_rgb': total / len(pa), 'rmse_rgb': math.sqrt(square / len(pa))}


def sheet(images, target):
    loaded = [comparison.read_ppm(image) for image in images]
    width, height, _ = loaded[0]
    if any((w, h) != (width, height) for w, h, _ in loaded): raise ValueError('image sizes differ')
    with target.open('xb') as out:
        out.write(f'P6\n{width * len(loaded)} {height}\n255\n'.encode('ascii'))
        for y in range(height):
            for _, _, pixels in loaded:
                out.write(pixels[y*width*3:(y+1)*width*3])


def bitmap_sheet(images, target):
    loaded = [comparison.read_ppm(image) for image in images]
    width, height, _ = loaded[0]
    if any((w, h) != (width, height) for w, h, _ in loaded): raise ValueError('image sizes differ')
    total_width = width * len(loaded)
    stride = (total_width * 3 + 3) & ~3
    with target.open('xb') as out:
        out.write(struct.pack('<2sIHHI', b'BM', 54 + stride*height, 0, 0, 54))
        out.write(struct.pack('<IiiHHIIiiII', 40, total_width, height, 1, 24, 0, stride*height, 0, 0, 0, 0))
        for y in range(height-1, -1, -1):
            for _, _, pixels in loaded:
                rgb = pixels[y*width*3:(y+1)*width*3]
                bgr = bytearray(len(rgb));bgr[0::3], bgr[1::3], bgr[2::3] = rgb[2::3], rgb[1::3], rgb[0::3]
                out.write(bgr)
            out.write(bytes(stride-total_width*3))


def run(package, renderer, output, runner=(), wine_paths=False, environment=None):
    package, renderer, output = Path(package).resolve(), Path(renderer).resolve(), Path(output).resolve()
    manifest = verify(package)
    if not renderer.is_file(): raise ValueError('renderer executable is missing')
    if output.exists(): raise ValueError('output already exists')
    if package == output or package in output.parents: raise ValueError('output must be outside the package')
    output.mkdir(parents=True)
    paths = [package/'catalog.bin', package/'scene.txt', *(output/(name+'.ppm') for name in ('flat', 'on', 'off'))]
    def argument(path):
        return 'Z:' + str(path).replace('/', '\\') if wine_paths else str(path)
    result = subprocess.run([*runner, str(renderer), *(argument(path) for path in paths)], check=True,
                            capture_output=True, text=True, timeout=300, env=environment)
    if verify(package) != manifest: raise ValueError('package changed during rendering')
    rendered = [output/(name+'.ppm') for name in ('flat', 'on', 'off')]
    sheet([package/'d3d8.ppm', *rendered], output/'comparison.ppm')
    bitmap_sheet([package/'d3d8.ppm', *rendered], output/'comparison.bmp')
    stats = json.loads(result.stdout.strip().splitlines()[-1])
    stats.update({'format': 't3bench-result-v1', 'package_files': manifest['files'],
                  'camera': manifest['camera'],
                  'images': {path.name: sha(path.read_bytes()) for path in
                             [*rendered, output/'comparison.ppm', output/'comparison.bmp']},
                  'differences': {'flat_vs_on': metrics(rendered[0], rendered[1]),
                                  'on_vs_off': metrics(rendered[1], rendered[2]),
                                  'd3d8_vs_flat': metrics(package/'d3d8.ppm', rendered[0]),
                                  'd3d8_vs_on': metrics(package/'d3d8.ppm', rendered[1]),
                                  'd3d8_vs_off': metrics(package/'d3d8.ppm', rendered[2]),
                                  'live_flat_vs_flat': metrics(package/'live-flat.ppm', rendered[0]),
                                  'live_on_vs_on': metrics(package/'live-on.ppm', rendered[1]),
                                  'live_off_vs_off': metrics(package/'live-off.ppm', rendered[2])},
                  'limitations': manifest['limitations'] + ['CPU timings include GPU readback and are not game frame times.']})
    (output/'metrics.json').write_text(json.dumps(stats, indent=2, allow_nan=False) + '\n')
    return stats


def run_highball(package, renderer, output, engine, bottle):
    # Set DYLD_* in the Python process itself: macOS strips these variables
    # when starting the system Python executable from a shell.
    from scene_pbr_camera_sequence import highball_environment
    wine, environment = highball_environment(engine, bottle)
    return run(package, renderer, output, [str(wine)], True, environment)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    create_parser = commands.add_parser('create')
    create_parser.add_argument('--capture', type=Path, required=True)
    create_parser.add_argument('--catalog', type=Path, required=True)
    create_parser.add_argument('--output', type=Path, required=True)
    verify_parser = commands.add_parser('verify'); verify_parser.add_argument('package', type=Path)
    run_parser = commands.add_parser('run'); run_parser.add_argument('package', type=Path)
    run_parser.add_argument('--renderer', type=Path, required=True); run_parser.add_argument('--output', type=Path, required=True)
    run_parser.add_argument('--runner', action='append', default=[], help='renderer launcher argument; repeat for each argument')
    run_parser.add_argument('--wine-paths', action='store_true')
    run_parser.add_argument('--highball-engine', type=Path, help='existing Highball engine for offscreen Wine/D3D12')
    run_parser.add_argument('--highball-bottle', type=Path, help='existing Highball bottle with Z: mapped to /')
    args = parser.parse_args(argv)
    try:
        if args.command == 'run' and (bool(args.highball_engine) != bool(args.highball_bottle) or
                                      (args.highball_engine and (args.runner or args.wine_paths))):
            raise ValueError('highball-engine and highball-bottle must be paired and cannot be combined with runner or wine-paths')
        result = (create(args.capture, args.catalog, args.output) if args.command == 'create' else
                  verify(args.package) if args.command == 'verify' else
                  run_highball(args.package, args.renderer, args.output, args.highball_engine, args.highball_bottle)
                  if args.highball_engine else
                  run(args.package, args.renderer, args.output, args.runner, args.wine_paths))
        print(json.dumps(result, indent=2, allow_nan=False))
    except subprocess.CalledProcessError as exc:
        print(f'error: renderer failed: {exc.stderr or exc}; partial output: {args.output}', file=sys.stderr)
        return 1
    except (OSError, ValueError, TypeError, KeyError, IndexError, subprocess.SubprocessError) as exc:
        partial = f'; partial output: {args.output}' if args.command in ('run', 'create') and args.output.exists() else ''
        print(f'error: {exc}{partial}', file=sys.stderr); return 1
    return 0


if __name__ == '__main__': sys.exit(main())
