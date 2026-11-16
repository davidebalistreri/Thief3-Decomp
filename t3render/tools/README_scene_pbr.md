# Independent SceneIR PBR/HDR preview

`t3render_scene_pbr_d3d12` is a separate, standalone SceneIR consumer. It uses
the synchronized camera and one scoped set of asset-backed geometry, not the
legacy draw states, shaders or multipass replay. `scene_pbr_packet.py`
revalidates both the geometry snapshot and a same-frame candidate-light
snapshot against the trace and fingerprinted offline assets. The snapshots'
existing provenance limitations remain in the packet manifest.

The exporter selects the same bed-relative BSP wall and floor replacements as
the Godot prototype. Their locally generated albedo, OpenGL normal, roughness
and AO PNGs become bounded 512² RGBA8 mip chains; albedo mips are filtered in
linear light and encoded back to sRGB. Other opaque materials use
their offline diffuse texture, geometric normals, roughness 0.8 and AO 1.
The world-space expansion reverses triangle winding for mirrored instances.
Sections whose offline diffuse has non-opaque alpha are excluded and listed
in the manifest; alpha alone does not prove the original blend mode. Runtime
light positions are scoped **candidates**. The calibration file selects
omni/point lights and supplies artistic linear colors, intensities, source
radii, ambient and exposure. These choices are not measured runtime values.
Spotlights are rejected rather than approximated as point lights.

The albedo texture uses an sRGB view, so hardware decodes before filtering;
normal, roughness and AO use linear views. The original assets' precomputed
diffuse mips have unknown filtering. The pixel shader samples the
UV-inferred tangent-space normal, and evaluates a dielectric Cook–Torrance
GGX/Schlick BRDF. Point-light energy falls with inverse squared distance in
metres, clamped inside the supplied source radius. A constant ambient term
is modulated by AO. Scene-linear light accumulates in an RGBA16F target;
energy is clamped to the finite half-float range before a separate exposure,
Reinhard and sRGB display pass. The PPM is an SDR preview of the HDR buffer.

`--filmic` optionally replaces Reinhard with the fitted curve
`x(2.51x+0.03)/(x(2.43x+0.59)+0.14)` after exposure and before sRGB encoding.
This is an artistic filmic approximation, not a complete ACES color-management
pipeline or automatic exposure. The original Reinhard curve remains default;
the temporal export records the selected curve for its CPU resolve.
With the existing frame-1001 calibration, this option changes all 786,432
pixels with mean absolute RGB difference 10.978 on a 0–255 scale. Highlights
may require a different artistic exposure.

This is a first PBR preview, not a physically calibrated lighting solution:
source strengths, color and exposure are artistic; the SDK metre scale and
tangent basis are assumptions. Metallic response and temporal history are
optional previews. The render is a partial runtime-selected scene, and offline asset bindings for
some actors remain candidates. Height maps are not used. The legacy renderer
and its benchmark remain independent.

## Screen-space diffuse bounce

The optional renderer flag `--indirect` adds one approximate diffuse bounce
before exposure and tonemapping. The geometry pass records camera-relative
positions, geometric normals, albedo and directly lit diffuse radiance in
half-float buffers. The display pass gathers 24 nearby visible samples,
weights their colors by estimated surface area, distance and both surface
orientations, and rejects sampled foreground blockers. It does not feed the
constant ambient term, specular highlights or emission back into the bounce.

This is a bounded screen-space approximation, not complete GI: the gather
uses at most 64 pixels and 3 metres, only the first visible layer, and two
blocker checks. Hidden and off-screen surfaces, multiple bounces, temporal
history and moving objects are unavailable. Thin or unsampled blockers can
leak light, and finite sampling can produce noise or flicker. Surface area,
the Fresnel-weighted source diffuse and unit scale are approximations; light
and exposure remain artistic preview inputs. The effect is off by default.

On the frame-1001 shadow preview it changes 65,004 pixels, with mean absolute
RGB difference 0.066 on a 0–255 scale and a maximum channel increase of 80.
This is a subtle, incomplete local bounce rather than a claim of visual parity
or a calibrated GI solution. With the flag omitted, the same frame was
byte-identical to the previous version.

## Horizon ambient occlusion

