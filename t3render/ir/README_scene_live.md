# Resident semantic scene: experimental in-process adapter

`scene_live.hpp` and `scene_live.cpp` provide a C++ value model for a future
T3SceneBridge consumer. They are built as `t3render_scene_live` on Win32. An
experimental opt-in adapter now connects the existing probe and D3D8 facade
to the window and driver described below. An experimental D3D12 viewer can
consume its current snapshot in the same process; two TDS attempts initialized
the viewer but produced no logged viewer frame, and the user reported severe
lag. A `synchronized` flag alone is an
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
an unobserved Reset. A scope without a draw is not selected; without any
verified draw viewport the interval is rejected. The experimental adapter's
callback coverage in TDS is not yet verified.

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

In the existing synchronized capture, frame 1243 contains no visible bed but
does contain other static actors and VBSP index 7. Its metadata uniquely
matches offline GMP block 5, including counts, bounds and material sections;
draw 10 is within the observed BSP submission. That old trace did not capture
the other actors' outers, so it cannot validate the new witness or prove the
map. The new path and disappearance/reappearance behavior are synthetic-only
until another TDS test.

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
no PBR lighting or gameplay changes are applied.

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
materials also remain ID colors. Normal, roughness and AO are included for a
future PBR consumer but are not sampled yet; this is an **unlit** albedo
preview, not HDR lighting, PBR or a measurement of the game's materials.
Diagnostics report `bedroom_material_runs` and `textured_draws`; a difference
can indicate missing UVs. The separate `T3RENDER_SCENE_LIVE_BEDROOM_MATERIAL_IDS=1`
flag enables only the partition/ID diagnostic with an older catalog. A v3
catalog enables the material selection in the viewer automatically.

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
`RuntimeCurrent()`. The adapter
uses pass 0/stage 1 and supports the researched bed and offline-matched BSP
only. Map changes and general resource lifetimes are not yet observed. The
separate visible-actor map witness is a candidate, not a map lifecycle hook.
It must not be presented as a general scene bridge or enabled in a game session
as a verified feature.

Host fixtures cover catalog round trips and corruption, candidate binding,
window publication, overlap, Reset and rebind. The Windows loader and driver
fixtures passed on Highball without TDS; the loader read a local catalog with
45 BSP blocks and a 257-vertex bed, and rejected a corrupted copy. These tests
do not exercise the real hook callbacks or prove that a runtime frame is
produced in TDS.

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
camera matrices and selection counts; at most two PPM images use the same
session-qualified prefix. These files contain scene-derived information and
must remain local. The diagnostic reads back the viewer's D3D12 output, not
the original game's GPU buffers.

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

Earlier logs have neither the session identity nor the direct Present link.
In the second local TDS attempt, the Wine log records multiple T3Main launches
and the single old viewer log reports zero probes while the trace reports six;
the files cannot establish whether they came from the same process. The new
isolation and comparison have synthetic coverage but have not yet been tested
in TDS.

For non-capture frames, the live probe avoids scanning every UObject by name;
other passes use a minimal sample. The stronger full probe is still used when
a capture is active. In a synthetic registry with 1,000 unrelated objects,
the full, live-bed and live-no-bed paths required 1,409, 346 and 243 reads
respectively in the updated fixture. This is a
read-count comparison, not a measured frame-rate improvement. The expensive
probe and DXGI presentation inside the scene lock are plausible sources of the
lag reported in the two TDS attempts; their individual costs remain unmeasured.

The default geometry cache budget is 128 MiB, accounted using D3D12 buffer
allocation sizes. When it fills, least-recently-used resources outside the
current selection are evicted after prior GPU work completes; if the current
selection itself exceeds the budget, the viewer fails closed. Resources are
retired on loss, including a missed empty interval, and recreated for a new
map/device epoch or continuity. No engine or D3D8
pointer is used as a cache key. This is a flat diagnostic backend: texture,
material and GPU upload residency for the PBR renderer remain separate work.

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
collector can independently verify. The current TDS hook supplies no light
samples, so live snapshots retain `Unknown` coverage.

`BuildVerifiedLighting` accepts only complete coverage and fully verified
enabled lights, omits only lights whose disabled state is verified, and
returns no partial result when any selected light is incomplete. It derives
the camera position from the same row-vector view matrix, converts influence
radii using the declared unit scale and retains frame, epoch, provenance,
camera and source observations. `LightingMatchesSnapshot` prevents reuse for
a different snapshot or changed observations. This is a renderer input, not a
new runtime light measurement or a PBR implementation. Viewer diagnostics
record coverage and counts by readiness; they do not promote candidate data.

Host fixtures exercise field evidence, point/spot completeness, disabled and
unknown states, map generations, radius units, camera inversion, stale frame
rejection, bracketing and overlap recovery. These tests use synthetic values;
the nine previously identified candidate lights are not upgraded by them.
