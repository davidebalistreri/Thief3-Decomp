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
conversion. This is a standalone frame consumer, not yet an integrated
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
for **both** frames. The tool verifies each scene, packet hash, camera, frame
ID and fingerprint; it requires increasing frame IDs and the same complete
source fingerprints, including the trace, map and assets. Thus both frames
must come from the same finalized trace; independent captures cannot be
combined merely because their frame numbers increase. It also requires matching
light calibration and positions, material and shadow settings, dimensions,
units and effect flags. Enabled generated bed, candlestick, arch and bedroom
detail geometry must have the same GLB hashes in both manifests; enabling,
disabling or changing an override, including the prop material option,
invalidates history. Metallic mode and its declared factors must also match;
the GPU history parser checks the set of texture, emission and metallic
bindings for version-5 packets. These checks do not prove that actor transforms or
lighting were unchanged between captures. History is rejected per pixel when
world positions differ by more than 0.05 m. Object motion vectors, animated
geometry, transparency and temporal denoising are absent;
nearest history sampling and a fixed rejection threshold can ghost or flicker.
This is a static-scene camera-motion prototype, not a production TAA claim.

The GPU packet does not contain SceneIR frame IDs. Before a runtime GPU
resolve, run `scene_temporal_resolve.py <previous-history> <current-unresolved>
--previous-packet <previous-packet> --current-packet <current-packet>
--previous-scene <previous-scene> --current-scene <current-scene>
--previous-manifest <previous-manifest> --current-manifest <current-manifest>
--validate-only`. Then use the same current packet, effects and jitter for the
GPU resolve. This validates provenance without writing a resolved artifact;
the GPU parser independently checks pixel bounds. Direct GPU use prints that
frame order is unverified and must not be presented as synchronized runtime
history without this check.

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
scale. These are synthetic checks; no two-frame runtime capture has been
claimed. `test_scene_temporal_resolve.py` covers history rejection, clamping and
runtime provenance; `test_scene_temporal_history.py` tests the GPU input parser.
