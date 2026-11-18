# Resident semantic scene: experimental in-process adapter

`scene_live.hpp` and `scene_live.cpp` provide a C++ value model for a future
T3SceneBridge consumer. They are built as `t3render_scene_live` on Win32. An
experimental opt-in adapter now connects the existing probe and D3D8 facade
to the window and driver described below. An experimental D3D12 viewer can
consume its current snapshot in the same process. Later TDS captures produced
partial live frames, including three matched to the same D3D8 Present and
camera; prolonged synchronized movement is still unverified. A `synchronized` flag alone is an
assertion supplied by its producer, not proof of a Present window. The existing
JSON SceneIR schema has not changed.

Resources contain immutable geometry and sections, a semantic ID and the
fingerprint of their source asset. The caller must establish both the key and
fingerprint; the cache does not hash or authenticate the geometry. Materials
and per-instance bindings are separate. Register the catalog first and obtain
its immutable objects with `FindSceneResource` for the matcher. On publication,
the matcher's in-memory resource handle and material names must agree with the
resident objects; these handles are never serialized. Each selected mesh includes an
instance generation, transform, binding evidence (`Verified` or `Candidate`)
and the observed sections. BSP blocks have the same evidence distinction.
There are no COM, D3D or engine object types in these structures; producer IDs
must not encode pointers. A fingerprinted offline asset is still offline data.

`Publish` validates the entire partial selection before replacing the current
snapshot. It rejects missing resources/materials, duplicate or invalid sections,
stale frame numbers and epochs, unknown evidence and changes in session
provenance or coordinate conventions. Missing selections mean only that they were not submitted in this
selected scope: they do **not** prove an actor was destroyed or fully invisible.
An instance ID and generation cannot silently change resource; a producer must
increment its generation on identity reuse. Sampling the same address twice
is not sufficient to establish lifetime or rule out ABA. Old snapshots retain
immutable resources and can safely be read while a newer frame is published.

The lifecycle contract is:

1. `BeginMap(expected_map_epoch, name, fingerprint)` starts a new map epoch,
   including on a reload of the same map. It clears the published frame and
   catalog. Asynchronous catalog writes carry the expected map epoch.
2. Register materials and immutable resources for that epoch. Re-registering
   exactly the same resource succeeds; changing its content under an existing
   key fails. The cache is bounded.
3. Supply a complete camera and a partial selection in a `FrameInput` using the
   current map/device epochs, an increasing frame ID and explicit provenance.
   `LiveProcess`, `CapturedReplay` and `Synthetic` cannot be mixed within an
   epoch. Only `CapturedReplay` carries a capture fingerprint. The producer
   must independently verify the selected pass, thread and Present interval.
4. Call `BeginReset` before forwarding any Reset. It immediately retires the
   published frame and increments the device epoch, even on failure. Finish
   using the returned ticket; after a failed Reset no frame can be published
   until a later successful Reset. CPU asset data stays cached.
5. A GPU consumer must synchronize its epoch check with submission. An
   already-retained old snapshot can remain alive but must not be submitted
   after invalidation. Device-specific caches need invalidation on epoch change.

`scene_live_window.hpp` adds a Present-delimited collector for the future hook
adapter. The adapter must attest that no untracked callback is in flight;
the first successful Present only primes the next interval. A frame can be published after a consecutive successful Present when the selected probe,
scoped draws and both boundaries share the same thread ID and thread creation
identity; every probe, scope and draw must have returned without overlap. The
adapter supplies stable camera and draw-viewport samples, semantic scope matches
and successful draw results. The collector rejects changed camera/viewport,
conflicting instance data, missing samples, failed draws, unknown passes and
overlapping Presents. Every draw is bracketed, including those outside selected
scopes, so an outstanding draw prevents publication. An invalid interval
invalidates the previously published frame. When a fully bracketed interval
fails, a clean boundary can prime the next one. An overlap or `MarkLost` sets
`needs_rebind`; the adapter must drain old callbacks and create a new collector
before priming again. A late invalidation cannot erase a newer frame or a
different device/map epoch.

An adapter must bracket every relevant operation, report lost hooks/reentry,
validate its own scope identity and sampling, catch exceptions without exposing
them to the game, and recreate the collector after map/device epoch changes.
The collector cannot independently prove callback coverage, a live binding or
an unobserved Reset. A scope without a draw is not selected. When no selected
draw supplies a viewport, the adapter may inspect at most four unselected
draws in the interval after a stable selected probe with a matching candidate
map witness. It checks view/projection against that probe and checks viewport
stability before and after the draw. Differing camera-matched viewports make
the fallback unavailable; a selected draw viewport always takes precedence.
This can publish a partial camera-only snapshot, not a claim that missing
objects are invisible or destroyed. No fallback is attempted without a map
witness, and the witness still does not prove map lifecycle or ownership.
The camera's `viewport_source` and viewer diagnostic `viewport.source` record
`scoped_draw` versus `camera_matched_draw`; `unscoped_viewport_attempts` counts
requests. If neither source is available the interval remains rejected.
Unselected draws can be from another stage, so their viewport is evidence of
that camera-matched draw only, not proof of the selected stage's viewport.
The adapter's callback coverage and the extra getter cost in TDS are not yet
verified. Fallback reads view, projection and viewport, not world transform.

Host fixtures cover absence and return of selected geometry, mismatched camera,
conflicting viewport, missing witness, the four-attempt cap, selected-draw
precedence and Reset. A synthetic Highball/D3D12 worker test renders a
camera-only frame between geometry frames. These do not establish live TDS
visibility or measure performance.

