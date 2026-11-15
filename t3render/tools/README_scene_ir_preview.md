# SceneIR DX12 preview

The separate [textured SceneIR D3D12 consumer](README_scene_ir_textures.md)
now renders the selected frame-1001 submissions with original diffuse textures.
The flat-color diagnostic path below remains useful for geometry inspection.

This diagnostic path takes geometry and transforms **only from SceneIR**. It
temporarily uses the existing `t3render_d3d12_replay_up` executable as a DX12
packet consumer; it does not extend the D3D8 replay. The offline example below
uses an independently sampled camera. The offline map export remains
`source.kind=offline`, with `frame_id=null` and `camera_synchronized=false`.
The attached camera records its runtime frame and trace fingerprint separately.
This combination is **not a synchronized runtime scene**.
For the asset-backed runtime-selected frame 2722, see the
[runtime checkpoint](README_bridge_runtime_checkpoint.md); provenance remains
the producer's assertion and the consumer carries its limitations forward.

The local `scene-ir-preview-2661.png` uses the 2661 draw-8 camera, the offline
`StaticMeshActor__1520 / INNbed1A1`, and BSP `source_index` 5, 7 and 19:
340 triangles after explicitly excluding the legacy hidden `BF` material.
`comparison-2661.png` places the captured pre-Present D3D8 backbuffer on the
left and that preview on the right. The bed and wall shapes are recognizable;
the rest of the scene is not reconstructed. The D3D8 image and camera come
from frame 2661, but the offline geometry is not proven to be the visible
runtime geometry for that frame.

The separate `scene-ir-preview-2661-nearby.png` adds three offline instances
selected by proximity, not runtime visibility: `D_384__5` (two-candle mesh),
`D_1224__21` (crate) and `StaticMeshActor__604` (beam). It contains 596
triangles. `comparison-2661-nearby.png` compares it with the same backbuffer.
Neither preview establishes the runtime identity or visibility of those
additional objects. All generated scenes, packets, screenshots and comparison
images contain game-derived data and stay local under `build/`.

The DX12 consumer uses diagnostic palette colors, synthetic shading, an empty
D32 depth surface with LESS, approximate CCW culling and a pixel-center
adjustment. It assumes clip depth `0..1` even though the offline SceneIR
convention is `unknown`. The frame-2661 draw viewport depth is `0..0.5`; the
probe's camera viewport depth was `0..1`. Textures, original shaders,
lighting, depth history, HUD, distant scene and unselected objects are absent.
No pixel parity or full geometric equivalence is claimed.

## Reproduce locally

From `t3sdk`, after generating the offline fixture and retaining the local
frame-2661 capture, use new output paths:

```sh
python3 -B t3render/tools/capture_to_camera_observation.py \
  build/t3render-captures/scene-bridge-bed-20261115-001/bedroom.jsonl \
  --frame 2661 --draw 8 --output build/t3render-offline/camera-frame-2661.json

python3 -B t3render/tools/scene_ir_attach_camera.py \
  build/t3render-offline/SouthQuarter1_int1.scene.json \
  --camera build/t3render-offline/camera-frame-2661.json \
  --output build/t3render-offline/SouthQuarter1_int1.camera-2661.scene.json

python3 -B t3render/tools/scene_ir_to_d3d12_packet.py \
  build/t3render-offline/SouthQuarter1_int1.camera-2661.scene.json \
  --instance actor:SouthQuarter1_int1:ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba:StaticMeshActor__1520 \
  --bsp 5 --bsp 7 --bsp 19 --exclude-material BF \
  --output build/t3render-offline/scene-ir-preview-2661.txt
```

The last command writes a packet and provenance manifest, both exclusively.
It can instead read an independent observation with `--camera`; an embedded
SceneIR camera is used when that option is absent. Select the nearby variant
explicitly by adding `--instance` for each of the three actor IDs and using a
different output name. Render a packet with
`build/t3render-mingw/t3render_d3d12_replay_up.exe <packet> --output <new.ppm>`
in a compatible D3D12 environment. The [Highball instructions](README_synthetic_replay.md)
describe the Wine environment; this does not launch TDS.

`python3 -B -m unittest discover -s t3render/tests -p test_scene_ir_to_d3d12_packet.py`
checks provenance, explicit selection, transforms, exclusion rules, malformed
input and compatibility with the existing DX12 packet parser using synthetic
data.
