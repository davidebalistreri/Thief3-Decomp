# Textured SceneIR D3D12 preview

`t3render_scene_d3d12` is a standalone SceneIR consumer, independent of the
legacy D3D8 replay. `scene_ir_to_textured_packet.py` turns a synchronized scoped
SceneIR v1 into a bounded local packet with expanded world-space vertices, UV0,
camera and original diffuse texture mip chains. It resolves each section's
semantic material via the T3SDK MatLib stage-0 reference, looking in the level
bundle then the kernel bundle. Referenced bundle SHA-256 values must match the scene;
ambiguous or missing resources and unsupported formats fail closed. The packet
and its manifest contain game-derived content and must stay local.

The renderer samples decoded RGBA8 texture mips with linear min/mag/mip filters
and wrap addressing. It assumes UV0 without flipping, D3D clip depth `0..1`,
and the same half-pixel adjustment as the flat preview. It draws selected
submissions in their SceneIR order, with per-draw alpha test, blend, depth,
color-write mask and culling. Without an explicit trace these states default
to opaque, back-face culling and D32 LESS. Texture and geometry are asset-backed
matches, not GPU readbacks or proof of the original shader bindings. The pixel
shader uses sampled texture alpha as an approximation; by default fog,
lighting, missing objects and HUD are absent. This is not D3D8 parity.

The frame-1001 capture produces 18 submissions, 450 triangles and four diffuse
textures (wall, floor, bed wood and fabric). The tested local Highball/D3D12
render shows recognizable textured surfaces aligned with the frame camera, but
is much brighter than D3D8 because it is unlit. The captured shadows remain
unavailable after Reset; the separate frame 2722's byte evidence is not
attributed to frame 1001. No game was launched for this render.

