# Runtime bridge checkpoint: frame 2722

## Scoped submission checkpoint: frame 1001

The later frame 1001 directly brackets the per-section virtual draw call at
`0x1092fc73`. In pass 0/stage 1, the scoped bed draws are 8/9 and the scoped
VBSP runtime block 7 draws are 5/6; the latter uniquely matches offline GMP
block 5. The synchronized frame has 338 completed scopes and no dropped or
pending scopes. The bed's runtime actor, resource object and mesh node are
rechecked at entry and return. Draw arguments, metadata, transforms, materials
and camera are compared with the offline fixture and the probe observation.

The frame-1001 buffer shadows for these draws are unavailable after an earlier
Reset, so draw ownership and argument matching do not establish GPU bytes. The
optional reference below independently verifies positions, UV and indices in
the separate frame 2722; its shadow evidence does not acquire frame-1001
provenance or direct ownership. The emitted resource key uses the runtime name
and the **offline** map/bundle fingerprints, after matching the runtime map
name and a unique offline resource. No process address is a persistent ID.

```sh
python3 -B t3render/tools/verify_bridge_submission.py \
  build/t3render-captures/scene-bridge-submission-20261115-001/bedroom.jsonl \
  --scene build/t3render-offline/SouthQuarter1_int1.scene.json \
  --frame 1001 --pass 0 \
  --reference-trace build/t3render-captures/scene-bridge-runtime-20261115-001/bedroom.jsonl \
  --reference-frame 2722 --reference-draw 8 --reference-bsp 5
```

The command returns success only when both bed sections and two sections of
one BSP have matched scopes in the same stage. The reference comparison is
separately marked `same_frame=false`; all captures remain local under `build/`.
For a fresh capture, `run_tds_bedroom_batch.py --scene-bridge
--submission-scope` opts into the additional, build-verified callsite hook.
The per-call ticket is a transient scope generation, not a resource ID.

## Original asset-backed checkpoint

The read-only probe observes one `0x10926800` list entry linked to
`StaticMeshActor__1520`, its local mesh property, mesh node, transform and
material sections, plus bounded VBSP array members. Camera and objects are
sampled in the same verified Present interval. Runtime addresses stay in the
local trace and are never serialized into SceneIR.

The comparator searches all offline BSP blocks against all observed candidates.
It requires a mutual unique metadata match, never equal scanner/array indices;
missing or ambiguous evidence remains unknown. A match does not prove runtime
payload identity. The offline scanner is heuristic and SceneIR does not retain
the original BSP section vertex ranges. The separate lock-shadow verifier
compares bytes for observed draws, not a GPU readback or direct ownership.

From `t3sdk`, with the local game assets and captured frame:

```sh
python3 -B t3render/tools/compare_bridge_fixture.py \
  build/t3render-captures/scene-bridge-runtime-20261115-001/bedroom.jsonl \
  --scene build/t3render-offline/SouthQuarter1_int1.scene.json \
  --frame 2722 --draw 8 --pass 0 --bsp 5 --bsp 7 --bsp 19

python3 -B t3render/tools/correlate_trace_bsp.py \
  build/t3render-captures/scene-bridge-runtime-20261115-001/bedroom.jsonl \
  "$T3RENDER_GAME_DIR/Content/T3/Maps/SouthQuarter1_int1.gmp" \
  --frame 2722 --reference-op 8 --block 5 --block 7 --block 19
```

## Asset-backed runtime snapshot

`build_bridge_runtime_snapshot.py` requires a complete synchronized probe
window, actor membership/backlink, a consistent mesh property and linked node,
matching metadata and a unique offline candidate. For each selected mesh or BSP
section, it also checks the captured position/UV/index data against the asset
in the same Present interval and thread, with the same last preceding probe
observation. It rejects missing or ambiguous evidence. That temporal association
does not prove that draws occurred in the probe's pass or were submitted by the
observed objects.

The output has `source.kind=runtime`: its **selection, transform, camera and
material names** were observed by the probe in that frame. Its geometry and
qualified asset IDs come from the offline fixture; the runtime qualified
resource ID is unknown. Only selected sections are verified against lock
shadows. The `BF` section of the bed remains asset-backed and unverified;
skin and lights are unknown. Its sidecar and `source.limitations` record these
distinctions, the trace/fixture fingerprints, draw IDs and the runtime ↔ offline
BSP indices. This is a partial asset-backed snapshot, not a GPU readback, a
complete scene or proof of direct draw ownership.

Use a new output filename; the snapshot and sidecar are written exclusively:

```sh
python3 -B t3render/tools/build_bridge_runtime_snapshot.py \
  build/t3render-captures/scene-bridge-runtime-20261115-001/bedroom.jsonl \
  --scene build/t3render-offline/SouthQuarter1_int1.scene.json \
  --frame 2722 --draw 8 --pass 0 --bsp 5 --bsp 7 --bsp 19 \
  --mesh-section 1 --mesh-section 2 \
  --output build/t3render-offline/bedroom-runtime-2722.scene.json

python3 -B t3render/tools/scene_ir_to_d3d12_packet.py \
  build/t3render-offline/bedroom-runtime-2722.scene.json \
  --instance actor:SouthQuarter1_int1:ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba:StaticMeshActor__1520 \
  --bsp 5 --bsp 7 --bsp 19 --exclude-material BF \
  --output build/t3render-offline/bedroom-runtime-2722.packet.txt
```

The diagnostic DX12 consumer accepts runtime and offline scenes, but does not
independently verify the producer's provenance. A separate camera never grants
runtime provenance or synchronization to an offline scene. Render the packet
using the existing x86 executable and Highball instructions in
[README_scene_ir_preview.md](README_scene_ir_preview.md); this does not launch
TDS. The frame-2722 preview uses 340 triangles and has no textures or lighting.
Its view/projection match draw 8; the probe camera depth range `0..1` differs
from the draw viewport `0..0.5`. Clip depth and culling remain assumptions.

All capture-derived scenes, packets and images remain local under `build/`.
See [the research record](../T3SCENEBRIDGE_RESEARCH.md#probe-runtime-e-snapshot-bedroom-frame-2722)
for the evidence and remaining limitations.