`scene_live_binding.hpp` translates the existing probe values to this model.
At the section-call entry, `ResolveBed` accepts only the researched
`StaticMeshActor__1520` / `INNbed1A1` path with its owner, class, map, asset and
mesh-node checks. It compares bounds, counts, section ranges and runtime
material names with the offline resource and instance skin. `ResolveBsp`
requires a complete supplied offline BSP catalog, checks current VBSP membership
and stable metadata, and requires a unique match in both directions. The
importer itself is heuristic, so the catalog-completeness assertion is not an
independent runtime measurement. The map and asset fingerprints qualify offline
IDs; neither is a fingerprint obtained from the running resource manager.
Consequently both returned bindings are **candidates**, and no transient
process address is copied into them. The full diagnostic sample checks global
actor-name uniqueness. The lighter live sample checks uniqueness only in the
complete current visible list; `identity_scope` records this weaker evidence
and does not turn it into a global uniqueness claim.

The adapter calls the resolver at scope entry, checks the indexed triangle
draw arguments with `ScopeDrawMatches`, compares pre/post draw view, projection
and world matrices with `ScopeCameraMatches` / `ScopeWorldMatches`, and passes
those results to `SceneWindow::EndDraw`; it compares entry/exit samples with
`ScopeUnchanged` at `EndScope`. The draw check compares arguments and offline indices, not the VB/IB
bytes actually consumed by the GPU. These functions do not by themselves
prove callback ordering, visibility or the Present interval. If the target bed
is absent, a bounded probe may instead sample the owner, class and direct map
outer of a different current visible `StaticMeshActor`. The candidate must be
a unique list entry, a stable object-table member with a valid backlink, and
its outer must match the supplied map name. The probe rereads the selected
identity, class, outer and complete list; it tries at most eight visible mesh
elements and never reuses a witness from a prior frame. BSP matches using this
witness retain `Candidate` and `VisibleList` evidence. No suitable witness,
an unreadable/truncated list or a different map yields no selection. A bed
witness with a conflicting map is never bypassed. The witness does not
independently bind the VBSP to that map or detect every map transition, and
repeated reads cannot exclude ABA.

The target bed's class and map outer are checked together for unique table
membership, then checked again after sampling its asset and mesh node. A
changed identity invalidates the target as a witness; an independently sampled
visible actor may still provide a witness. The no-bed witness also checks the
class and outer together, without weakening the uniqueness checks. These
bounded double reads still cannot exclude ABA or unobserved mutations.

In the existing synchronized capture, frame 1243 contains no visible bed but
does contain other static actors and VBSP index 7. Its metadata uniquely
matches offline GMP block 5, including counts, bounds and material sections;
draw 10 is within the observed BSP submission. That old trace did not capture
the other actors' outers, so it cannot validate the new witness or prove the
map. The new path and disappearance/reappearance behavior are synthetic-only
until another TDS test.

For the later `scene-live-inprocess-20261116-diagnostic` capture, offline
callback replay of frames 1243–1245 rejects all three frames without a
validated viewport and accepts no semantic bed or BSP scope. The recorded
probe has no independent map-witness field; its VBSP index 7 metadata matches
the golden frame 2864, but that alone cannot establish ownership or map
identity. This replay is not a new live verification.

The local frames 2864/2865 were revalidated against their common finalized
trace and fed through the matcher, residency and Present collector: each yielded
two bed sections and two GMP-block-5 sections, with viewport depth 0..0.5. For
this offline exercise, the trace's recorded stable scope was represented by
identical entry/exit samples and a synthetic thread creation token. The recorded
draw matrices were compared once; post-draw matrix stability was not captured.
Those facts were not newly sampled from a live process. The resulting snapshots were marked
`CapturedReplay`, and their resource bindings remained `Candidate`.

`scene_live_flat.cpp` converts a selected snapshot to the existing API-neutral
flat SceneIR renderer input using row-major, row-vector transforms and stable
material-ID colors. Culling is disabled because absolute winding is unknown. It accepts T3 XYZ /
Z-up coordinates and rejects minus-one-to-one clip depth; when clip depth is
unknown the diagnostic backend assumes zero-to-one without upgrading the
source evidence. The output covers the camera's viewport-sized crop; nonzero viewport origin is
not composed into a full backbuffer. It expands vertices on the CPU each frame
and does not manage a D3D12 device, implement a streaming renderer or demonstrate
real-time performance. An empty selection produces an empty frame, which the
existing standalone D3D12 image renderer cannot yet display.

Run the synthetic lifecycle, visibility, transform, reset and concurrent-reader
fixture, and cross-compile the library:

```sh
python3 -B -m unittest t3render.tests.test_scene_live t3render.tests.test_scene_live_window t3render.tests.test_scene_live_binding
cmake --build build/t3render-mingw --target t3render_scene_live
```

A local check also loaded the captured SceneIR selections for frames 2864/2865
and revalidated their common trace with `scene_trace_session.validate`. For the
bed sections 1/2 and GMP block 5 sections 0/1, the flat adapter produced 798
vertices per frame; all transformed positions matched the independent SceneIR
calculation within 0.000123 T3 units and camera matrices matched. These are
captured inputs, not a live consumer or a GPU image; generations were assigned
for the fixture rather than observed as runtime lifetimes.

## Experimental catalog and hook adapter

`tools/export_live_catalog.py` exports a **local offline** SceneIR fixture to
a bounded binary catalog, alongside a JSON manifest. It contains the target
bed, BSP blocks and referenced materials; it is not a frame or a runtime asset
inventory. The loader checks structure and a SHA-256 trailer. This detects
accidental corruption; it does not authenticate the catalog or establish its
identity against a running resource manager. Do not distribute catalogs made
from game assets without considering their provenance.

```sh
python3 -B t3render/tools/export_live_catalog.py /path/to/offline-scene.json \
  --output /path/to/new-local-catalog.bin
```

To include the generated Godot high-poly bed as an optional viewer override,
add `--bed-high /path/to/bed_high.glb`. This produces catalog version 2;
without it the exporter still writes version 1. Both versions remain local
offline asset catalogs. The exporter checks the SDK unit scale, the six named
material sections and the local bounds against `INNbed1A1`, converts glTF Y-up
coordinates to T3 Z-up and reverses triangle winding for that reflection. It
preserves normals and UVs, but does not establish T3's absolute winding or
tangent convention. The source GLB's SHA-256 identifies the generated asset in
the manifest and resource ID; the catalog trailer detects corruption, not
provenance or deliberate tampering.

