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
and wrap addressing. It currently assumes UV0 without flipping, D3D clip depth
`0..1`, the same half-pixel adjustment as the flat preview, back-face culling
and D32 LESS. It draws selected submissions in their SceneIR order. Textures
and geometry are asset-backed matches, not GPU readbacks or proof of every
original shader binding. Alpha, blending, fog, light, the rest of the visible
scene and HUD are absent. This is not D3D8 parity.

The frame-1001 capture produces 18 submissions, 450 triangles and four diffuse
textures (wall, floor, bed wood and fabric). The tested local Highball/D3D12
render shows recognizable textured surfaces aligned with the frame camera, but
is much brighter than D3D8 because it is unlit. The captured shadows remain
unavailable after Reset; the separate frame 2722's byte evidence is not
attributed to frame 1001. No game was launched for this render.

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
