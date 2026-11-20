"""Optional offscreen integration test with synthetic, non-game assets."""
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_t3bench as fixture

t3bench = fixture.t3bench


def synthetic_package(root):
    """Create a visible synthetic wall and two distinct candle candidates."""
    data = json.loads(json.dumps(fixture.catalog_fixture.static_fixture()).replace('a'*64, t3bench.MAP_SHA))
    data['conventions']['meters_per_unit'] = .01905
    data['materials'][0]['name'] = 'STMwall512D_D'
    geometry = data['bsp_blocks'][0]['geometry']
    geometry['positions'] = [[0, -40, 0], [0, 40, 0], [0, -40, 40]]
    geometry['normals'] = [[1, 0, 0]] * 3
    source = fixture.scene_ir.SceneSnapshot.from_dict(data)
    catalog, _ = fixture.catalog_fixture.catalog.export(
        json.dumps(data).encode(), fixture.bed_fixture.glb(source), fixture.catalog_fixture.TinyTextures(), True)
    selected, actors, on, off = fixture.fixture()
    selected['bsp'][0]['id'] = data['bsp_blocks'][0]['id']
    view = [0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -20, 50, 1]
    projection = [.02, 0, 0, 0, 0, .02, 0, 0, 0, 0, .01, 0, 0, 0, 0, 1]
    for row in (on, off):
        row['view'] = view.copy(); row['projection'] = projection.copy()
        row['viewport'].update(width=64, height=64)
        for key, y in (('candle_link', -15), ('other_candle_link', 15)):
            link = row['light_raw'][key]
            link['transform_bits'] = link['transform_bits'].copy()
            link['transform_bits'][13:15] = [fixture.bits(y), fixture.bits(5)]
        for item in row['light_raw']['items']:
            y = -15 if item['id'].endswith(':14') else 15
            item['field_34_bits'] = [fixture.bits(0), fixture.bits(y), fixture.bits(5)]
    packet = t3bench.encode_scene(selected, actors, on, off)
    t3bench.validate_scene(catalog, packet)
    ppm = b'P6\n64 64\n255\n' + bytes([20, 25, 30]) * (64*64)
    payload = dict(zip(t3bench.PAYLOAD_FILES, (catalog, packet, ppm, ppm, ppm, ppm)))
    payload['integrity.txt'] = ''.join(
        f'{name} {t3bench.sha(payload[name])}\n' for name in t3bench.PAYLOAD_FILES).encode('ascii')
    root.mkdir()
    for name, contents in payload.items(): (root/name).write_bytes(contents)
    manifest = {'format': 't3bench-bedroom-v1', 'map': t3bench.MAP, 'map_sha256': t3bench.MAP_SHA,
                'camera': t3bench.camera_metadata(on),
                'files': {name: t3bench.sha(contents) for name, contents in payload.items()},
                'limitations': ['Synthetic test data; this is not a captured bedroom or golden package.']}
    (root/'manifest.json').write_text(json.dumps(manifest)+'\n')
    return root