The viewer applies the replacement only to the catalog's map fingerprint,
`StaticMeshActor__1520` and original resident `INNbed1A1` resource, using the
selected instance transform. The source resource and snapshot are unchanged.
Replacement geometry and material IDs stay outside semantic residency; they
are held only by the viewer's override rule.
If any original bed section is selected, all six replacement sections are
drawn; selection is partial and does not prove that every part was visible in
TDS. A stale source handle fails closed, including after a map reload. The
diagnostic field `bed_override_instances` reports the number of replacements.
Without the optional material library the viewer uses flat material-ID colors;
if complete verified lights are available, those ID colors are instead used as
placeholder albedos in the experimental direct-light pass. Gameplay is unchanged.

The optional `--catalog <Windows-path-to-v2-catalog>` argument to
`t3render_live_dx12_fixture.exe` exercises the Windows loader and D3D12 viewer
backend with local catalog geometry, a synthetic camera and synthetic selection.
On Highball the real generated GLB (18,548 vertices, 61,260 indices) passed
original → override → original, movement, visibility and Reset checks. This is
an offline GPU fixture, not a runtime TDS result or a performance measurement.

## Bedroom visual materials

`--bedroom-materials /path/to/Godot/materials` adds the existing
`stone_wall_v2` and `wood_floor_v2` textures to a version 3 local catalog.
`--bedroom-texture-size` chooses a power-of-two size up to 1024 (default 512).
The optional `--bed-high` can be combined with it. The exporter generates
bounded mip chains, filtering albedo in linear space, and records source PNG
hashes. The loader checks the trailer, dimensions, complete mips, opaque
albedo and material IDs. Catalogs v1 and v2 remain supported. As with other
catalog fields, a checksum does not authenticate the texture's origin.

```sh
python3 -B t3render/tools/export_live_catalog.py /path/to/offline-scene.json \
  --bed-high /path/to/bed_high.glb \
  --bedroom-materials /path/to/Godot/materials \
  --bedroom-texture-size 512 --output /path/to/new-local-catalog.bin
```

The library is separate from semantic residency and is loaded once by the
viewer worker. On a selected `SouthQuarter1_int1` bed in the same snapshot,
the viewer partitions selected BSP sections by original material name,
bed-relative triangle centroid and orientation, using the same bedroom rules
as the existing offline preview. It keeps index order and the original
geometry and splits only the necessary indexed ranges. Without that bed
selection, or with an invalid anchor, it retains the source material IDs; it
does not reuse a transform from an earlier frame. These bounds are an artistic
region heuristic, not proof of room membership or total visibility.

The D3D12 viewer uploads albedo mips once and samples them with UV0 and wrap
addressing. Sections with missing UVs fall back to material-ID colors. Other
materials also remain ID colors. With unknown or incomplete lighting and no
explicit preview light this is **unlit**. The verified-light path below uses sRGB albedo and linear
roughness; its normal map is enabled only in a synthetic fixture, and AO is
not applied without an indirect-light term. These textures are remaster assets,
not measurements of the game's materials. Diagnostics report
`bedroom_material_runs`, `textured_draws` and `pbr_draws`; a difference in the
first two can indicate missing UVs. The separate `T3RENDER_SCENE_LIVE_BEDROOM_MATERIAL_IDS=1`
flag enables only the partition/ID diagnostic with an older catalog. A v3
catalog enables the material selection in the viewer automatically.

For the experimental head-wall relief, export the bedroom catalog with
`--wall-height` as well. This creates version 6, retaining the source PNG's
hash and mip chain separately; old catalogs do not implicitly enable relief.
It can also be combined with `--static-meshes`. The viewer packs the height
into the wall AO texture's alpha at upload, preserving AO RGB, and applies
the generated normal map and 24-layer POM only to the bed-relative wall
behind the headboard, on the fingerprinted bedroom map. The selection is a
triangle-centroid and orientation heuristic tied to the same-frame bed anchor.
`candidate_head_wall_triangles` reports the selected triangles, not proof
that textured POM was rendered; missing UVs, normals or lighting fall back.

The preview assumes a 5 cm relief depth and derives its tangent basis from
the draw's UV derivatives. It changes texture sampling, not geometry, depth,
silhouettes or shadow casters. Steep angles are bounded and can lose relief;
the normal convention and appearance have not been verified in TDS. The
[offscreen comparison](../../build/t3render-captures/scene-live-m4-wall-20261118-v6/M4-wall-report.md)
uses a captured selection with additional synthetic close cameras. No live
frame rate or GPU duration has been measured for this effect.

`--wall-displacement` in the bedroom runner selects a separate viewer-only
geometry experiment instead of POM. It tessellates each selected head-wall
triangle into 576 triangles and offsets the vertices by the same height map,
up to 5 cm, with a 12 cm fade at the known room edges. It requires the exact
bedroom map and the captured bed transform; an incompatible or over-budget
frame retains POM and sets `wall_displacement_failed`. The generated geometry
casts shadows and is cached by source range and bed transform, without changing
the game or SceneIR. Vertex normals use the height-field gradient so triangles
with matching UV mappings agree at their shared edges. Gaps or seams can still
occur at incompatible UVs or missing geometry; the mesh
and its performance have not been verified in TDS. Diagnostics expose source
and displaced triangle counts.

`--preview-gi` opts into a GPU screen-space one-bounce preview on a lit
viewer path. It gathers only direct diffuse radiance from the selected head
wall and applies it only to the generated `bed_high` receiver. Four additional
half-float targets store camera-relative position, geometric normal, direct
diffuse and albedo; ambient and earlier bounce are not fed back. The gather
uses 32 samples within 128 pixels and three metres, with two approximate
blocker checks. Off-screen and occluded geometry, other receivers, multiple
bounces and temporal history are absent. Noise, light leaks and flicker are
possible. `preview_gi_enabled` means the pass was enabled for that frame, not
that a contribution was observed. It requires the v6 wall-height catalog for
the wall selection. The [offscreen report](../../build/t3render-captures/scene-live-m5-gi-20261118-v4/M5-gi-report.md)
records the captured-pose comparison and its controls; TDS runtime behavior
and reliable GPU timings remain unverified.

