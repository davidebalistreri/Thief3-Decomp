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
prove callback ordering, visibility or the Present interval. The current map
check depends on the target bed appearing in the sampled list: when it does
not, this matcher declines BSP as well. Removing that limitation needs a
separately observed map/lifetime signal, not reuse of the last seen actor.

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

The experimental combined proxy can opt in with
`T3RENDER_EXPERIMENTAL_COMBINED=1`, `T3RENDER_SCENE_LIVE=1` and
`T3RENDER_LIVE_CATALOG` pointing to the local catalog. Initialization and the
verified-build hook installation happen outside `DllMain`; failed setup yields
no live output. `SceneDriver` connects probe, submission scopes, draw samples,
Present boundaries and Reset events to `SceneResidency`. It rejects unsafe
overlap or reentry and publishes at most a candidate partial snapshot through
`RuntimeCurrent()`. The adapter
uses pass 0/stage 1 and supports the researched bed and offline-matched BSP
only. Map changes and general resource lifetimes are not yet observed; the BSP
matcher also requires the bed in the sampled list. It must not be presented as
a general scene bridge or enabled in a game session as a verified feature.

Host fixtures cover catalog round trips and corruption, candidate binding,
window publication, overlap, Reset and rebind. The Windows loader and driver
fixtures passed on Highball without TDS; the loader read a local catalog with
45 BSP blocks and a 257-vertex bed, and rejected a corrupted copy. These tests
do not exercise the real hook callbacks or prove that a runtime frame is
produced in TDS.

## Opt-in D3D12 live viewer

With the previous flags and `T3RENDER_SCENE_LIVE_DX12=1`, the experimental
combined proxy starts a separate worker and a non-activating D3D12 window.
Its device, queue, pipeline and swapchain persist across frames; geometry is
still CPU-expanded and uploaded every frame. The window displays a flat
material-ID view of the **partial candidate selection**, not a full bedroom or
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
directory. The worker creates `scene-live.jsonl` with frame IDs, epochs,
camera matrices and selection counts; it writes at most two PPM images, the
first frame and the first with a changed view. These files contain scene-derived
information and must remain local. Frame IDs originate from the worker's
Present-delimited producer; equality with a capture trace must be established
for the same device and session, not inferred from time. The diagnostic reads
back the viewer's D3D12 output, not the original game's GPU buffers.

For non-capture frames, the live probe avoids scanning every UObject by name;
other passes use a minimal sample. The stronger full probe is still used when
a capture is active. In a synthetic registry with 1,000 unrelated objects,
the full and live paths required 1,384 and 322 reads respectively. This is a
read-count comparison, not a measured frame-rate improvement. The expensive
probe and DXGI presentation inside the scene lock are plausible sources of the
lag reported in the two TDS attempts; their individual costs remain unmeasured.

The x86 Highball fixture exercised an offscreen target and a hidden swapchain:
camera movement, depth ordering, visibility, empty selection, resize and Reset
passed. A worker fixture also checked stale-snapshot rejection and reset clear.
Those fixtures are synthetic and do not establish TDS behavior.