The optional [expanded snapshot](README_bridge_runtime_checkpoint.md#additional-submitted-mesh-candidates-frame-1001)
adds submitted actors whose offline asset bindings are explicitly candidates.
Its local stage-1 preview has 51 submissions, 2,997 triangles and 11 textures;
it includes a candle holder and a crate near the bed. The renderer preserves
the candidate limitations and actor IDs in its packet manifest. The objects
and textures are not runtime mesh-payload or material-binding proof.

## Ambient candidate

With `--ambient-candidate-stage 1`, the exporter requires a state trace and a
matching offline fixture. It revalidates the scoped snapshot, checks the
frame-wide constant and pre-Reset program budgets, then requires each selected
draw to have sampled finite register 18 and an exact, unique match to the
fingerprinted kernel's `Ambient11_Light_Vertex`. It uses the sampled RGB as a
constant ambient output. A following stage-4 snapshot can modulate that output
with its offline diffuse texture and the captured blend/depth state.

The pixel shader binding is not captured, and the vertex program identity is a
**pre-Reset hypothesis**, not proof of the active GPU program. The mode is an
explicit approximation, not a replay of either shader. Texture alpha still
comes from the offline asset; unselected draws, direct lights, stencil and
other effects remain missing. No candidate is added to the SceneIR light list.
The packet manifest records the sampled per-draw colors and these limitations.

For the local fingerprinted kernel, D3DDisassemble of the ambient vertex asset
(`sha256:10fb13338c9197ebaa83a0bef3e6bc575d11534645beb60d8ef0d8672934a75f`)
shows register 18 sent to vertex color, with a constant-one secondary coordinate
when register 0.z is one. The separately named ambient pixel asset multiplies
those inputs for RGB and samples texture alpha. Its presence in the bundle
does not establish its runtime binding. Original shader bytes remain local.

In frame 1001 the expanded stage-1 snapshot has 51 matching ambient candidates;
their register-18 RGB is approximately `(0.01464, 0.02133, 0.03137)`. Combining
it with the separately revalidated stage 4 produced 102 draws on Highball/DX12.
The room is very dark until direct light contributions are reconstructed. A
synthetic GPU fixture checks ambient output and diffuse modulation, while the
existing texture, depth/blend and sampler fixtures still pass.

```sh
python3 -B t3render/tools/build_scoped_scene_snapshot.py \
  build/t3render-captures/scene-bridge-submission-20261115-001/bedroom.jsonl \
  --scene build/t3render-offline/SouthQuarter1_int1.scene.json \
  --frame 1001 --pass 0 --stage 4 --anchor-draw 302 \
  --include-asset-candidates \
  --output build/t3render-offline/bedroom-runtime-1001-candidates-stage4.scene.json

python3 -B t3render/tools/scene_ir_to_textured_packet.py \
  build/t3render-offline/bedroom-runtime-1001-candidates.scene.json \
  --additional-scene build/t3render-offline/bedroom-runtime-1001-candidates-stage4.scene.json \
  --game-dir "$T3_GAME_DIR" --pass 0 --stage 1 \
  --state-trace build/t3render-captures/scene-bridge-submission-20261115-001/bedroom.jsonl \
  --offline-scene build/t3render-offline/SouthQuarter1_int1.scene.json \
  --ambient-candidate-stage 1 \
  --output build/t3render-offline/bedroom-runtime-1001-ambient.scene.bin
```

The stage-1 snapshot is generated as described in the runtime checkpoint. All
capture-derived outputs remain local. The optional mode writes packet version
4; versions 1–3 remain readable and the default exporter still writes v3.

## Direct-light candidates

`--direct-candidate-stage 3` adds a diagnostic point-light approximation to
the ambient and diffuse passes. It requires the same-frame state trace,
the corrected offline SceneIR fixture, its fingerprint-matched actor export,
and a matching kernel bundle. Each stage-3 draw must have a unique pre-Reset
omni/spot program hypothesis, sampled constants and a unique offline light
position within 0.01 T3 units. Ambiguous or missing inputs fail closed.

By default only omni candidates are shaded: register 20 supplies position; register 21.y
is used **as if** it were inverse radius. Color and the approximate `t3map`
energy are offline values; Lambert shading, squared linear falloff and the
optional `--direct-preview-gain` are renderer choices, not measured legacy
behavior. Normals use the inverse-transpose instance transform. The gain is
explicit and defaults to 1. Spot candidates contribute zero unless explicitly
enabled; no spot is silently treated as an omni light.

This does not observe a runtime light object, pixel shader, color or energy.
It does not implement stencil shadows, normal maps, emissions, missing scene
objects or HUD. The preview does not write candidates into SceneIR's light
list. The packet manifest labels every point or spot and records the offline
light ID, program hypothesis and parameters. The baseline point-light path
writes packet version 5, while versions 1–4 remain readable.

Using the local frame-1001 capture and the corrected offline fixture, the
expanded stage-1/3/4 packet contains 222 draws: 51 ambient, 69 approximate
point-light contributions, 51 skipped spot contributions and 51 material
modulation draws (four have other vertex-program hypotheses). Five offline
omni lights have candidate positional matches. The local
Highball/DX12 image lights the wall and candle holder, but is not legacy parity.
An independent GPU fixture checks point lighting, texture modulation and the
skipped-spot path; the earlier GPU fixtures still pass.

After generating the corrected offline fixture as described in
[the importer instructions](README_scene_ir_import.md), build the three scoped
snapshots using `--include-asset-candidates` and the following stage/anchor
pairs: `1/8`, `3/78`, `4/302`. Then, with those new local files:

```sh
python3 -B t3render/tools/scene_ir_to_textured_packet.py \
  build/t3render-offline/bedroom-runtime-1001-lights-stage1.scene.json \
  --additional-scene build/t3render-offline/bedroom-runtime-1001-lights-stage3.scene.json \
  --additional-scene build/t3render-offline/bedroom-runtime-1001-lights-stage4.scene.json \
  --game-dir "$T3_GAME_DIR" --pass 0 --stage 1 \
  --state-trace build/t3render-captures/scene-bridge-submission-20261115-001/bedroom.jsonl \
  --offline-scene build/t3render-offline/SouthQuarter1_int1.lights.scene.json \
  --offline-actors build/assets/godot/SouthQuarter1_int1/SouthQuarter1_int1.actors.json \
  --ambient-candidate-stage 1 --direct-candidate-stage 3 \
  --output build/t3render-offline/bedroom-runtime-1001-direct.scene.bin
```

With `--spot-candidates`, the exporter also checks each spot's sampled register
19 against the offline actor rotation and checks the sampled projector rows
22, 23 and 25 against its position and axis. It uses those rows with a soft
circular mask and approximate axial falloff. The four spot axes in frame 1001
match the corresponding offline rotations within 0.0001 per component. A
separate D3DDisassemble of the local `SpotLightNormal11_Light_Vertex` asset
(`sha256:d4f34f1aeda5f7747cfec11166536e73e183f7a882ad9de74478490a08dae95f`)
shows those registers feeding the spot projection; this does not establish the
post-Reset GPU program or the projected texture. The circular mask is a
preview approximation, not the measured legacy cone.

Optionally append `--spot-candidates --spot-projector-candidate genwindowint_p`
to the command above and use a new output path. The sampler and the candidate
texture's dimensions, format and mip count must match the observed stage-3
metadata. The offline bundle has five matching entries, including two
byte-identical `genwindowint_p` entries: the texture name is therefore a
**hypothesis**, not proof of GPU identity. Same-named entries with different
contents are rejected. The projected texture uses linear filtering, a black
border and mip zero, as in the checked stage-3 sampler metadata. Colors,
energy, normal mapping, shadows and the original pixel shader remain unproven.

The local 222-draw preview then includes 51 approximate spot contributions.
Compared with the frame-1001 screenshot, whole-image mean absolute RGB error
was `(8.23, 6.82, 6.56)` for the circular mask and `(7.55, 6.45, 5.78)` for
the named texture hypothesis, on a 0–255 scale. This comparison includes dark
background, missing geometry and HUD and is not proof of visual parity or
texture identity. The synthetic GPU fixtures exercise both projector paths;
earlier packet fixtures also pass. Circular and textured modes write packet
versions 6 and 7 respectively; versions 1–5 remain readable.

The fixture, packets and rendered images contain game-derived data and stay
local under `build/`.

The optional `--state-trace <trace> --offline-scene <fixture>` revalidates the
trace fingerprint and rebuilds the bounded bed-and-BSP scoped snapshot before
mapping its draw states to the renderer's neutral vocabulary. This currently
requires an exact snapshot produced by `build_scoped_scene_snapshot.py`,
including its explicit candidate limitations when enabled. It fails on unsupported or
unobserved states. For all 18 frame-1001 stage-1 draws it observes disabled alpha
test and blending, depth LESS/write, CCW culling, and no fixed-function fog or lighting. The stage-3
draws use additive blending, stage 4 uses two-times modulation, and stage 6 uses
source-alpha blending; these observations do not reconstruct the missing legacy
pixel shaders or light contributions. A stage-3 snapshot and packet were also
revalidated locally: 59 scoped submissions and 1,684 asset-backed triangles.
It is evidence only: this standalone renderer clears depth, so stage 3's
`LESS_EQUAL` depth test cannot reproduce occlusion without the preceding depth history;
the submissions do not yet reproduce lighting. The selected stage-3 draws
also have stencil testing enabled, which this renderer does not implement. No
runtime addresses or D3D state enums are added to SceneIR.

For diagnostics, `--additional-scene` composes separately revalidated stages
from the same frame, pass, camera and asset fingerprints into one packet, in
captured operation order with a shared depth buffer. It rejects conflicting
resources, repeated draws and intervening color/depth clears. It requires
`--state-trace` and `--offline-scene`; each snapshot is revalidated before
its states are paired, and each source's limitations are retained in the
manifest. Omitted draws and stencil operations are not recreated,
so the depth and visibility history remains partial. Without either lighting
preview option, stage 3 uses an unlit diffuse shader, not the legacy shader.

With the existing frame-1001 stage-1 and stage-3 snapshots, composition
produced 77 draws and 2,134 triangles on the standalone Highball/D3D12 path.
The additional diffuse passes overexpose the image; this demonstrates packet
composition and shared depth, not legacy lighting or visual parity. The
rendered packet, manifest and image stay local under `build/`.

The optional `--sampler-candidate-stage 2` requires the trace and offline
fixture above. It accepts only linear wrap filtering, zero max mip and no
texture transform, then compares the observed texture dimensions, format and
mip count with each offline asset. A metadata match is an explicit hypothesis,
not proof of texture identity or the shader's UV routing. A failed getter or
unbound stage stays a separately labeled bias-0 fallback; unsupported states
or mismatched metadata fail. In frame 1001, 17 draws use the observed bias -1
under this hypothesis and draw 9 has the unbound fallback. With otherwise
identical v3 packets and renderer, this changes 638,471 of 786,432 pixels, with
mean absolute channel difference 4.10/255. This A/B measurement is not a D3D8
parity claim; lighting and shader behavior remain missing.

## Reproduce

Build with an existing Windows x86 CMake toolchain:

```sh
cmake -S t3render -B build/t3render-mingw -DT3RENDER_BUILD_SCENE_D3D12=ON
cmake --build build/t3render-mingw --target t3render_scene_d3d12
python3 -B t3render/tools/scene_ir_to_textured_packet.py \
  build/t3render-offline/bedroom-runtime-1001-v1.scene.json \
  --game-dir "$T3_GAME_DIR" --pass 0 --stage 1 \
  --output build/t3render-offline/bedroom-runtime-1001-textured.scene.bin
```

The snapshot is produced as described in the
[runtime checkpoint](README_bridge_runtime_checkpoint.md). The packet and
manifest are written exclusively. On Windows, pass the packet and a new PPM
filename to the built executable:

To use the revalidated states instead of preview defaults, append:

```sh
--state-trace build/t3render-captures/scene-bridge-submission-20261115-001/bedroom.jsonl \
--offline-scene build/t3render-offline/SouthQuarter1_int1.scene.json
```

Append `--sampler-candidate-stage 2` to apply the separate sampler hypothesis.

To compose a second stage for the depth diagnostic, add the state options above
and append:

```sh
--additional-scene build/t3render-offline/bedroom-runtime-1001-stage3.scene.json
```

```powershell
.\build\t3render-mingw\t3render_scene_d3d12.exe `
  .\build\t3render-offline\bedroom-runtime-1001-textured.scene.bin `
  --output .\build\t3render-offline\bedroom-runtime-1001-textured.ppm
```

For Highball, use the Wine/D3D12 environment in
[the synthetic replay instructions](README_synthetic_replay.md#repeat-on-highball),
and pass the packet and output as Wine-visible paths. The executable rejects
malformed or oversized packets and returns failure when no pixels differ from
the clear color, including a silently failed driver pipeline.

## Synthetic GPU check

Create a new fixture, render it with the same executable, then verify the PPM:

```sh
python3 -B t3render/tools/scene_texture_fixture.py create /absolute/new-fixture.bin
# Run t3render_scene_d3d12 <fixture.bin> --output <new-image.ppm>
python3 -B t3render/tools/scene_texture_fixture.py verify /absolute/new-image.ppm
python3 -B -m unittest discover -s t3render/tests -p test_scene_textured_packet.py
```

The six checked pixels independently exercise texture upload and orientation,
UV wrapping, bilinear sampling, a smaller mip, and nearer-over-far depth on the
GPU. They passed on the local Highball setup. They do not prove the game's
sampler state, alpha mode, lighting or visual parity. On Wine's DXBC-to-SPIR-V
path, placing the varying before `SV_Position` keeps shader locations matched;
the fixture detects a failure that otherwise can yield an empty render.

`scene_pipeline_fixture.py create <new.bin>` and `verify <image.ppm>` exercise
alpha test, source-alpha/additive/multiply blending, color masking, depth
comparison and writes, disabled depth, and front/back/disabled culling. Its 11
GPU pixel checks passed locally on Highball. `scene_sampler_fixture.py create
<new.bin>` and `verify <image.ppm>` check biases -1, 0 and +1 on distinct mips;
all three pixels passed on the same GPU environment. `scene_ambient_fixture.py`
checks constant ambient and subsequent diffuse modulation, plus the ordinary
texture path. Existing versions 1–3 remain readable.
