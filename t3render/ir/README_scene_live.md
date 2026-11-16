# Resident semantic scene: first in-process building block

`scene_live.hpp` and `scene_live.cpp` provide a C++ value model for the future
T3SceneBridge consumer. They are built as `t3render_scene_live` on Win32. This
library is **not yet connected to the TDS hooks or a live D3D12 device**. Its
`synchronized` flag is an assertion supplied by its producer, not proof of a
Present window. The existing JSON SceneIR schema has not changed.

Resources contain immutable geometry and sections, a semantic ID and the
fingerprint of their source asset. The caller must establish both the key and
fingerprint; the cache does not hash or authenticate the geometry. Materials
and per-instance bindings are separate. Each selected mesh includes an
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
5. The GPU consumer must invalidate/recreate resources on device epoch change
   and synchronize its epoch check with submission. An already-retained old
   snapshot can remain alive but must not be submitted after invalidation.

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

The adapter must bracket every relevant operation, report lost hooks/reentry,
validate its own scope identity and sampling, catch exceptions without exposing
them to the game, and recreate the collector after map/device epoch changes.
The collector cannot independently prove callback coverage, a live binding or
an unobserved Reset. A scope without a draw is not selected; without any
verified draw viewport the interval is rejected. The collector is not yet
connected to the game or a graphics device.

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
python3 -B -m unittest t3render.tests.test_scene_live t3render.tests.test_scene_live_window
cmake --build build/t3render-mingw --target t3render_scene_live
```

A local check also loaded the captured SceneIR selections for frames 2864/2865
and revalidated their common trace with `scene_trace_session.validate`. For the
bed sections 1/2 and GMP block 5 sections 0/1, the flat adapter produced 798
vertices per frame; all transformed positions matched the independent SceneIR
calculation within 0.000123 T3 units and camera matrices matched. These are
captured inputs, not a live consumer or a GPU image; generations were assigned
for the fixture rather than observed as runtime lifetimes.