For an explicitly artificial material preview, opt in with
`T3RENDER_SCENE_LIVE_PREVIEW_PBR=1` (or `--preview-pbr` in the bedroom runner).
When verified lighting is unavailable, the viewer uses a neutral point light
at the current camera position to exercise the existing GGX/HDR path. The
window title and diagnostic `lighting_mode=preview_headlight` identify this
mode. It is not a sampled TDS light, does not alter the scene's light evidence,
does not cast shadows, and must not be used as the candle verification. The
light follows the camera, so its brightness is not the original game's
illumination. If verified lighting is available it takes precedence. This
preview uses albedo and roughness but retains the fixture-only normal-map and
missing-indirect-light limits described below.

The separate opt-in `--live-lights --preview-candle` uses a same-frame raw
candidate for the bedroom `D_384__5` position and normalized color, with
an artistic 1 cd intensity at factor one and a 3 m range. These are not
measured game values. It casts one bounded shadow using only selected
geometry; it does not prove that the game light is enabled. It multiplies the artistic intensity by the candidate
`+0x8c` factor. A missing or stale sample falls back to a dim artistic ambient,
labelled `preview_candle_unknown`, rather than reusing another frame's light;
this does not prove the light is disabled. The [live report](../../build/t3render-captures/scene-live-m3-preview-20261117-mac-v1/M3-live-report.md)
records the earlier preview and the [toggle report](../../build/t3render-captures/scene-live-m3-toggle-20261118-mac-v1/M3-toggle-report.md)
records the subsequent experiment and offscreen fallback correction.

With `--candle-link-diagnostics` the viewer can also draw the generated
metal and wax candlestick (4,328 triangles) at the same-frame visual actor
transform, including when its sampled factor is zero. The untextured materials
use artistic colors. A stable actor transform can also retain the candlestick
when the actor has no light link; this does not establish whether the light is
disabled. It can also draw the generated `CandleFlame` mesh
(248 triangles) at the same actor transform.
The light-list and actor-link samples must both match, including `+0x8c`, and
the factor must be positive. The warm HDR emission is artistic and scaled by
that factor; the flame does not cast opaque shadows. A small screen-space
glow gathers only visible emissive HDR pixels before tonemapping. It is
limited to the projected bounds of emissive draws plus a margin; if the
bounds cross the camera plane, it conservatively uses the full viewport.
Fully occluded emission produces no glow; visible glow can spill over foreground
edges. This is an artistic approximation, not measured TDS bloom; its live
cost has not been measured.
No earlier transform or
light is reused. A zero actor factor without a listed light keeps the dim
ambient and is labelled `preview_candle_zero_candidate`, not verified off.
If an actor-link observation conflicts with a listed light, the viewer uses
the unknown-light fallback instead of rendering the conflicting light value.
`preview_flame_draws` and `preview_candlestick_draws` count the additional
draws. The geometry, shader and zero label were checked offscreen; the new
candlestick, flame and zero label have not been checked in TDS. The live [link report](../../build/t3render-captures/scene-live-m3-candle-link-20261118-mac-v1/M3-link-report.md)
records the earlier actor-link experiment and synchronized dim fallback. An
[offscreen reconstruction](../../build/t3render-captures/scene-live-m3-glow-replay-20261118-v1/M3-glow-report.md)
uses a captured bedroom pose, showing the generated candlestick and flame;
it also documents its synthetic link sample, artistic calibration and simulated zero state.

The [same captured pose](../../build/t3render-captures/scene-live-m3-crate-analysis-20261118/analysis.md)
also contains a submission for `D_1224__21` whose observed transform and
offline candidate mesh project onto the crate underneath the candlestick.
The generic live sampler only accepts `StaticMeshActor`, so it does not select
this object. The projection does not establish the crate's runtime asset binding
or visibility under occlusion; a stored offline pose cannot track gameplay.

The embedded geometry is derived from the generated project asset, not from
a game binary. To regenerate both headers after changing that asset:

```sh
python3 -B t3render/tools/export_live_candle_flame.py \
  build/assets/godot/remaster/garrett_home/highpoly/generated/candlestick_high.glb \
  t3render/src/scene_live_candle_flame_geometry.hpp \
  --candlestick-output t3render/src/scene_live_candlestick_geometry.hpp
```

Host tests compared 16 BSP sections from captured SceneIR frame 2864 with the
existing offline selector (67 selected triangles). Highball fixtures tested
UV interpolation, switching, missing-UV fallback and Reset; an offline
45-block catalog with a synthetic camera exercised the real textures through
the GPU and viewer worker. The fixture selected all catalog blocks itself;
it is not a synchronized live frame or a performance test. TDS integration
and the reported lag still require a later game session.

The experimental combined proxy can opt in with
`T3RENDER_EXPERIMENTAL_COMBINED=1`, `T3RENDER_SCENE_LIVE=1` and
`T3RENDER_LIVE_CATALOG` pointing to the local catalog. Initialization and the
verified-build hook installation happen outside `DllMain`; failed setup yields
no live output. `SceneDriver` connects probe, submission scopes, draw samples,
Present boundaries and Reset events to `SceneResidency`. It rejects unsafe
overlap or reentry and publishes at most a candidate partial snapshot through
`RuntimeCurrent()`. The default adapter uses pass 0/stage 1 and supports the
researched bed and offline-matched BSP. The optional v4 static mesh path below
adds bounded candidates but has not been exercised in TDS. Map changes and
general resource lifetimes are not yet observed. The
separate visible-actor map witness is a candidate, not a map lifecycle hook.
If a structurally valid target or visible-actor witness contradicts the loaded
catalog on a selected, stable probe, the driver immediately discards the
current snapshot and the partial Present interval, then requires a clean
boundary before rebind. An incomplete witness remains unknown. The
`map_witness_conflicts` telemetry counter records these conservative
invalidations. They do not establish that a map change occurred, reload a
catalog, detect same-map reloads, or cover changes without an observable witness.
It must not be presented as a general scene bridge or enabled in a game session
as a verified feature.

