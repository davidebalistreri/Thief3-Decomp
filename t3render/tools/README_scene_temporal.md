# Offline temporal accumulation preview

The PBR renderer can export bounded scene-linear HDR and camera-relative
position buffers with `--temporal-output <new.bin>`. `--jitter <x> <y>` adds a
subpixel offset in the range -0.5 to 0.5 pixels and requires that output.
The export contains the SHA-256 of the exact input packet, camera matrices,
unit scale, exposure, effect flags and the jitter used. It contains no runtime
pointers. The regular PPM is still produced. With zero jitter, the local
frame-1001 image differs from the non-export path by at most one channel level
on one pixel.

The renderer can also resolve history on GPU with `--temporal-input
<history.bin> --previous-packet <previous.bin> --temporal-output <new.bin>`.
It checks the previous packet hash and camera, dimensions, units, lighting,
textures, shadow settings and effect flags before using history. The new HDR
buffer contains the resolved result and can feed another frame. Its default
history weight is 0.8; `--history-weight` accepts 0 to 0.95.

The GPU pass and the separate **offline CPU reference**
`scene_temporal_resolve.py` reproject each current static world position
through the previous camera, compute a current-to-previous pixel vector,
reject out-of-frame or position-mismatched history, clamp accepted HDR history
to the current 3x3 color neighborhood, and blend before the existing display
conversion. Sampling stays nearest except for a 1/32-pixel transition before
texel boundaries; each contributing history sample must pass the position
check independently. This limits precision-dependent discontinuities without
the full bilinear blur that degrades the subpixel fixture. This is a standalone frame consumer, not yet an integrated
real-time game loop. The CPU reference accepts `--weight` from 0 to 0.95.
When the exported renderer uses `--filmic`, the resolve applies the same
filmic curve; frames with different effect or curve flags cannot be combined.

`--upscale 2` optionally reconstructs the output image at twice the width and
height, in linear HDR before tonemapping. It uses bilinear weights while
rejecting neighbors across invalid-position boundaries or more than 0.15 m
apart; this avoids some cross-edge bleeding. History and motion vectors stay
at their native resolution. This is an offline spatial preview, not temporal
super-resolution or a claim of recovered detail; distant or slanted surfaces
can fall back toward nearest sampling. The default is no upscaling.

For runtime inputs, supply the synchronized SceneIR and PBR packet manifest
for **both** frames and the actual finalized capture with `--trace`. The tool
requires consecutive frames in the same two- or three-frame capture, the same
observed device and a continuous Present thread, no Reset anywhere in the
capture, and matching complete source fingerprints. It rechecks each listed
submission, map name, camera, viewport and mesh transform against its own
synchronized Present interval, plus the bed/BSP metadata and candidate draw
arguments.
The trace and snapshots must remain unchanged while being read. The device
address is used only for an in-memory comparison, never exported. These checks
do not re-prove offline asset fingerprints, GPU payload bytes, or the unique
BSP match against the complete offline fixture; those remain the producer's
evidence and limitations. A frame with no independently verified bed and BSP
cannot currently be built. Independent captures cannot be combined merely
because their frame numbers increase. The tool also requires matching
light calibration and positions, material and shadow settings, dimensions,
units and effect flags. Enabled generated bed, candlestick, arch and bedroom
detail geometry must have the same GLB hashes in both manifests; enabling,
disabling or changing an override, including the prop material option,
invalidates history. Metallic mode and its declared factors must also match;
the GPU history parser checks the set of texture, emission and metallic
bindings for version-5 packets. These checks do not prove that actor transforms or
lighting were unchanged between captures. History is rejected per pixel when
world positions differ by more than 0.05 m. Object motion vectors, animated
geometry, transparency and temporal denoising are absent; the narrow transition
and fixed rejection threshold can still ghost or flicker.
This is a static-scene camera-motion prototype, not a production TAA claim.

The GPU packet does not contain SceneIR frame IDs. Before a runtime GPU
resolve, run `scene_temporal_resolve.py <previous-history> <current-unresolved>
--previous-packet <previous-packet> --current-packet <current-packet>
--previous-scene <previous-scene> --current-scene <current-scene>
--previous-manifest <previous-manifest> --current-manifest <current-manifest>
--trace <finalized-capture.jsonl>
--validate-only`. Then use the same current packet, effects and jitter for the
GPU resolve. This validates provenance without writing a resolved artifact;
the GPU parser independently checks pixel bounds. Direct GPU use prints that
frame order is unverified and must not be presented as synchronized runtime
history without this check.

To prepare SceneIR inputs from a new finalized multi-frame trace, use:

```sh
python3 -B t3render/tools/build_scoped_scene_sequence.py <capture.jsonl> \
  --scene build/t3render-offline/SouthQuarter1_int1.scene.json \
  --pass 0 --stage 1 --output-dir build/t3render-offline/<new-sequence>
```

The output directory must be new. Each frame is built independently using an
unambiguous verified bed anchor; if any frame lacks the required proof, changes
device, contains Reset, or fails continuity checks, no sequence is written.
`--include-asset-candidates`, `--allow-quantized-rotators` and
`--offline-actors <export.json> --game-dir <game>` enable the same separately
labeled offline candidate paths as the single-frame producer. The resulting
snapshots still need PBR packets and temporal buffers for both frames. The
local frame-1001 trace contains only one frame and cannot satisfy this command.

On 2026-11-16 a [new capture](../benchmarks/runtime/2026-11-16-scene-sequence/result.json)
produced synchronized frames 2863–2865 with no Reset and one device. The
sequence producer independently selected the bed and nine BSP blocks in each
frame. With offline mesh candidates enabled, each partial scene contains 57
submissions and 32 mesh instances, of which 31 use candidate asset bindings.
Standalone flat D3D12 renders of the bed and BSP succeeded for all three frames
and visually resemble the corresponding pre-Present bedroom screenshots.
The first two renders and cameras are identical; the third camera and image
differ slightly. Textures, lights, other objects and HUD are absent, and pixel
parity is not claimed. The raw trace, snapshots and images remain under `build/`.

The same capture could not produce the existing light candidates: the direct
candidate path requires unavailable pre-Reset shader evidence. The separate
`--offline-lights` PBR option uses a fingerprinted offline fixture and artistic
calibration, with no claim that those lights are active in TDS. Using that
option, frames 2864 and 2865 produced PBR/HDR renders with shadows, indirect
light, GTAO and filmic conversion. Both CPU and GPU resolved their temporal
history after checking the finalized trace. The CPU reprojected 783,848 pixels,
rejected 2,505 for position and 79 outside the frame; the maximum reprojection,
including jitter, was 1.009 pixels. CPU and GPU images differed on 12,321 pixels,
with mean absolute RGB-channel difference 0.0054 and maximum 5 on a 0–255 scale.
The older nearest-only path on the same inputs had maximum 45; the synthetic
subpixel fixture still improved mean error from 6 to 0 on both CPU and GPU.
This remains a two-frame static-scene experiment, not a live game loop or proof
of runtime light state.

Synthetic examples must use `--synthetic`; their manifest records
`source = synthetic` and has no frame IDs. In particular, rerendering frame
1001 with different jitter is a synthetic sampling experiment, not a second
runtime frame. The local experiment reprojected 784,604 pixels and rejected
1,792 for position differences. Its images and game-derived data stay under
`build/`.

The output is a new temporal buffer plus `.ppm`, `.motion.bin` and
`.manifest.json`. Motion vectors map the current pixel to the previous frame
in pixels, include jitter, and use status 0 = background, 1 = outside,
2 = disoccluded, 3 = accepted. Both history and motion headers contain the
current packet SHA-256; the manifest records input and output hashes. Binary
history stores top-left, row-major RGBA16F linear color followed by RGBA16F
camera-relative positions in metres, with position alpha marking valid pixels.

## Synthetic verification

Create `scene_temporal_fixture.py create <packet.bin>`, render it with the PBR
executable at jitter -0.25 and +0.25, each with its own temporal buffer, and
resolve them with `scene_temporal_resolve.py <previous.bin> <current.bin>
--previous-packet <packet.bin> --current-packet <packet.bin> --synthetic
--weight 0.5 --output <new.bin>`. To build a four-sample GPU reference, render
the same packet at horizontal jitters -0.375, -0.125, 0.125 and 0.375 and use
`scene_temporal_fixture.py reference <new.ppm> --packet <packet.bin> --sample
<buffer>` four times. Then `verify <resolved.bin.ppm> --reference <new.ppm>
--jittered <current.ppm>` compares the result with the current sample.

On the local D3D12 path, mean RGB-channel error against the four-sample
reference fell from 6 to 0 for this texture fixture on both CPU and GPU. A
camera translation produced one-pixel vectors, while a 0.2 m depth change
rejected all 4,096 fixture pixels; the GPU matched both controls. On the
synthetic frame-1001 pair, CPU and GPU differed in 9,886 pixels with mean
absolute RGB difference 0.00424 and maximum channel difference 8 on a 0–255
scale. Those earlier measurements are synthetic checks. The separate real
two-frame experiment and its offline lighting limitations are described above.
`test_scene_trace_session.py` exercises synthetic multi-frame
continuity and the CLI; `test_scene_temporal_resolve.py` covers history
rejection, clamping and runtime provenance; `test_scene_temporal_history.py`
tests the GPU input parser.