def real_catalog_synthetic_package(root, catalog_path, scene_path):
    """Use local catalog geometry with a synthetic camera and candle states."""
    catalog = Path(catalog_path).read_bytes()
    offline = json.loads(Path(scene_path).read_text())
    bed = next(item for item in offline['mesh_instances'] if item['id'].endswith(':StaticMeshActor__1520'))
    selected, _, on, off = fixture.fixture()
    selected['bsp'] = [{'id': block['id'], 'sections': list(range(len(block['geometry']['sections'])))}
                       for block in offline['bsp_blocks']]
    actors = {'StaticMeshActor__1520': ([fixture.bits(value) for value in bed['transform']], {0, 1, 2})}
    # A synthetic look-at camera based on the local Godot preview, converted to T3 units.
    eye = [66.9/.01905, 29.2/.01905, 6.4/.01905]
    target = [65.7/.01905, 28.1/.01905, 5.9/.01905]
    def norm(vector):
        length = math.sqrt(sum(value*value for value in vector))
        return [value/length for value in vector]
    def cross(a, b):
        return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]
    def dot(a, b): return sum(x*y for x, y in zip(a, b))
    z = norm([b-a for a, b in zip(eye, target)])
    x = norm(cross([0, 0, 1], z)); y = cross(z, x)
    view = [x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0,
            -dot(x, eye), -dot(y, eye), -dot(z, eye), 1]
    focal = 1/math.tan(math.radians(70)/2); near, far = 2.6, 20000
    projection = [focal, 0, 0, 0, 0, focal, 0, 0, 0, 0, far/(far-near), 1, 0, 0, -near*far/(far-near), 0]
    for row in (on, off):
        row['view'] = view.copy(); row['projection'] = projection.copy()
        row['viewport'].update(width=256, height=256)
        for key, px, py in (('candle_link', 3420, 1514), ('other_candle_link', 3440, 1495)):
            row['light_raw'][key]['transform_bits'][12:15] = [fixture.bits(px), fixture.bits(py), fixture.bits(330)]
        for light in row['light_raw']['items']:
            px, py = (3420, 1514) if light['id'].endswith(':14') else (3440, 1495)
            light['field_34_bits'] = [fixture.bits(px), fixture.bits(py), fixture.bits(330)]
    packet = t3bench.encode_scene(selected, actors, on, off)
    t3bench.validate_scene(catalog, packet)
    ppm = b'P6\n256 256\n255\n' + bytes([20, 25, 30])*(256*256)
    payload = dict(zip(t3bench.PAYLOAD_FILES, (catalog, packet, ppm, ppm, ppm, ppm)))
    payload['integrity.txt'] = ''.join(
        f'{name} {t3bench.sha(payload[name])}\n' for name in t3bench.PAYLOAD_FILES).encode('ascii')
    root.mkdir()
    for name, contents in payload.items(): (root/name).write_bytes(contents)
    manifest = {'format': 't3bench-bedroom-v1', 'map': t3bench.MAP, 'map_sha256': t3bench.MAP_SHA,
                'camera': t3bench.camera_metadata(on), 'files': {name: t3bench.sha(data) for name, data in payload.items()},
                'limitations': ['Synthetic camera, selection, lights and D3D8 image; this is not a captured golden.']}
    (root/'manifest.json').write_text(json.dumps(manifest)+'\n')
    return root