For an adapter that has a separately established map lifecycle signal,
`SceneDriver::BeginMapTransition` immediately retires the current snapshot and
continues tracking outstanding callbacks while the old window is disabled.
`CommitMapTransition` accepts a separately loaded, unused catalog only after
those callbacks and any Reset have drained. It validates the catalog's map and
resource handles, atomically adopts its immutable resources into the existing
residency, advances map and device epochs, clears identity tracks and requires
a fresh Present boundary. An invalid catalog, stale epoch, failed Reset or
undrained callback leaves the transition pending and the old snapshot retired.
The prepared residency is consumed; old snapshot holders cannot submit it.
This also handles an explicitly signalled reload of the same map.

The viewer can request overrides and material assets for the exact current
snapshot. When the material library changes, its worker rebuilds the D3D12
backend; the old GPU cache and stale bed override are not reused. A synthetic
host fixture checks same-map and different-map adoption, in-flight callbacks,
Reset, asset handles and old-snapshot rejection. A synthetic Highball fixture
checks the worker's same-map reload and material-library replacement. The
runtime adapter does **not** yet detect or forward a TDS map lifecycle event,
load a replacement catalog, or update its optional light sampler's map context.
Versions 1–4 and 6 require the researched bed and a BSP catalog; version 5 below
supports scenes without either, but the different-map runtime fixture remains
synthetic and does not demonstrate a TDS map transition.
Consequently this API is not a live map-change implementation or proof of a
same-map reload in the game.

Host fixtures cover catalog round trips and corruption, candidate binding,
window publication, overlap, Reset and rebind. The Windows loader and driver
fixtures passed on Highball without TDS; the loader read a local catalog with
45 BSP blocks and a 257-vertex bed, and rejected a corrupted copy. These tests
do not exercise the real hook callbacks or prove that a runtime frame is
produced in TDS.

## Optional static mesh candidates

Add `--static-meshes` to `export_live_catalog.py` to create a v4 catalog. It
includes eligible `StaticMeshActor__N` identities, referenced mesh geometry and
per-section material bindings. It never exports actor transforms or asserts
visibility. Resource IDs are qualified by map and offline bundle/kernel
fingerprints; matching names from different offline sources are excluded as
ambiguous. Missing material bindings, unsupported section/geometry sizes and
unqualified identities are also excluded and listed in the manifest. Versions
1–3 remain readable. The local `SouthQuarter1_int1` export contains 325 actor
candidates and 46 extra resources (plus the bed); five actors with unresolved
materials are excluded. Other actor classes and attachments are not covered.

For maps without the researched bed, `--general-map` exports a v5 catalog
containing BSP blocks and eligible generic static mesh candidates. Either
collection may be empty, but at least one must contain geometry. The format
does not carry a special bed binding, high-poly bed override or bedroom
material library: actor number 1520 is an ordinary generic candidate. It
includes candidates without requiring `--static-meshes`, while runtime sampling
remains opt-in. Unqualified actors and missing materials are excluded and
reported, not guessed. The v1–v4 bytes and readers remain supported.

For example, from the SDK root:

```sh
python3 -B t3render/tools/export_live_catalog.py /path/to/offline.scene.json \
  --general-map --output /path/to/new/catalog.bin
```

The exporter still requires `source=offline`; neither v5 nor its asset keys
are runtime visibility, transforms or package fingerprints. Host tests cover
empty collections, candidate identity, malformed data and transitions into a
bedless catalog. A BSP-only catalog does not solve the runtime map-witness
requirement: without a suitable visible static actor the bridge may still
reject its BSP selections. The Windows loader accepted two deliberately modified offline
`SouthQuarter1_int1` subsets: one retained 45 BSP blocks and 324 eligible actors,
the other removed BSP and retained the same 324 candidates. It rejected a
corrupted copy of each. These are synthetic subsets of an offline fixture, not
observations of another TDS level. The generic matcher and lifecycle signal
still need real-game validation.

With a v4 or v5 catalog, explicitly set `T3RENDER_SCENE_LIVE_STATIC_MESHES=1` to
enable the new path. The bedroom batch runner clears inherited `T3RENDER_`
variables, so its equivalent is `--live-static-meshes` together with
`--live-catalog`; it rejects catalog headers other than v4/v5 before starting a game.
It samples one candidate inside its existing submission
scope and requires a unique visible-list name, owner/table/class/map checks,
local asset-property audit, mesh-node counts/bounds/sections and material
agreement with the offline candidate. The scope's current matrix supplies the
transform; pre/post submission identity and asset checks plus the selected draw
and `Present` window gate publication. The result is still `Candidate` with
`VisibleList` identity scope: resource-manager fingerprints, GPU payloads,
global uniqueness, full lifetime coverage and ABA freedom are not proven.
Names and fingerprints are semantic values; transient pointers stay in-process.

Identity generations advance after a visibility gap, identity change, loss or
Reset; ordinary transform changes retain the generation. A missing mesh means
only no accepted submission in this selected scope, not actor destruction.
`static_mesh_samples`, `static_mesh_scopes` and `static_mesh_rejected` in the
viewer telemetry show attempted/accepted/rejected scope samples.