`--gtao` independently enables a GTAO-style horizon approximation. Four
screen-space slices search both directions for nearby occluders, then integrate
the visible cosine-weighted angular range against the geometric normal. A
normal-offset bias and distance fade reduce self-occlusion. The search is
limited to 32 pixels and one metre. It attenuates the ambient term and, when
`--indirect` is also enabled, the diffuse bounce; direct light and specular
are not attenuated. The existing material AO remains a separate multiplier.

This is a sparse, first-layer approximation: off-screen or hidden occluders,
thin geometry between samples and temporal stabilization are unavailable.
The distance fade, radius, unit scale and normal bias are preview assumptions,
and may cause halos, light leaks or flicker. It is not a ground-truth or
production-quality GTAO implementation. It is off by default.

On the frame-1001 preview, GTAO alone changes 119,825 pixels with mean absolute
RGB difference 0.193 on a 0–255 scale; adding it to the diffuse-bounce preview
changes 121,881 pixels with mean difference 0.199. Both are measurements of
this partial scene and artistic lighting, not a comparison against TDS.

## Emissive candle preview

`--shadows --candle-flames <candlestick_high.glb>` reads the `CandleFlame`
primitive from the locally generated Godot replacement. For a submitted
`GENxLIGHT2candles` actor, it requires a calibrated same-identity candidate
light and a matching fingerprinted offline actor. The actor's observed
transform places the generated geometry; its offline on/off flag and the
candidate's nonzero preview intensity determine whether the flames appear.
The manifest records the GLB hash, actor, light and these limitations.

Packet version 4 adds bounded linear HDR emission per draw. Flame radiance is
artistic, defaults to 8 with warm color `(1, 0.4, 0.08)`, and can be changed
with `--flame-radiance`. With `--indirect`, visible emissive triangles also
contribute to the screen-space diffuse bounce. Direct illumination still comes
from the existing calibrated point-light candidate; it is not inferred from
the flame geometry. No runtime flame state is observed, and the original
non-opaque wax section remains omitted. Versions 1–3 remain supported.

The local frame-1001 render adds one pair of flames, 248 triangles. Against
the same indirect/GTAO preview without flames, 62 pixels change and the mean
absolute RGB difference is 0.0006 on a 0–255 scale. The small projected flames
and incomplete screen-space bounce are not a final candle-lighting solution.

## Semantic high-poly bed

`--bed-high <bed_high.glb>` opts into the generated Godot bed replacement and
requires `--bedroom-remaster-materials`. It selects the submitted
`StaticMeshActor__1520 / INNbed1A1` semantic anchor, checks the local mesh
bounds against the source asset and uses the observed actor transform. The
original bed sections and its offline caster candidates are excluded; the
generated geometry casts according to the fingerprinted offline actor flag.
Removing the option restores the original preview. This affects only the
render packet, not gameplay, collisions or the original game renderer.

The six generated material sections use the existing floor wood, wool and
pillow textures plus simple seam colors. Their mapping and normal strengths
approximate the Godot materials; vertex tangent frames are inferred from UVs
with a flat-normal fallback. They are not a full Godot shader conversion. The
local GLB hash, semantic resource identity, triangle count and fallback count
are recorded in the manifest; runtime asset files are not committed.

For frame 1001 the generated bed has 20,420 triangles, with 384 tangent
fallback triangles. Against the same filmic PBR preview with the original bed,
258,815 pixels change and mean absolute RGB difference is 2.720 on a 0–255
scale. The replacement is a visual reconstruction, not proof of geometric
parity with the original bed.

## In-process semantic bed switch

Add `--bed-variants` together with `--bed-high` to build a local variant
bundle. The exporter creates the original and replacement packets from the
same revalidated SceneIR and checks unrelated draw and offline caster records,
geometry, textures and lighting context against the packet bytes. The bundle contains two complete packets and a qualified
`INNbed1A1` resource key; its manifest records that key, the actor, the
individual packet hashes and the replacement GLB hash. It contains game data
and remains under `build/`. The two-packet prototype duplicates geometry and
textures; the frame-1001 bundle is approximately 102 MB.