@unittest.skipUnless(os.environ.get('T3BENCH_D3D12_RENDERER'), 'D3D12 renderer not supplied')
class D3D12IntegrationTest(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('T3BENCH_REAL_CATALOG') and os.environ.get('T3BENCH_REAL_SCENE'),
                         'local offline catalog and scene not supplied')
    def test_real_catalog_with_synthetic_camera_and_lights(self):
        renderer = Path(os.environ['T3BENCH_D3D12_RENDERER'])
        engine, bottle = os.environ.get('T3BENCH_D3D12_ENGINE'), os.environ.get('T3BENCH_D3D12_BOTTLE')
        self.assertEqual(bool(engine), bool(bottle), 'Highball engine and bottle must be supplied together')
        runner = [os.environ['T3BENCH_D3D12_RUNNER']] if os.environ.get('T3BENCH_D3D12_RUNNER') else []
        wine_paths = os.environ.get('T3BENCH_D3D12_WINE_PATHS') == '1'
        with tempfile.TemporaryDirectory(prefix='t3bench-real-catalog-test-') as temp:
            root = Path(temp)
            package = real_catalog_synthetic_package(root/'synthetic-package', os.environ['T3BENCH_REAL_CATALOG'],
                                                     os.environ['T3BENCH_REAL_SCENE'])
            if engine and bottle:
                first = t3bench.run_highball(package, renderer, root/'run-1', engine, bottle)
                second = t3bench.run_highball(package, renderer, root/'run-2', engine, bottle)
            else:
                first = t3bench.run(package, renderer, root/'run-1', runner, wine_paths)
                second = t3bench.run(package, renderer, root/'run-2', runner, wine_paths)
            self.assertEqual(first['images'], second['images'])
            self.assertEqual(first['draws'], second['draws'])
            self.assertEqual(first['shadowed_lights'], [0, 2, 1])
            self.assertGreater(first['differences']['flat_vs_on']['different_pixels'], 0)
            self.assertGreater(first['differences']['on_vs_off']['different_pixels'], 0)

    def test_offscreen_variants_are_repeatable(self):
        renderer = Path(os.environ['T3BENCH_D3D12_RENDERER'])
        engine, bottle = os.environ.get('T3BENCH_D3D12_ENGINE'), os.environ.get('T3BENCH_D3D12_BOTTLE')
        self.assertEqual(bool(engine), bool(bottle), 'Highball engine and bottle must be supplied together')
        runner = [os.environ['T3BENCH_D3D12_RUNNER']] if os.environ.get('T3BENCH_D3D12_RUNNER') else []
        wine_paths = os.environ.get('T3BENCH_D3D12_WINE_PATHS') == '1'
        with tempfile.TemporaryDirectory(prefix='t3bench-d3d12-test-') as temp:
            root = Path(temp); package = synthetic_package(root/'synthetic-package')
            try:
                if engine and bottle:
                    first = t3bench.run_highball(package, renderer, root/'run-1', engine, bottle)
                    second = t3bench.run_highball(package, renderer, root/'run-2', engine, bottle)
                else:
                    first = t3bench.run(package, renderer, root/'run-1', runner, wine_paths)
                    second = t3bench.run(package, renderer, root/'run-2', runner, wine_paths)
            except subprocess.CalledProcessError as exc:
                self.fail(f'renderer exited {exc.returncode}: {exc.stderr or exc.stdout}')
            self.assertEqual(first['images'], second['images'])
            self.assertGreater(first['differences']['flat_vs_on']['different_pixels'], 0)
            self.assertGreater(first['differences']['on_vs_off']['different_pixels'], 0)
            self.assertEqual(first['draws'], second['draws'])
            self.assertEqual(first['triangles'], second['triangles'])

    def test_live_snapshot_uses_the_same_camera(self):
        live_fixture = os.environ.get('T3BENCH_D3D12_LIVE_FIXTURE')
        if not live_fixture: self.skipTest('live viewer fixture not supplied')
        renderer = Path(os.environ['T3BENCH_D3D12_RENDERER'])
        engine, bottle = os.environ.get('T3BENCH_D3D12_ENGINE'), os.environ.get('T3BENCH_D3D12_BOTTLE')
        self.assertEqual(bool(engine), bool(bottle), 'Highball engine and bottle must be supplied together')
        runner = [os.environ['T3BENCH_D3D12_RUNNER']] if os.environ.get('T3BENCH_D3D12_RUNNER') else []
        wine_paths = os.environ.get('T3BENCH_D3D12_WINE_PATHS') == '1'
        environment = None
        if engine and bottle:
            from scene_pbr_camera_sequence import highball_environment
            wine, environment = highball_environment(engine, bottle)
            runner, wine_paths = [str(wine)], True
        with tempfile.TemporaryDirectory(prefix='t3bench-live-test-') as temp:
            root = Path(temp);package = synthetic_package(root/'synthetic-package')
            baseline = t3bench.run(package, renderer, root/'offline', runner, wine_paths, environment)
            output = root/'live';output.mkdir()
            def argument(path):
                value = str(path.resolve())
                return 'Z:' + value.replace('/', '\\') if wine_paths else value
            result = subprocess.run([*runner, live_fixture, argument(package), argument(output)],
                                    capture_output=True, text=True, timeout=90, env=environment)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('t3bench_live_fixture=ok', result.stdout)
            directory = output/('t3bench-live-'+'a'*32)
            metrics = json.loads((directory/'metrics.json').read_text())
            self.assertGreater(metrics['matched_live_meshes'], 0)
            self.assertEqual(metrics['camera'], 'golden')
            self.assertEqual(metrics['camera_view_bits'], baseline['camera']['view_bits'])
            self.assertEqual(metrics['camera_projection_bits'], baseline['camera']['projection_bits'])
            self.assertEqual(metrics['viewport'], baseline['camera']['viewport'])
            self.assertEqual(metrics['package']['scene_sha256'], baseline['package_files']['scene.txt'])
            self.assertEqual(metrics['package']['catalog_sha256'], baseline['package_files']['catalog.bin'])
            self.assertEqual(metrics['package']['d3d8_sha256'], baseline['package_files']['d3d8.ppm'])
            log = output/('scene-live-live-'+'a'*32+'.jsonl')
            frames = [json.loads(line) for line in log.read_text().splitlines()]
            captured = [row for row in frames if row.get('event') == 'frame' and row.get('frame') == 42]
            self.assertEqual(len(captured), 1)
            self.assertTrue(captured[0]['image'])
            actors, resources, bsp = t3bench.recorded_selection(captured[0])
            self.assertEqual(set(actors), {'StaticMeshActor__1520'})
            self.assertEqual(actors['StaticMeshActor__1520'][1], {0})
            self.assertTrue(resources['StaticMeshActor__1520'].endswith(':innbed1a1'))
            self.assertEqual(len(bsp), 1)
            for name in ('flat.ppm', 'on.ppm', 'off.ppm', 'comparison.ppm', 'comparison.bmp'):
                self.assertEqual(t3bench.sha((directory/name).read_bytes()), baseline['images'][name])


if __name__ == '__main__': unittest.main()