This path performs expensive bounded table, asset and mesh-pool reads per
candidate scope. In the same in-memory fixture with 1,000 unrelated objects,
the complete generic scope used 428 reads before the probe optimization and
359 afterwards. Two class/outer membership sweeps are now combined while
retaining independent slot and stability checks. A synthetic 16,005-slot table
still needs five complete sweeps (630 reads of 128-slot chunks, including the
last partial chunks) per generic observation, down from seven before the
change. These are read counts, not frame-rate results. The path stays off by
default; live coverage and the reported lag require a later coordinated TDS test.
Host fixtures reject ambiguous identity/materials and exercise transform,
visibility, reappearance and Reset. A Highball GPU fixture used offline
`StaticMeshActor__258 / GENrafters1` geometry with a synthetic camera and
selection; it is not proof that the runtime actor was observed.

## Opt-in D3D12 live viewer

With the previous flags and `T3RENDER_SCENE_LIVE_DX12=1`, the experimental
combined proxy starts a separate worker and a non-activating D3D12 window.
Its device, queue, pipeline and swapchain persist across frames. Immutable
mesh/BSP position, UV and index buffers are cached by semantic resource identity;
draws supply per-instance transforms and material IDs. The window displays a flat
or partially textured **partial candidate selection**, not a full bedroom or
a production renderer. The worker takes the latest available snapshot and may
drop superseded frames; no real-time performance or every-frame coverage is
claimed. Window closing affects only the viewer.

`SceneResidency::SubmitIfCurrent` serializes GPU submission against snapshot
invalidation, Reset and map epoch changes. The worker prepares commands
outside the lock, submits only if that exact snapshot remains current, and
waits for the GPU and calls DXGI `Present` outside the lock. A newer frame in
the same continuity is allowed, so a faster producer does not starve the
viewer; a loss, Reset or map change breaks continuity. Invalidation can still
race the final display check: a stale image may briefly appear until the
viewer clears it. A pending GPU failure disables the viewer and
retains potentially in-flight resources instead of terminating the game.
The DXGI and D3D12 libraries are loaded only when the viewer is requested.

An optional `T3RENDER_SCENE_LIVE_DIAGNOSTIC_DIR` names an existing local
directory. Each process generates a random live session ID and uses it in the
`scene-live-<session>.jsonl` filename, so separate TDS processes cannot silently
share the first process's log. The log records frame IDs, pass/stage, epochs,
camera matrices, selection and triangle counts, and elapsed milliseconds after
the viewer's successful Present. Differences between those timestamps measure
viewer-frame intervals, not GPU time or the original game's frame time. The
log is bounded to 32,768 records. Up to two initial PPM images use the same
session-qualified prefix; when the D3D8 capture marker appears, up to eight
additional viewer frames are read back. A matching frame still has to be
verified using the shared Present link below. These files contain scene-derived information and
must remain local. The diagnostic reads back the viewer's D3D12 output, not
the original game's GPU buffers.

With `T3RENDER_SCENE_LIVE_GPU_METRICS=1` and an open diagnostic log, the viewer
also requests D3D12 timestamp queries at most every 250 ms. The bedroom runner
enables this only with `--gpu-metrics`.
`gpu_frame_ms` measures the viewer command list, including its image copy when
`gpu_frame_includes_readback` is true, and excludes DXGI Present and the
game's D3D8 commands. Three consecutive ordered, nonzero timestamp pairs are
required before reporting a value; a failed map or invalid pair stops sampling
and reports null. `gpu_timing_status` records availability or warmup. Separate
single-entry query heaps are used because the tested Highball runtime returned
zero for the second query in a shared heap. Even with separate heaps, the
Highball fixture has intermittently returned a zero end timestamp, so a
synthetic success is not proof that the live measurement is supported.
`video_memory_usage_bytes` is one DXGI local-segment usage estimate requested
about two seconds after the first frame; `video_memory_sample_age_ms` records
its age. It can be unavailable and is not an isolated measure of the viewer.
`resident_bytes` and `material_gpu_bytes` are the separately accounted geometry
and material allocations, not total GPU memory. A live run with GPU diagnostics
and periodic memory queries was much slower than the previous run, although the
cause was not isolated. The current single-sample version is not yet tested in
TDS; keep the feature disabled during normal gameplay.

Driver telemetry also records callback counts pending at the most recent
Present entry. Nonzero counts identify overlap but do not prove that an old
callback has ended or is safe to discard.

When capture and live output run together, the trace header records the same
session ID. The combined observer adds `scene_live_frame` to a successfully
tracked trace frame from the actual shared native `Present` call. Frame numbers
alone and elapsed time are not synchronization evidence. The following command
checks the session, explicit Present link, synchronized trace window, pass/stage,
camera matrices (with signed zero normalized) and viewport against a successful
same-thread draw in that frame. It does not establish complete callback coverage,
semantic bindings or pixel parity:

```sh
python3 -B t3render/tools/compare_live_present.py /path/to/trace.jsonl /path/to/scene-live-session.jsonl
```

The comparator streams up to 32,768 viewer events and 64 MiB, retaining only
frames linked from the trace. It rejects incomplete or changing logs, duplicate
frame IDs and a linked frame whose session differs from the trace.

Earlier logs have neither the session identity nor the direct Present link.
In the second local TDS attempt, the Wine log records multiple T3Main launches
and the single old viewer log reports zero probes while the trace reports six;
the files cannot establish whether they came from the same process. A later
[real TDS capture](../../build/t3render-captures/scene-live-m1-20261117-001712/M1-report.md)
matched three consecutive frames. The recorded camera direction changes by
5.20° and 8.79° between them, and the [image sequence](../../build/t3render-captures/scene-live-m1-20261117-001712/comparison-frames-967-969.png)
shows the corresponding partial views. This covers only 775 ms between viewer
Presents; walking and the required 30–60 seconds of synchronized movement
remain unverified.

A [subsequent BSP texture comparison](../../build/t3render-captures/scene-live-m1-bsp-texture-20261118-v1/M1-bsp-texture-report.md)
projects candidate wall and floor geometry and textures on the CPU for those
same poses. On regions whose material color agrees with the flat viewer, the
unshifted image gradients correlate with D3D8 more strongly than almost all
tested shifts; a vertically flipped texture is a negative control. This does
not identify individual BSP blocks or prove GPU texture bindings.