The standalone renderer's `--variant-cycle <replacement.ppm> <restored.ppm>
--resource-key <key>` renders the original to `--output`, then selects the
replacement and original again in the same process. Alternatively,
`--variant-stream --resource-key <key>` renders the original first and accepts
repeated `replacement <new-output-path>`, `original <new-output-path>` and
`quit` lines on stdin. Each output is exclusively created. The key must match
the bundle's exact `variant.resource_key`; no draw number is used.

The fixed snapshot and both variants remain in memory while the process runs.
The renderer builds a combined vertex buffer, deduplicates identical textures
and reuses one D3D12 device, pipeline and GPU resource set across selections.
Each render clears depth and shadow maps before drawing the selected variant.
The frame-1001 test uses 51 shared textures instead of uploading 44 and 52
separately. This is not a real-time performance claim or a live TDS bridge.
Temporal options cannot be combined with either switch mode, so history is
reset on every switch. Gameplay,
collision and the original renderer are untouched. In the local frame-1001
D3D12 cycle, the restored original is byte-identical to the initial original,
and the replacement is byte-identical to the earlier high-poly render. The
variants differ at 259,095 pixels, with mean absolute RGB difference 2.714 on
a 0–255 scale. Five consecutive stdin switches on one device produced the
same outputs as the separate-frame render. `scene_pbr_variants_fixture.py`
also verifies byte-identical standalone and resident variants with and without
the optional G-buffer effects and without shadow maps. This does
not establish dynamic-scene or game-loop integration.

## Semantic high-poly candlesticks

`--candlestick-high <candlestick_high.glb>` opts into the generated Godot
metal and wax geometry for the submitted bedroom `D_384__5` and `D_384__13`
`GENxLIGHT2candles` candidates. It requires bedroom remaster materials,
checks the generated local bounds against the candidate mesh and places each
replacement using its scoped actor transform. The original sections and
offline caster candidates are excluded. Removing the option restores the
original preview, without affecting gameplay or the original renderer.

The metal uses `candlestick_metal_v2` albedo, normal and roughness with a
constant AO; wax uses a simple opaque color. Unless `--metallic` is enabled,
the renderer uses a dielectric BRDF even for metal. UV-inferred tangent frames have a flat-normal
fallback. Actor ownership and transforms are scoped runtime observations, but
the resource binding remains an offline candidate. Existing offline shadow
flags and owner-light exclusions still apply. The GLB hash, qualified actor
and resource IDs, triangle and fallback counts are in the packet manifest.

Flames remain a separate `--candle-flames` option, and use offline light state
and artistic radiance. When both options are set, they must reference the same
GLB to keep flame and holder geometry aligned. In the frame-1001 preview the
one selected holder has 4,328 triangles, with 392 tangent fallback triangles.
With all other preview options unchanged, the D3D12 render changes 3,228
pixels and has mean absolute RGB difference 0.054 on a 0–255 scale.

## Metallic PBR preview

`--metallic` opts into packet version 5, with a bounded per-draw metallic
factor. It assigns the generated `METAL1Goodness_D` candlestick a factor of
0.8, matching the Godot material; other current materials remain dielectric.
Versions 1–4 and the default behavior remain unchanged. The shader mixes
dielectric F0 with linear base color, reduces the diffuse and ambient terms,
and uses colored Fresnel for screen-space reflections. Diffuse bounce and
GTAO respect the same factor. The manifest records the mode and factors.

This is not a complete metallic material pipeline: no metallic texture or
image-based environment illumination is loaded, the material's source factor
is a preview mapping, and screen-space reflections miss off-screen geometry.
The scene remains a partial runtime selection with candidate asset bindings.
In frame 1001 the visible candlestick uses the factor; compared with the same
preview without `--metallic`, 583 pixels change, with mean absolute RGB
difference 0.0103 on a 0–255 scale. This is not a legacy parity measurement.

`scene_pbr_metallic_fixture.py` verifies factors 0, 0.5, 0.8 and 1 against an
independent GGX calculation. Synthetic D3D12 checks also confirm no diffuse
bounce or ambient-occlusion attenuation on a fully metallic receiver, a colored
metallic reflection, and a byte-identical dielectric-only v5 render versus the
v1 control. The previous
frame-1001 v4 preview also remains byte-identical. For one synthetic v5
history pair, CPU and GPU resolve differ in 55 pixels by at most one RGB level;
this is not validation of two synchronized runtime frames.

## Semantic high-poly arch detail

`--arch-high <rosette_high.glb>` selects the submitted bedroom candidate
`StaticMeshActor__59 / GENsknDOORFRMarc4x10half` and requires both its stone
and rosette sections in the selected scope. The original stone geometry is
preserved and uses `stone_wall_v2`; its `CIVGoodness_D` rosette is replaced
by generated relief using `arch_stone_v2`, and generated stone jambs are
added. The local bounds are checked against the corresponding source
sections before applying the scoped actor transform. Removing the option
restores the original packet. The original submitted rosette is not added
again as an offline caster; the generated and preserved geometry use the
offline actor's cast flag and light ownership rules.

The actor's mesh binding is a fingerprinted offline candidate; the runtime
transform and submission are scoped observations. The generated GLB hash,
actor, sections and counts appear in the manifest. UV-inferred tangents and
the material mapping are preview approximations. Frame 1001 contains 6,696
generated triangles, with 360 tangent fallback triangles; the D3D12 preview
changes 43,705 pixels versus the same preview without this option, with mean
absolute RGB difference 0.108 on a 0–255 scale. The camera shows mainly the
arch edge, so this image does not establish a detailed visual comparison of
the rosette.

## Bed-relative bedroom details

`--bedroom-details <bedroom_details.glb>` adds the locally generated Godot
baseboards, trim and book using the submitted bed's transform and a bounded
bed-relative region. This is an offline artistic addition: it is not a
visibility or ownership observation from TDS, and does not modify SceneIR,
gameplay or collision. Its draw records and manifest state this provenance.
The option requires bedroom remaster materials and is off by default.

Wood uses `beam_wood_v2` albedo, normal and roughness with constant AO;
pages and leather use constant colors and roughness. The rectangular wood
texture is resampled to the packet's square dimensions while retaining its
normalized UV mapping. Height maps and Godot's material parameters are not
fully reproduced. If shadow maps are enabled, these artistic additions cast
shadows for the selected lights; there is no observed runtime cast flag.

Frame 1001 has 6,956 detail triangles, none requiring tangent fallback. With
all other options unchanged, the D3D12 preview changes 36,489 pixels with
mean absolute RGB difference 0.166 on a 0–255 scale. These figures do not
establish runtime parity.

## Bedroom prop materials

`--bedroom-prop-materials` opts into two Godot wood mappings on the selected
scope: `StaticMeshActor__258 / GENrafters1 / GENbeamA_D` uses `beam_wood_v2`,
and `D_1224__21 / GENxGOODScrateB / HSEGoodnessA_D` uses
`furniture_wood_v2`. The qualified actor, mesh and material must all match;
other actors and materials are unchanged. The manifest records the selected
actors and texture hashes. These resource bindings are offline candidates,
and the material conversion uses constant AO and inferred tangents.

On frame 1001 both candidates match; 48 rafter triangles use flat-normal
fallback. With all other options unchanged, the D3D12 preview changes 22,153
pixels with mean absolute RGB difference 0.147 on a 0–255 scale. The frame
does not provide a close view of either prop.

## Volumetric preview

`--volumetric` integrates a homogeneous participating medium from the camera
to each visible surface. Sixteen midpoint samples accumulate point-light
scattering with a Henyey–Greenstein phase approximation and exponential
extinction along both view and light paths. Existing point shadow maps occlude
the scattering when available; without them, scattering is unshadowed. The
result is combined in linear HDR before exposure and tonemapping.

The fixed preview coefficients are extinction 0.06/m, scattering 0.045/m and
anisotropy 0.2; they are artistic assumptions, not measurements of TDS.
Scattering is integrated for at most 50 m and only up to the first visible
surface. Empty background, multiple scattering, moving media, temporal
stabilization and spatial density variation are absent. Sixteen steps and
single shadow samples can band, leak or miss fine features. The option is off
by default and is not a real-time performance claim.

On the frame-1001 PBR/indirect/GTAO/flame preview it changes 786,225 pixels,
with mean absolute RGB difference 11.976 on a 0–255 scale. The same frame
without the option remains byte-identical to the previous version. This is an
art-directed preview of a partial runtime scene, not parity with TDS.

## Screen-space reflections

`--reflections` traces a reflected view ray against the visible position
buffer, then samples the directly rendered HDR image. A dielectric Fresnel
term and roughness-dependent fade and nine-tap filter approximate rough
reflections. The result is added before volumetric attenuation and tonemapping.

This is a first screen-space preview, not ray tracing or a full PBR reflection
model. It uses geometric rather than normal-mapped normals, a depth-thickness heuristic,
up to 48 steps and a roughly 7 m ray. Off-screen and occluded surfaces,
indirect light, fog in reflected radiance, history, metallic response and a
fallback environment are unavailable. Roughness filtering is a small screen
kernel, not a physically correct microfacet convolution. Missing hits,
leaks, edge fade, banding and flicker are possible. It is off by default.

On the frame-1001 preview with the other optional effects enabled, reflections
change 68,050 pixels, with mean absolute RGB difference 0.0442 on a 0–255
scale. Without the flag the same frame remains byte-identical to the previous
version. This measurement is not evidence of parity with TDS.

## Point-light shadow maps

`--shadows` opts into a separate shadow-map path in packet version 2. Each
selected point light renders six radial-distance faces into an R32F cubemap
array with a shared depth buffer. The PBR pass compares 25 point samples per
light. Its sampling cone uses the calibrated source radius and an adjustable
softness multiplier; this is an area-light approximation, not exact physical
penumbra reconstruction. A receiver-plane approximation from interpolated
vertex normals and slope- and texel-scaled bias reduce self-shadowing, but
cannot eliminate all acne or light leaks.

Caster geometry comes from the selected, opaque same-frame sections only.
For mesh instances, the fingerprinted offline actor export supplies the cast
flag and mesh identity; a light's owner and attached parent are excluded for
that light. All selected opaque casters are rendered two-sided. Cast flags and
ownership are **offline candidates**, not observed runtime shadow state.
Off-screen objects and sections omitted from the capture, non-opaque
textures, and dynamic shadow-state changes can therefore produce missing or
incorrect shadows. The previous unshadowed version-1 packet remains supported.

`--shadows --offline-shadow-casters` optionally adds static caster candidates
from the fingerprinted offline SceneIR and actor export. Packet version 3
stores their positions separately: they contribute only to shadow maps, not the
visible geometry pass. BSP sections and unattached `StaticMeshActor` sections
with an enabled offline cast flag are considered when their world-space bounds
intersect a light's shadow range. Sections already submitted in the selected
scope are not duplicated; hidden, missing-material and non-opaque diffuse
sections are omitted. If an instance was observed in the scoped frame, its
runtime transform is used for its unsubmitted sections; otherwise its offline
transform is used. A manifest records the selection, exclusions and light masks.

These additions are **offline candidates**, not an observation of visibility,
runtime transforms or shadow flags. Save-game changes and unresolved
attachments can make them inaccurate or incomplete. Bounds are a coarse range
filter, not an occlusion or room-membership test. Both the candidate count and
the total GPU shadow work are bounded; exceeding a limit rejects the packet.
The default version-1 and version-2 packet bytes are unchanged.

Defaults are 512² per face, near 0.05 m, far 15 m, bias 0.015 m and softness
0.5. `--shadow-map-size`, `--shadow-near-m`, `--shadow-far-m`,
`--shadow-bias-m` and `--shadow-softness` change them; softness 0 gives a hard
comparison. Near clipping may omit nearby blockers; receivers past the far
distance are unshadowed. Sizes are powers
of two from 64 to 512, with at most 65,536 shadow draw calls and six million
triangle submissions. Six faces and 25 samples per light make this an offline
preview, not a real-time performance claim.

On the local frame-1001 five-light preview, 58 selected draws (3,445
triangles) cast for each light. Shadow maps change 651,125 pixels compared
with the same unshadowed PBR image, with mean absolute RGB difference 7.012
on a 0–255 scale. This measures a visual change, not parity with TDS. The
version-1 PBR image remains byte-identical when shadows are not enabled.

For the same frame, the optional offline selection adds 213 sections and 8,911
triangles, with 6,066 shadow draws and 272,352 shadow triangle submissions in
total. Compared with the scoped-only shadow render, 2,601 pixels change and
mean absolute RGB difference is 0.002 on a 0–255 scale. This is a measurement
of the preview, not proof of runtime shadow correctness.

For the local frame 1001, five preview omni lights, 26 textures, 59 draws and
3,553 triangles render the bedroom; one candle-wax section is excluded for
alpha. The stone and parquet replacements cover 45 and 22 triangles with no
tangent fallback. The light choices use warm artistic colors and are **not**
evidence for the original light intensity. The local preview and all packets
remain under `build/` because they contain game-derived assets.

## Reproduce

Requires the existing fingerprinted frame-1001 snapshots, trace, SDK exports,
local Godot material PNGs, ImageMagick 7, and a Windows x86 D3D12 toolchain.
From the T3SDK root, prepare a local example calibration (the values are visual
choices and may be adjusted). Use a new path if this file already exists:

```sh
python3 -B - <<'PY'
import json
from pathlib import Path
scene = json.loads(Path('build/t3render-offline/bedroom-runtime-1001-light-candidates-v1.scene.json').read_text())
lights = []
for light in scene['lights']:
    if light['kind'] != 'omni':
        continue
    blue = light['id'].endswith(':D_57__1')
    lights.append({'id': light['id'], 'color_linear': [.25, .4, 1] if blue else [1, .7, .42],
                   'intensity_candela': 2 if blue else 5, 'radius_m': .12})
