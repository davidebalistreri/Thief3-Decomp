# Independent SceneIR PBR/HDR preview

`t3render_scene_pbr_d3d12` is a separate, standalone SceneIR consumer. It uses
the synchronized camera and one scoped set of asset-backed geometry, not the
legacy draw states, shaders or multipass replay. `scene_pbr_packet.py`
revalidates both the geometry snapshot and a same-frame candidate-light
snapshot against the trace and fingerprinted offline assets. The snapshots'
existing provenance limitations remain in the packet manifest.

The exporter selects the same bed-relative BSP wall and floor replacements as
the Godot prototype. Their locally generated albedo, OpenGL normal, roughness
and AO PNGs become bounded 512² RGBA8 mip chains; albedo mips are filtered in
linear light and encoded back to sRGB. Other opaque materials use
their offline diffuse texture, geometric normals, roughness 0.8 and AO 1.
The world-space expansion reverses triangle winding for mirrored instances.
Sections whose offline diffuse has non-opaque alpha are excluded and listed
in the manifest; alpha alone does not prove the original blend mode. Runtime
light positions are scoped **candidates**. The calibration file selects
omni/point lights and supplies artistic linear colors, intensities, source
radii, ambient and exposure. These choices are not measured runtime values.
Spotlights are rejected rather than approximated as point lights.

The albedo texture uses an sRGB view, so hardware decodes before filtering;
normal, roughness and AO use linear views. The original assets' precomputed
diffuse mips have unknown filtering. The pixel shader samples the
UV-inferred tangent-space normal, and evaluates a dielectric Cook–Torrance
GGX/Schlick BRDF. Point-light energy falls with inverse squared distance in
metres, clamped inside the supplied source radius. A constant ambient term
is modulated by AO. Scene-linear light accumulates in an RGBA16F target;
energy is clamped to the finite half-float range before a separate exposure,
Reinhard and sRGB display pass. The PPM is an SDR preview of the HDR buffer.

This is a first PBR preview, not a physically calibrated lighting solution:
source strengths, color and exposure are artistic; the SDK metre scale and
tangent basis are assumptions. Metallic materials, shadows, GI, emissive
lighting, volumetrics, reflections and temporal effects are absent. The
render is a partial runtime-selected scene, and offline asset bindings for
some actors remain candidates. Height maps are not used. The legacy renderer
and its benchmark remain independent.

For the local frame 1001, five preview omni lights, 26 textures, 59 draws and
3,553 triangles render the bedroom; one candle-wax section is excluded for
alpha. The stone and parquet replacements cover 45 and 22 triangles with no
tangent fallback. The light choices use warm artistic colors and are **not**
evidence for the original light intensity. The local preview and all packets
remain under `build/` because they contain game-derived assets.

## Reproduce

Requires the existing fingerprinted frame-1001 snapshots, trace, SDK exports,
local Godot material PNGs, ImageMagick 7, and a Windows x86 D3D12 toolchain.
From the T3SDK root, prepare a local example calibration (the values are visual
choices and may be adjusted). Use a new path if this file already exists:

```sh
python3 -B - <<'PY'
import json
from pathlib import Path
scene = json.loads(Path('build/t3render-offline/bedroom-runtime-1001-light-candidates-v1.scene.json').read_text())
lights = []
for light in scene['lights']:
    if light['kind'] != 'omni':
        continue
    blue = light['id'].endswith(':D_57__1')
    lights.append({'id': light['id'], 'color_linear': [.25, .4, 1] if blue else [1, .7, .42],
                   'intensity_candela': 2 if blue else 5, 'radius_m': .12})
calibration = {'schema': 't3.scene.pbr-preview', 'version': 1,
               'ambient_linear': [.025, .025, .03], 'exposure': 1.5, 'lights': lights}
with Path('build/t3render-offline/bedroom-runtime-1001-pbr-calibration.json').open('x') as stream:
    json.dump(calibration, stream, indent=2)
    stream.write('\n')
PY

cmake -S t3render -B build/t3render-mingw -DT3RENDER_BUILD_SCENE_PBR_D3D12=ON
cmake --build build/t3render-mingw --target t3render_scene_pbr_d3d12
python3 -B t3render/tools/scene_pbr_packet.py \
  build/t3render-offline/bedroom-runtime-1001-warm-stage1.scene.json \
  --light-scene build/t3render-offline/bedroom-runtime-1001-light-candidates-v1.scene.json \
  --calibration build/t3render-offline/bedroom-runtime-1001-pbr-calibration.json \
  --state-trace build/t3render-captures/scene-bridge-submission-20261115-001/bedroom.jsonl \
  --offline-scene build/t3render-offline/SouthQuarter1_int1.lights.scene.json \
  --offline-actors build/assets/godot/SouthQuarter1_int1/SouthQuarter1_int1.actors.json \
  --game-dir "$T3_GAME_DIR" --pass 0 --stage 1 \
  --bedroom-remaster-materials build/assets/godot/remaster/garrett_home/materials \
  --output build/t3render-offline/bedroom-runtime-1001-pbr.scene.bin
```

Run `t3render_scene_pbr_d3d12 <packet> --output <new.ppm>` in a D3D12
environment. The packet, manifest and PPM use exclusive creation. See the
[Highball setup](README_synthetic_replay.md#repeat-on-highball); TDS does not
need to be running.

## Verification

`python3 -B -m unittest discover -s t3render/tests -p test_scene_pbr_packet.py`
tests semantic selection, per-channel bindings, opacity exclusions, matching
frame/camera, calibration bounds, and the independent C++ packet parser.

For GPU verification, create a new fixture with
`python3 -B t3render/tools/scene_pbr_fixture.py create <new.bin>`, render it
with the PBR executable, then use `scene_pbr_fixture.py verify <new.ppm>`.
Repeat with `--half-unit`, `--extreme` and `--srgb-filter` on both create and
verify. All four fixtures passed on the local Highball/D3D12 path: they check
sRGB filtering, roughness, AO, normal direction, inverse
square distance in metres, HDR values above one and finite output at extreme
energy. The extracted shared D3D12 helpers were also checked by rendering the
existing legacy shadow packet again and comparing its PPM byte for byte.