An earlier [live viewer sequence](../../build/t3render-captures/scene-live-m1-20261116-234934/M1-report.md)
contains 2,290 consecutive frames with the same continuity and epochs over a
bounded 47.497–49.504 seconds. The camera changes and the bed candidate is
selected, then absent, then selected and absent again. Its estimated 46.24–48.19
viewer presentations per second includes instrumentation. That older sequence
has no same-pose D3D8 comparison, so it does not establish continuous
synchronization, actual bed visibility, or that the player remained in the
bedroom while walking. Its recorded camera crosses an opening in the offline
BSP geometry after 22.2–24.2 seconds and continues into the adjacent area;
the offline geometry does not prove the corresponding runtime player path.
In that sequence the candidate bed is absent from 2,034 viewer frames: its
offline bounding box is outside the camera frustum in 1,983, while offline BSP
intersects every in-frustum bed-vertex ray in the other 51. This is consistent
with possible occlusion, not a verification of runtime visibility or pixels.

In a later [isolated-save manual load](../../build/t3render-captures/scene-live-manual-20261117-mac/M1-report.md),
the user reached the bedroom but the viewer published no frames: one draw was
still pending at every sampled Present and the driver remained in
`WindowNeedsRebind`. The user reported severe stutter; an instrumented D3D8
interval measured approximately 18.4 FPS. Repeated losses now invalidate an
already-retired window only once, while still clearing driver state and
tracking outstanding callbacks. Telemetry records the oldest pending draw's
kind, thread relation and age in Presents; this does not establish that an
old callback may be discarded. The optimization and the callback's cause
remain unverified when that condition recurs.

In the [next isolated live run](../../build/t3render-captures/scene-live-manual-20261117-mac-v2/M1-report.md),
943 consecutive viewer frames covered 47.112 seconds of requested movement in
the bedroom, with no recorded losses in that interval. D3D8 Present cadence
was approximately 20.1 FPS with the proxy and viewer active. The session then
had two device resets, with viewer-frame gaps of 12.057 and 5.756 seconds; the
user had switched apps, but the cause is unproven. Three later frames matched
the D3D8 Present and camera, including a [same-pose comparison](../../build/t3render-captures/scene-live-manual-20261117-mac-v2/confronto-frame-3001.png).
The viewer was mostly covered, and continuous D3D8 agreement remains unverified.

In the [subsequent M1 capture](../../build/t3render-captures/scene-live-m1-translation-20261118-v1/M1-translation-report.md),
five consecutive viewer images show the candidate bed changing as the recorded
camera translates 31.01 T3 units without rotating over 159 ms. Their bed-color
masks overlap a CPU projection by 99.93–99.95% IoU, assuming the captured bed
transform remains fixed. Only the first image is paired with D3D8; this does
not establish continuous synchronization or BSP correspondence.

In the later M1 capture, a [separate projected-bed check](../../build/t3render-captures/scene-live-m1-bed-projection-20261118-v1/M1-bed-projection-report.md)
covered 48,186 of the 48,209 viewer pixels classified as bed in synchronized
frame 2730. It uses the same candidate offline mesh and captured camera, with
no independent depth or runtime asset proof; it does not establish continuous
agreement during movement.

Offline analysis of that capture's frame 967 matched 28 additional submitted
actors to offline mesh candidates. Twenty are `StaticMeshActor` entries also
present in the generated v4 catalog; the remaining eight use other classes and
are not covered by the current generic live sampler. A local offscreen D3D12
comparison renders the same captured camera with the bed and nine BSP blocks
(450 triangles), then adds those 20 candidates (2,401 triangles) or all 28
(3,447 triangles). The additional actor transforms and scoped submissions were
observed, but their resource bindings and geometry remain offline candidates.
The previews use a different diagnostic renderer from the live viewer, with
its own culling and flat palette; they do not prove live visibility, frame
rate, or visual parity. The [local comparison](../../build/t3render-offline/m1-frame-967-confronto.png)
and its scene/packet manifests stay under `build/`. The separately generated
v4 catalog `SouthQuarter1_int1-m1-static.livecatalog` contains no bed or
material overrides; its opt-in sampler and potential performance cost still
require a coordinated game test. The [offline report](../../build/t3render-offline/M1-offline-frame-967.md)
also records the bounded analysis of the later load crash.

For non-capture frames, the live probe avoids scanning every UObject by name;
other passes use a minimal sample. The stronger full probe is still used when
a capture is active. In a synthetic registry with 1,000 unrelated objects,
the full, live-bed and live-no-bed paths required 1,409, 346 and 243 reads
before the optimization and 1,354, 295 and 235 afterwards, respectively. This is a
read-count comparison, not a measured frame-rate improvement. The expensive
probe and DXGI presentation inside the scene lock are plausible sources of the
lag reported in the two TDS attempts; their individual costs remain unmeasured.

The default geometry cache budget is 128 MiB, accounted using D3D12 buffer
allocation sizes. When it fills, least-recently-used resources outside the
current selection are evicted after prior GPU work completes; if the current
selection itself exceeds the budget, the viewer fails closed. Resources are
retired on loss, including a missed empty interval, and recreated for a new
map/device epoch or continuity. No engine or D3D8
pointer is used as a cache key. The persistent backend supports both the flat
diagnostic path and the limited direct-light HDR pass described below.

The x86 Highball fixture exercised an offscreen target and a hidden swapchain:
the resident path matched the expanded path pixel-for-pixel for synthetic camera,
transform and visibility changes. Reset, continuity breaks, budget exhaustion
and eviction also passed. A worker fixture checked stale-snapshot rejection,
reset clear and retirement when the first snapshot is invalidated before or
after submission. Those fixtures are synthetic and do not establish TDS behavior.

## Light observations and frame coherence

