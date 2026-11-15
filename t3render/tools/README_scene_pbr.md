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
tangent basis are assumptions. Metallic materials, GI, emissive
lighting, volumetrics, reflections and temporal effects are absent. The
render is a partial runtime-selected scene, and offline asset bindings for
some actors remain candidates. Height maps are not used. The legacy renderer
and its benchmark remain independent.

## Point-light shadow maps

`--shadows` opts into a separate shadow-map path in packet version 2. Each
selected point light renders six radial-distance faces into an R32F cubemap
array with a shared depth buffer. The PBR pass compares 25 point samples per
light. Its sampling cone uses the calibrated source radius and an adjustable
softness multiplier; this is an area-light approximation, not exact physical
penumbra reconstruction. A receiver-plane approximation from interpolated
vertex normals and slope- and texel-scaled bias reduce self-shadowing, but
cannot eliminate all acne or light leaks.

Caster geometry comes from the selected, opaque same-frame sections only.
For mesh instances, the fingerprinted offline actor export supplies the cast
flag and mesh identity; a light's owner and attached parent are excluded for
that light. All selected opaque casters are rendered two-sided. Cast flags and
ownership are **offline candidates**, not observed runtime shadow state.
Off-screen objects and sections omitted from the capture, non-opaque
textures, and dynamic shadow-state changes can therefore produce missing or
incorrect shadows. The previous unshadowed version-1 packet remains supported.

`--shadows --offline-shadow-casters` optionally adds static caster candidates
from the fingerprinted offline SceneIR and actor export. Packet version 3
stores their positions separately: they contribute only to shadow maps, not the
visible geometry pass. BSP sections and unattached `StaticMeshActor` sections
with an enabled offline cast flag are considered when their world-space bounds
intersect a light's shadow range. Sections already submitted in the selected
scope are not duplicated; hidden, missing-material and non-opaque diffuse
sections are omitted. If an instance was observed in the scoped frame, its
runtime transform is used for its unsubmitted sections; otherwise its offline
transform is used. A manifest records the selection, exclusions and light masks.

These additions are **offline candidates**, not an observation of visibility,
runtime transforms or shadow flags. Save-game changes and unresolved
attachments can make them inaccurate or incomplete. Bounds are a coarse range
filter, not an occlusion or room-membership test. Both the candidate count and
the total GPU shadow work are bounded; exceeding a limit rejects the packet.
The default version-1 and version-2 packet bytes are unchanged.

Defaults are 512² per face, near 0.05 m, far 15 m, bias 0.015 m and softness
0.5. `--shadow-map-size`, `--shadow-near-m`, `--shadow-far-m`,
`--shadow-bias-m` and `--shadow-softness` change them; softness 0 gives a hard
comparison. Near clipping may omit nearby blockers; receivers past the far
distance are unshadowed. Sizes are powers
of two from 64 to 512, with at most 65,536 shadow draw calls and six million
triangle submissions. Six faces and 25 samples per light make this an offline
preview, not a real-time performance claim.

On the local frame-1001 five-light preview, 58 selected draws (3,445
triangles) cast for each light. Shadow maps change 651,125 pixels compared
with the same unshadowed PBR image, with mean absolute RGB difference 7.012
on a 0–255 scale. This measures a visual change, not parity with TDS. The
version-1 PBR image remains byte-identical when shadows are not enabled.

For the same frame, the optional offline selection adds 213 sections and 8,911
triangles, with 6,066 shadow draws and 272,352 shadow triangle submissions in
total. Compared with the scoped-only shadow render, 2,601 pixels change and
mean absolute RGB difference is 0.002 on a 0–255 scale. This is a measurement
of the preview, not proof of runtime shadow correctness.

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

To build the shadowed alternative, append `--shadows` and choose a different
output path; all other arguments and the calibration stay the same.
Add `--offline-shadow-casters` as well to opt into offline candidates.

Run `t3render_scene_pbr_d3d12 <packet> --output <new.ppm>` in a D3D12
environment. The packet, manifest and PPM use exclusive creation. See the
[Highball setup](README_synthetic_replay.md#repeat-on-highball); TDS does not
need to be running.

## Verification

`python3 -B -m unittest discover -s t3render/tests -p test_scene_pbr_packet.py`
tests semantic selection, per-channel bindings, opacity exclusions, matching
frame/camera, calibration bounds, and the independent C++ packet parser.
`test_scene_pbr_offline_casters.py` additionally checks offline provenance,
selection, deduplication, transforms, opacity and parser/work limits.

For GPU verification, create a new fixture with
`python3 -B t3render/tools/scene_pbr_fixture.py create <new.bin>`, render it
with the PBR executable, then use `scene_pbr_fixture.py verify <new.ppm>`.
Repeat with `--half-unit`, `--extreme` and `--srgb-filter` on both create and
verify. All four fixtures passed on the local Highball/D3D12 path: they check
sRGB filtering, roughness, AO, normal direction, inverse
square distance in metres, HDR values above one and finite output at extreme
energy. The extracted shared D3D12 helpers were also checked by rendering the
existing legacy shadow packet again and comparing its PPM byte for byte.

For shadow-map verification, `scene_pbr_shadow_fixture.py create <new.bin>`
creates a synthetic receiver and occluder; use `--baseline` for the unshadowed
input. Render both with the PBR executable, then run
`scene_pbr_shadow_fixture.py verify <shadow.ppm> --reference <base.ppm>`.
Repeat with `--face 0` through `--face 6` (6 is a cubemap seam),
`--asymmetric`, and `--two-lights`. `--hard` uses `--soft-reference <soft.ppm>`
at verification; `--short-range` checks the far-distance fallback and
`--behind` checks that a blocker behind the receiver does not shadow it. On the
local Highball/D3D12 path, these fixtures passed for the six faces, seam,
asymmetric occluders, independent light layers, soft versus hard penumbrae,
range fallback and an off-axis receiver self-shadow case.
`--offline-caster` produces a version-3 fixture with a shadow-only blocker;
add `--baseline` to omit that blocker while keeping the visible geometry
identical. The local GPU test passed with the blocker shadowing the receiver
and leaving an unoccluded sample unchanged.
