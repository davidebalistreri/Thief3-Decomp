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
shader uses sampled texture alpha as an approximation; fog, lighting, missing
objects and HUD are absent. This is not D3D8 parity.

The frame-1001 capture produces 18 submissions, 450 triangles and four diffuse
textures (wall, floor, bed wood and fabric). The tested local Highball/D3D12
render shows recognizable textured surfaces aligned with the frame camera, but
is much brighter than D3D8 because it is unlit. The captured shadows remain
unavailable after Reset; the separate frame 2722's byte evidence is not
attributed to frame 1001. No game was launched for this render.

The optional `--state-trace <trace> --offline-scene <fixture>` revalidates the
trace fingerprint and rebuilds the bounded bed-and-BSP scoped snapshot before
mapping its draw states to the renderer's neutral vocabulary. This currently
requires the exact snapshot produced by `build_scoped_scene_snapshot.py` and
does not generalize to other actors. It fails on unsupported or
unobserved states. For all 18 frame-1001 stage-1 draws it observes disabled alpha
test and blending, depth LESS/write, CCW culling, and no fixed-function fog or lighting. The stage-3
draws use additive blending, stage 4 uses two-times modulation, and stage 6 uses
source-alpha blending; these observations do not reconstruct the missing legacy
pixel shaders or light contributions. A stage-3 snapshot and packet were also
revalidated locally: 59 scoped submissions and 1,684 asset-backed triangles.
It is evidence only: this standalone renderer clears depth, so stage 3's equal
depth test cannot reproduce that stage without the preceding depth history;
the submissions have not been composed as lighting. The adapter does not infer sampler-to-asset
identity: most stage-1 draws have a bound stage-2 texture with linear wrap and
mip bias -1, while the current preview uses bias 0, and one draw has no bound
texture. Treat this as a recorded discrepancy rather than a verified sampler
match. No runtime addresses or D3D state enums are added to SceneIR.

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
GPU pixel checks passed locally on Highball. Existing version-1 packets remain
readable; the exporter writes version 2 with API-neutral per-draw state.