calibration = {'schema': 't3.scene.pbr-preview', 'version': 1,
               'ambient_linear': [.025, .025, .03], 'exposure': 1.5, 'lights': lights}
with Path('build/t3render-offline/bedroom-runtime-1001-pbr-calibration.json').open('x') as stream:
    json.dump(calibration, stream, indent=2)
    stream.write('\n')
PY

cmake -S t3render -B build/t3render-mingw -DT3RENDER_BUILD_SCENE_PBR_D3D12=ON
cmake --build build/t3render-mingw --target t3render_scene_pbr_d3d12
python3 -B t3render/tools/scene_pbr_packet.py \
  build/t3render-offline/bedroom-runtime-1001-warm-stage1.scene.json \
  --light-scene build/t3render-offline/bedroom-runtime-1001-light-candidates-v1.scene.json \
  --calibration build/t3render-offline/bedroom-runtime-1001-pbr-calibration.json \
  --state-trace build/t3render-captures/scene-bridge-submission-20261115-001/bedroom.jsonl \
  --offline-scene build/t3render-offline/SouthQuarter1_int1.lights.scene.json \
  --offline-actors build/assets/godot/SouthQuarter1_int1/SouthQuarter1_int1.actors.json \
  --game-dir "$T3_GAME_DIR" --pass 0 --stage 1 \
  --bedroom-remaster-materials build/assets/godot/remaster/garrett_home/materials \
  --output build/t3render-offline/bedroom-runtime-1001-pbr.scene.bin
