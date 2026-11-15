# Offline temporal accumulation preview

The PBR renderer can export bounded scene-linear HDR and camera-relative
position buffers with `--temporal-output <new.bin>`. `--jitter <x> <y>` adds a
subpixel offset in the range -0.5 to 0.5 pixels and requires that output.
The export contains the SHA-256 of the exact input packet, camera matrices,
unit scale, exposure, effect flags and the jitter used. It contains no runtime
pointers. The regular PPM is still produced. With zero jitter, the local
frame-1001 image differs from the non-export path by at most one channel level
on one pixel.

`scene_temporal_resolve.py` is an **offline CPU prototype**, not an integrated
real-time TAA implementation. It reprojects each current static world position
through the previous camera, computes a current-to-previous pixel vector,
rejects out-of-frame or position-mismatched history, clamps accepted HDR
history to the current 3x3 color neighborhood, and blends before the existing
exposure/Reinhard/sRGB conversion. Its history output can be used in a later
resolve. The default history weight is 0.8, adjustable from 0 to 0.95.

`--upscale 2` optionally reconstructs the output image at twice the width and
height, in linear HDR before tonemapping. It uses bilinear weights while
rejecting neighbors across invalid-position boundaries or more than 0.15 m
apart; this avoids some cross-edge bleeding. History and motion vectors stay
at their native resolution. This is an offline spatial preview, not temporal
super-resolution or a claim of recovered detail; distant or slanted surfaces
can fall back toward nearest sampling. The default is no upscaling.

For runtime inputs, supply the synchronized SceneIR and PBR packet manifest
for **both** frames. The tool verifies each scene, packet hash, camera, frame
ID and fingerprint; it requires increasing frame IDs, matching map/assets,
light calibration and positions, material and shadow settings, dimensions,
units and effect flags. These checks do not prove that actor transforms or
lighting were unchanged between captures. History is rejected per pixel when
world positions differ by more than 0.05 m. Object motion vectors, animated
geometry, transparency, temporal denoising and GPU history reuse are absent;
nearest history sampling and a fixed rejection threshold can ghost or flicker.
This is a static-scene camera-motion prototype, not a production TAA claim.

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
reference fell from 6 to 0 for this texture fixture. A camera translation
produced one-pixel vectors, while a 0.2 m depth change rejected all 4,096
fixture pixels. These are synthetic checks; no two-frame runtime capture has
been claimed. `test_scene_temporal_resolve.py` also covers history rejection,
clamping, malformed buffers and runtime provenance.
