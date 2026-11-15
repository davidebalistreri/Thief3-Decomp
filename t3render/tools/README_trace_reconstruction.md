# Bedroom DX12 partial reconstruction

The [3377 result](../benchmarks/d3d12/2026-11-14-tds-bedroom-reconstruction/result.json)
uses 35 ordered indexed draws and 3,932 triangles from a captured TDS frame.
Stable application-lock VB/IB observations, a narrow vertex declaration, four
`dp4` position instructions and sampled `c1`–`c4` matching the observed
WORLD × VIEW × PROJECTION select the supported subset. The diagnostic packet
expands the observed indices and applies WORLD to positions on the CPU. All
draws must use the same observed camera/viewport and compatible opaque depth,
color-write and CCW-culling states. Unsupported draws are omitted and counted.

The DX12 result is a **partial reconstruction**, not a faithful frame replay:
flat diagnostic colors and synthetic directional shading replace the material
pipeline; textures, original pixel shaders, lighting, blending, postprocessing,
depth history, other cameras and render targets are absent. It starts with a new
D32_FLOAT depth surface, applies LESS depth and approximates CCW culling. The
game's depth format, exact winding and pixel-center equivalence remain unproved.
The D3D8 image is captured before the corresponding Present, not after display
composition. Draw bytes and shader candidates remain observations rather than
proof of GPU inputs.

For frame 3377 the [manifest](../benchmarks/d3d12/2026-11-14-tds-bedroom-reconstruction/result.json)
records the hashes and observed differences. The bed, floor, doorway and
viewpoint are recognizable in both images, but the doorway's distant scene,
candles, HUD, textures, shadows and lighting differ or are missing. No pixel
parity is claimed. The comparison PNG, game-derived packet, trace and images
remain local and are not committed.

From `t3sdk`, build the existing `t3render_d3d12_replay_up` target and create a
packet in a new path:

```sh
python3 -B t3render/tools/trace_to_reconstruction.py \
  build/t3render-captures/bedroom-provenance-20261114-001/bedroom.jsonl \
  --frame 3377 --camera-op 8 \
  --output build/t3render-reconstruction/bedroom-3377-cull.txt
```

The packet and its `.manifest.json` are created exclusively; neither overwrites
an existing file. Run `build/t3render-mingw/t3render_d3d12_replay_up.exe` in a
compatible D3D12 environment with the packet and `--output` pointing to a new
PPM. For Highball, use the Wine environment described in
[the synthetic replay guide](README_synthetic_replay.md) and Windows `Z:` paths.
The output is labelled `DIAGNOSTIC`. For the recorded run the local comparison
is `build/t3render-reconstruction/comparison-3377-cull.png` (D3D8 left, DX12
right), built by placing the synchronized PPM and DX12 PPM side by side.

Frame 3377 is the frozen diagnostic oracle. Verify its local trace, D3D8 image,
DX12 packet, DX12 image and comparison against the committed manifests with
`python3 -B t3render/tools/verify_bedroom_golden.py`. This also checks the
recorded Present outcome and the camera and buffer observations for the bed and
BSP draw candidates. The source files remain local and are not a runtime scene
snapshot or proof of GPU inputs.

An alternative [offline asset packet](offline_mesh_reconstruction.py) combines
explicit actor and BSP candidates with the same sampled camera. It reads
`t3mesh.py` and `t3bsp.py` assets and requires unique matching section signatures;
its sidecar identifies it as offline rather than a runtime snapshot. This is
separate from the trace-sourced image above and does not establish a Scene IR.