The in-process model can carry `VisualLight` values separately from geometry.
Every field has independent `Unknown`, `Candidate` or `Verified` evidence:
kind, position, linear color, intensity, influence radius, source radius in
meters, direction, inner/outer cone cosines and enabled state. A missing value
is unknown, including enabled. The influence radius uses scene coordinates;
source radius is a separate emitter-size parameter in meters. Neither an
intensity nor a radius is automatically physically calibrated. Light IDs are
semantic values, not engine addresses, and generations must not go backwards
within a map epoch.

`LightCoverage` distinguishes an unobserved collection from a partial sample
and a collection asserted complete within the producer's selected scope,
including an explicitly empty one. `SceneWindow` and `SceneDriver`
provide `BeginLightSample` / `EndLightSample`: the full read is bracketed on
the selected probe's thread between consecutive `Present` boundaries. A sample
that overlaps `Present`, Reset or another in-flight operation invalidates the
interval. Repeated samples in the same window must agree. Stability, coverage
and field evidence are still claims supplied by the adapter, not facts the
collector can independently verify. The opt-in `T3RENDER_SCENE_LIVE_LIGHTS=1`
adapter can sample candidate actors from the selected renderer list after the
probe; it publishes only `Partial` coverage and candidate positions. A
[bedroom run](README_scene_light_probe.md) exercised it in TDS without
establishing the physical meanings of its raw fields. Without opt-in or when the sample is unavailable, live
snapshots retain `Unknown` coverage. The bounded list, identity limitations
and static evidence are described in [the light probe](README_scene_light_probe.md).

`BuildVerifiedLighting` accepts only complete coverage and fully verified
enabled lights, omits only lights whose disabled state is verified, and
returns no partial result when any selected light is incomplete. It derives
the camera position from the same row-vector view matrix, converts influence
radii using the declared unit scale and retains frame, epoch, provenance,
camera and source observations. `LightingMatchesSnapshot` prevents reuse for
a different snapshot or changed observations. This is a renderer input, not a
new runtime light measurement. Viewer diagnostics record coverage and counts
by readiness; they do not promote candidate data.

Host fixtures exercise field evidence, point/spot completeness, disabled and
unknown states, map generations, radius units, camera inversion, stale frame
rejection, bracketing and overlap recovery. These tests use synthetic values;
the nine previously identified candidate lights are not upgraded by them.

## Persistent direct-light PBR/HDR experiment

When `BuildVerifiedLighting` accepts the current snapshot, the worker builds a
resident frame with the same session, map, frame ID, epochs, pass/stage, view,
projection and viewport (the output is cropped to that viewport),
then prepares a direct-light pass. The backend repeats the evidence and camera
checks before recording commands. Submission still uses `SubmitIfCurrent`, so
the exact snapshot must remain current at enqueue. Unknown, partial or
incomplete lighting leaves the existing flat/albedo path selected. A complete
empty light set produces no direct light. A producer's `Verified` and
`Complete` flags remain attestations, not independent proof.

The pass uses an `RGBA16F` scene-linear target, dielectric GGX/Schlick shading,
point and spot lights, inverse-square falloff with finite source radius and a
smooth influence cutoff, then ACES-like tone mapping and sRGB output. Intensity
values have not been calibrated to physical units in TDS. Instance normals use
the inverse transpose. Missing or zero normals and singular transforms fall
back to diagnostic flat shading within the HDR pass; other untextured materials
use material-ID colors as placeholders. Culling remains disabled. There is no
ambient/indirect term, GI or exposure control in this path.

The persistent direct-light pass builds radial `R32_FLOAT` cube shadow maps for
up to the first four accepted lights, in the order supplied by the current
snapshot; additional lights remain unshadowed. Each map has six 256×256 faces.
It renders only the selected SceneIR sections using their current transforms;
those triangles are treated as opaque. A bounded blocker search estimates a
penumbra from the verified source radius and blocker/receiver distances, then
a 25-tap filter and geometric-normal bias soften the result. This is an
approximation: missing or off-screen casters, translucent surfaces, finite map
resolution, sparse blocker samples and bias can create incorrect shadows. It
does not establish the game's original shadowing or fix the beam.

To bound preparation, if six face passes for the selected lights would exceed
16,384 indexed draw calls, shadows are disabled for that entire frame and direct
lighting continues. The viewer reports `shadowed_lights`, `shadow_draws` and
`shadow_budget_exceeded`; no missing light is represented as shadowed. Shadow
resources are allocated on the first eligible frame and the map is cleared for
each light/face before use. A canceled, superseded or reset snapshot cannot
contribute a cached shadow image to a later frame. These limits cap work but
are not a real-time performance guarantee.

Albedo is decoded through an sRGB SRV; roughness is linear. Normal maps use an
explicit derivative-based +Y convention **only for synthetic fixtures**; TDS's
tangent convention is unverified. AO is uploaded but does not darken direct
light: it needs an indirect term first. Extra material channels and the HDR
target are created on first accepted PBR frame rather than on a flat-only
session. Shader and pipeline setup still occurs during viewer initialization;
neither its startup cost nor runtime performance has been measured in TDS.

The x86 Highball fixture uses synthetic camera, geometry and lights to check a
numeric GGX sample, HDR values above one, point attenuation, spot direction,
normal inverse transpose under non-uniform scale, material sRGB/roughness,
fixture-only normal maps, stale and partial rejection, worker fallback,
visibility, resize and Reset. The offline catalog fixture also exercises the
existing assets with a synthetic camera. None of these proves TDS light state,
live frame delivery, physical calibration, a correction to the beam, or that
the reported lag has improved. The current TDS hook emits no light samples,
so it cannot yet activate this pass in a real game frame.

The shadow fixture adds a synthetic point and spot light, a movable/hidden
blocker and a receiver. It checks that shadowed and clear pixels differ as
expected, that the soft edge changes with source size and blocker distance,
that multiple lights, the four-light cap, work-budget fallback, cancellation
and Reset do not reuse stale maps. These checks are offscreen GPU fixtures,
not observations in TDS.
