# Runtime bridge checkpoint: frame 2722

## Additional submitted mesh candidates: frame 1001

The optional `--include-asset-candidates` path extends the stage-1 snapshot
without another game run. It matches a complete observed list entry to a
balanced same-thread submission, requires a unique offline actor name, compares
its runtime and offline transforms within 0.001 T3 units of translation and
0.000001 for the other matrix entries, and checks the submitted section's
index range and draw counts. The serialized transform is the runtime one.

These checks do **not** read the additional actors' live mesh properties,
mesh-node names, materials or vertex/index contents. The offline mesh and
material bindings remain candidates, identified by actor ID in both the
snapshot limitations and its manifest; they are not equivalent to the bed's
stronger resource proof. Ambiguous actors, unstable scopes, transform
mismatches and incompatible draw metadata are excluded. The original strict
bed-and-BSP path remains the default.

```sh
python3 -B t3render/tools/build_scoped_scene_snapshot.py \
  build/t3render-captures/scene-bridge-submission-20261115-001/bedroom.jsonl \
  --scene build/t3render-offline/SouthQuarter1_int1.scene.json \
  --frame 1001 --pass 0 --stage 1 --anchor-draw 8 \
  --include-asset-candidates \
  --output build/t3render-offline/bedroom-runtime-1001-candidates.scene.json
```

This local capture yields the bed, 26 additional candidate actors, nine BSP
blocks and 51 submitted sections. It includes a candidate candle holder and
nearby crate, but does not establish their runtime mesh or light bindings. The
same-frame textured DX12 preview has 2,997 triangles and 11 offline textures;
it is unlit and does not establish parity. The packet exporter revalidates the
expanded snapshot before attaching captured draw states. The generated scene,
sidecar, packet and images remain local under `build/`.

### Quantized rotator candidates

`--allow-quantized-rotators` is a separate opt-in for scoped actors whose
offline and runtime matrices differ only under a two-bit rotator truncation
hypothesis. It requires `--include-asset-candidates`: the offline matrix must
match the observed owner location and unquantized owner rotation, while the
runtime matrix must independently match that location and the rotation with
its low two bits cleared. Both checks use the existing 0.001 T3-unit
translation and 0.000001 matrix tolerances. Uniform signed scale is retained;
missing owner fields, incompatible scale or changed position fail closed.
The serialized transform remains the observed runtime matrix. The hypothesis
does not prove the engine's rotation implementation or the runtime mesh binding.

In frame 1001 this identifies `D_548__0` (the archery target) and `D_77__7`
(an arrow) in addition to the previous candidates. Their largest runtime versus
quantized matrix errors are approximately `4.1e-8` and `2.1e-8`; their
unquantized offline matrices differ by up to `8.8e-5` and `9.5e-5`. Stage
1/3/4/6 snapshots contain 54/124/54/54 draws respectively. The training dummy
`D_1997__0` is still omitted because its offline fixture excludes an unresolved
self-attachment; it is not silently reconstructed here. Snapshots and manifests
list the candidate actors and retain the offline provenance limitation.

Generate the four snapshots with the existing corrected offline fixture,
frame 1001, `--include-asset-candidates --allow-quantized-rotators`, and
stage/anchor pairs `1/8`, `3/78`, `4/302`, `6/362`. The normal-mapped four-stage
preview contains 286 draws and 18,672 triangles. Its 1024×768 comparison against
the synchronized screenshot changes whole-image mean absolute RGB error from
`(7.475, 5.966, 4.338)` to `(7.457, 5.960, 4.335)`; in rectangle
`(115,215)`–`(196,390)` around the target it changes from `(12.191, 8.473,
5.256)` to `(11.177, 8.139, 5.049)`. The target remains too dark and these
metrics do not validate the offline resource or material binding.

## Light position candidates: frame 1001

The offline importer now places mesh-actor lights at `hp_lighttarget` plus
offset, rotated without `DrawScale`. Against a regenerated fixture, stage-3
bed draws yield four unique positional candidates: `Light__15` (draws 78/79),
`D_384__13` (115/116), `D_384__5` (134/135) and `D_384__0` (180/181).
For each pair, sampled vertex constant register 20 agrees with an offline light
position within 0.01 T3 units; the largest observed distance is 0.00195 units.
The pre-Reset shader program hypotheses also exactly match uniquely named omni
or spot vertex resources in the fingerprinted kernel bundle. Register 21's
coefficient is recorded without claiming its meaning or calibration.

These are **candidates**, not observed runtime light objects: program identities
are pre-Reset hypotheses, constants are not GPU readback, the match uses offline
positions, and no live light color, enabled state or radius was sampled. The
result is not inserted into the runtime SceneIR light list. It narrows a future
semantic light verification without reopening general renderer research.

With a newly regenerated offline fixture and a stage-3 scoped snapshot:

```sh
python3 -B t3render/tools/correlate_runtime_light_candidates.py \
  build/t3render-captures/scene-bridge-submission-20261115-001/bedroom.jsonl \
  --scene build/t3render-offline/bedroom-runtime-1001-lightstage.scene.json \
  --offline-scene build/t3render-offline/SouthQuarter1_int1.lights.scene.json \
  --kernel "$T3_GAME_DIR/Content/T3/Maps/Kernel_GFXALL.ibt" \
  --pass 0 --stage 3 \
  --output build/t3render-offline/bedroom-runtime-1001-light-candidates.json
```

The tool revalidates the snapshot and trace, checks frame-wide observation
budgets, and rejects ambiguous positions and duplicate shader matches. Output
stays local and contains no process addresses or shader bytes.

## Scoped SceneIR v1: frame 1001

The bridge v0 contract is frozen at the verified per-section `+0x24`
submission for static meshes and BSP. The local frame-1001 capture can now
produce a partial SceneIR v1 directly from the scoped proof:

```sh
python3 -B t3render/tools/build_scoped_scene_snapshot.py \
  build/t3render-captures/scene-bridge-submission-20261115-001/bedroom.jsonl \
  --scene build/t3render-offline/SouthQuarter1_int1.scene.json \
  --frame 1001 --pass 0 --stage 1 --anchor-draw 8 \
  --output build/t3render-offline/bedroom-runtime-1001-v1.scene.json
```

The producer revalidates the trace and proof, and selects only verified
submissions with the anchor's camera matrices, draw viewport and thread. The
result contains the runtime bed transform, nine BSP blocks and 18 scoped draws
in pass 0/stage 1. It contains no runtime lights: their observation remains
unimplemented. All geometry, material bindings and IDs are from the offline
fixture, matched to runtime names and metadata; per-draw lock-shadow status is
`unverified` in this capture. The separate frame 2722's byte comparisons do
not establish frame-1001 payload contents. Skin and near/far remain unknown.
The camera viewport comes from the selected draws (`0..0.5`), not the probe's
different depth range. The output and its manifest remain local under `build/`.

The existing SceneIR preview supports v1 and uses only submitted sections of
the selected pass/stage. When there is more than one scope, select it explicitly
with `--submission-pass` and `--submission-stage`. The frame-1001 scene above
produces a 450-triangle flat diagnostic packet with BSP indices 1, 2, 4, 5, 6,
7, 9, 11 and 19; it is not a textured or lit parity render. The producer remains
limited to the verified target mesh and observed BSP candidates; it is not yet
a general live scene bridge. The packet was parsed and rendered on the local
Highball/D3D12 environment without launching TDS. In the local image the bed,
floor and wall silhouettes are recognizable against the frame-1001 screenshot;
textures, lights, other objects and HUD are missing. No pixel parity is claimed.
See [SceneIR v1](../ir/README_scene_ir.md).

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