```

To build the shadowed alternative, append `--shadows` and choose a different
output path; all other arguments and the calibration stay the same.
Add `--offline-shadow-casters` as well to opt into offline candidates.
Add `--candle-flames build/assets/godot/remaster/garrett_home/highpoly/generated/candlestick_high.glb`
to include the generated flame geometry. This requires the existing Godot
replacement asset and shadows; its packet and manifest remain under `build/`.
Add `--bed-high build/assets/godot/remaster/garrett_home/highpoly/generated/bed_high.glb`
to use the generated high-poly bed instead of the original bed sections.
Add `--bed-variants` alongside it to produce an in-process switch bundle
instead of a single packet; see the semantic bed switch above.
Add `--candlestick-high build/assets/godot/remaster/garrett_home/highpoly/generated/candlestick_high.glb`
to replace the scoped bedroom candlestick metal and wax; optionally use the
same GLB with `--candle-flames` for emissive flame geometry.
Add `--arch-high build/assets/godot/remaster/garrett_home/highpoly/generated/rosette_high.glb`
for the bedroom arch's generated rosette and jambs.
Add `--bedroom-details build/assets/godot/remaster/garrett_home/highpoly/generated/bedroom_details.glb`
for optional bed-relative artistic trim and decoration.
Add `--bedroom-prop-materials` for the selected bedroom rafter and crate wood.
Add `--metallic` to use the Godot candlestick's metallic factor in PBR shading.

Run `t3render_scene_pbr_d3d12 <packet> --output <new.ppm> [--indirect] [--gtao] [--volumetric] [--reflections] [--filmic]` in a D3D12
environment. The packet, manifest and PPM use exclusive creation. See the
[Highball setup](README_synthetic_replay.md#repeat-on-highball); TDS does not
need to be running.

For static-camera temporal accumulation, its GPU pass and CPU reference,
see [offline temporal accumulation](README_scene_temporal.md).

## Verification

`python3 -B -m unittest discover -s t3render/tests -p test_scene_pbr_packet.py`
tests semantic selection, per-channel bindings, opacity exclusions, matching
frame/camera, calibration bounds, and the independent C++ packet parser.
`test_scene_pbr_offline_casters.py` additionally checks offline provenance,
selection, deduplication, transforms, opacity and parser/work limits.

For GPU verification, create a new fixture with
`python3 -B t3render/tools/scene_pbr_fixture.py create <new.bin>`, render it
with the PBR executable, then use `scene_pbr_fixture.py verify <new.ppm>`.
Repeat with `--half-unit`, `--extreme` and `--srgb-filter` on both create and
verify. All four fixtures passed on the local Highball/D3D12 path: they check
sRGB filtering, roughness, AO, normal direction, inverse
square distance in metres, HDR values above one and finite output at extreme
energy. The extracted shared D3D12 helpers were also checked by rendering the
existing legacy shadow packet again and comparing its PPM byte for byte.
The standard fixture also accepts `verify --filmic` to check the alternate
curve against independently computed HDR values; the local GPU and temporal
CPU display results matched on that fixture.

For shadow-map verification, `scene_pbr_shadow_fixture.py create <new.bin>`
creates a synthetic receiver and occluder; use `--baseline` for the unshadowed
input. Render both with the PBR executable, then run
`scene_pbr_shadow_fixture.py verify <shadow.ppm> --reference <base.ppm>`.
Repeat with `--face 0` through `--face 6` (6 is a cubemap seam),
`--asymmetric`, and `--two-lights`. `--hard` uses `--soft-reference <soft.ppm>`
at verification; `--short-range` checks the far-distance fallback and
`--behind` checks that a blocker behind the receiver does not shadow it. On the
local Highball/D3D12 path, these fixtures passed for the six faces, seam,
asymmetric occluders, independent light layers, soft versus hard penumbrae,
range fallback and an off-axis receiver self-shadow case.
`--offline-caster` produces a version-3 fixture with a shadow-only blocker;
add `--baseline` to omit that blocker while keeping the visible geometry
identical. The local GPU test passed with the blocker shadowing the receiver
and leaving an unoccluded sample unchanged.

`scene_pbr_indirect_fixture.py create <new.bin> --kind <kind>` produces a
synthetic receiver and source. Render each packet both with and without
`--indirect`, then run `scene_pbr_indirect_fixture.py verify <indirect.ppm>
--reference <base.ppm> --kind <kind>`. The local GPU checks passed for
`facing`, `flat`, `blocker` and `unlit`: colored transfer, source orientation,
an intervening blocker and absence of feedback from ambient-only light.

`scene_pbr_gtao_fixture.py` uses the same create/verify workflow with `--gtao`
for `flat`, `tilted`, `blocker` and `direct`. The local GPU tests verified
unoccluded flat and slanted surfaces, contact darkening, unchanged direct
lighting and use alongside `--indirect`.

`scene_pbr_emission_fixture.py` produces on/off version-4 packets. Render each
with and without `--indirect`; `verify` checks the emissive HDR surface and
its colored bounce, while `verify --off` checks that ambient-only illumination
does not produce a bounce. Both controls passed on the local GPU. The portable
`test_scene_pbr_flames.py` also checks GLB bounds, qualified selection,
transforms, offline on/off state and version-4 parser limits.

`scene_pbr_volumetric_fixture.py` provides `unlit`, `lit` and `shadow` packets.
Render the first two with and without `--volumetric`, and render `shadow` with
it; `verify` compares all five. The local GPU test verified extinction,
scattering and an occluder affecting only the shadow map. A version-1 packet
without shadow maps was also rendered with the option enabled.

`scene_pbr_reflection_fixture.py` provides `smooth`, `medium`, `rough` and
`away` packets. Render each with and without `--reflections` and use `verify`
with the corresponding base image. Local GPU checks observed a colored
reflection, reduced response at intermediate roughness, and no response for
a fully rough or oppositely oriented receiver.

`test_scene_pbr_bed.py` covers semantic selection, observed and mirrored
transforms, material bindings, source bounds, shadow flags and restoring the
original packet. The offline-caster tests also check that all original bed
sections are excluded when the generated replacement is active.
