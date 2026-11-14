# T3Render: D3D8 diagnostic capture

T3Render is a separate renderer experiment. Its first step observes the
original D3D8 device; it does not replace the renderer or change the game's
stealth calculations. A separate DX12 replay covers synthetic single- and multi-draw
fixtures, including a fixture-owned indexed buffer snapshot; this is not a
general TDS frame replay or production backend.

## Build

Use CMake and an x86 MSVC or MinGW-w64 toolchain containing `d3d8.h` and
`dxguid`. In an x86-capable Visual Studio environment:

```powershell
cmake -S t3render -B build/t3render -A Win32
cmake --build build/t3render --config Release
```

The output is an x86 `d3d8.dll`. A Windows SDK may not include the legacy
header; use an installed DirectX SDK where necessary. No SDK is downloaded by
this project. To cross-compile with an installed i686 MinGW-w64 toolchain:

```sh
cmake -S t3render -B build/t3render-mingw -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++
cmake --build build/t3render-mingw
```

### Experimental combined observer

The normal proxy remains the default. Build the separate experimental facade
with `-DT3RENDER_BUILD_EXPERIMENTAL_COMBINED_PROXY=ON` and target
`t3render_experimental_combined_proxy`; its DLL is in
`build/t3render-mingw/experimental_combined_proxy/`. Both a valid capture
configuration and `T3RENDER_EXPERIMENTAL_COMBINED=1` are required to activate
it. Set `T3RENDER_CAPTURE_BUFFERS=1`, `T3RENDER_CAPTURE_DECLARATIONS=1`,
`T3RENDER_CAPTURE_VERTEX_PROGRAMS=1` and
`T3RENDER_CAPTURE_VERTEX_CONSTANTS=1` to request the combined observations.

This is a deliberately experimental COM boundary: unsupported interfaces,
resource aliases, reset, private data, and runtime concurrency can change game
behavior or invalidate evidence. A known state-block epoch begins at device
creation; shader deletion and unrecognized state-block operations invalidate
observations rather than trusting a reused handle. Shader deletion alone
invalidates shader evidence without discarding independent buffer shadows.
Reset retires the state-block epoch.
The trace is always marked non-replayable, and records unsafe or unavailable
data rather than presenting it as confirmed. Texture contents are not captured.
The proxy does not alter stealth data.

For the hidden synthetic Highball smoke test and the limited DX12 replay, build
the D3D8 UP fixture and DX12 replay targets too, then run
`tools/run_synthetic_up_replay.py` with `--buffered --proxy-buffer-replay
--experimental-combined` and the engine, bottle, build and new output paths.
This checks one FVF triangle; the programmable-shader synchronized fixture is
documented in [the resource facade notes](tests/README_resource_facade.md).
Neither synthetic result establishes TDS compatibility or room replay.
The [Highball result](benchmarks/d3d8/2026-11-14-experimental-combined-proxy/result.json)
records the tested binary and matching RGB hashes.

A subsequent [TDS bedroom capture](benchmarks/runtime/2026-11-14-tds-combined-observer/result.json)
completed one frame but had no stable VB/IB windows. The earlier trace labeled
prepared getters `getter_failed` even if a missing draw ticket meant they were
never attempted; independent native binding getters succeeded. New traces mark that case
`window_unavailable` and record the first known buffer invalidation site and
cause, without restoring or trusting invalidated observations. A user also
reported truncated menu labels with the experimental facade; its cause remains
unverified. The temporary DLL was removed after the test.

A [second bedroom capture](benchmarks/runtime/2026-11-14-tds-buffer-window/result.json)
located the first invalidation in the observer's `BeginUnlock` consistency
check. This did not identify the subcondition or native result. The diagnostic
now records those details as observations while retaining fail-closed behavior;
the native Unlock HRESULT is a candidate if calls overlap.
A [third capture](benchmarks/runtime/2026-11-14-tds-unlock-mismatch/result.json)
found no active tracked lock on the affected vertex buffer; both its last
observed Lock and the Unlock result candidate succeeded. A separate
[repeated-Unlock fixture](benchmarks/d3d8/2026-11-14-repeated-unlock/result.json)
shows that Highball accepts that sequence in both native and facade modes, but
does not prove the game used that exact sequence.

`T3RENDER_EXPERIMENTAL_REPEATED_UNLOCK=1` is a separate opt-in within the
combined facade. If the same facade buffer has a completed observed Lock/Unlock
pair and no lock or mutation is active, a subsequent Unlock discards that
buffer's entire shadow before forwarding and holds a device mutation window.
Success preserves unrelated shadows; failure, detected overlap or any other
mismatch invalidates the device. Draws outside the capture window are not
bracketed, so this does not establish general concurrency safety. A discarded
shadow is never restored by later locks.
The header and per-draw counter explicitly identify this policy. Its
[Highball fixture](benchmarks/d3d8/2026-11-14-repeated-unlock-policy/result.json)
checks this restricted sequence; unobserved native aliases, general TDS
compatibility and the menu text regression remain unverified. A later
[TDS bedroom test](benchmarks/runtime/2026-11-14-tds-repeated-unlock-policy/result.json)
used this flag and obtained 144 indexed draws with VB, IB, declaration,
program and constants candidates in the same record. All 144 have color writes
disabled and stencil enabled; they are not sufficient for the visible room.
One color-writing indexed quad from that frame can now be displayed by the
[partial DX12 diagnostic](tools/README_diagnostic_quad.md). Its content is not
identified, and this is not a replay of the room. The frame remains partial and
non-replayable; the user reported truncated menu text in that run.

The combined observer also records a `shadow_reason` for failed prepared
buffer reads and, for an unsupported vertex program, the first observed word if
readable. These are diagnostic metadata only: an unknown buffer remains
unknown and a shader word is not a program candidate. The paired draw result
must still be checked, and these observations cannot identify unobserved aliases
or prove GPU input. Older traces do not contain this information.

Add `-DT3RENDER_BUILD_SMOKE_TEST=ON` at configuration time to build a
standalone D3D8 program in the `smoke/` subdirectory. It draws four synthetic
triangles, checks one pixel of each and prints a backbuffer hash. Compare that
hash with and without the proxy only on the same adapter, format and runtime.
`-DT3RENDER_BUILD_D3D8_UP_FIXTURE=ON` instead adds a synthetic
fixture in `d3d8_up_fixture/`, with optional PPM readback for replay testing.
The fixture's explicit `--buffered --verify-buffer-readback` mode also locks
its managed, non-write-only VB/IB with `D3DLOCK_READONLY` and compares only the
written ranges using bounded `ReadProcessMemory` snapshots. It adds driver calls
that may synchronize or stall; it is not passive capture and does not test TDS
or other buffer types. Add `--buffer-snapshot new-file` to serialize the two
verified ranges in the fixture-specific `T3R_BUFFER_V1` ASCII format. Existing
files are never replaced; these bytes are fixture-owned readback, not proxy trace.

The separate `--buffered --wrapped-buffers` fixture creates MANAGED,
WRITEONLY vertex and index buffers, wraps their COM interfaces without changing
the runtime objects, and observes the application's own Lock/Unlock writes.
The fixture explicitly unwraps resources before passing them to the real
device. An optional `--buffer-snapshot new-file` serializes the observed
application writes without READONLY locks on the buffers. The fixture also checks
getter identity while wrappers are alive and that the device retains its raw
bindings after the last wrapper reference is released. Its [benchmark](benchmarks/d3d12/2026-11-12-wrapped-writeonly-replay/result.json)
compares the synthetic D3D8 outputs and the DX12 replay.

This harness is only for controlled, serial fixture calls; it is not connected
to the proxy or TDS. Its `GetDevice` exposes the native device, unknown IIDs can
expose raw resource aliases, and only the individual wrapper shadow is
invalidated on such escape. Adopting the same real object twice creates
different wrapper identities; the fixture adopts each resource exactly once.
The getter fallback after final wrapper release returns the raw resource as
untracked. State blocks, global alias invalidation, complete COM identity and
device interception remain unimplemented. Each wrapper has its own 512 KiB
shadow/staging budget and a 256 KiB per-lock snapshot limit. CPU writes racing
the snapshot may differ from what the driver consumes; no complete frame or
gameplay parity is implied.

This direction matters because T3SDK reconstructs WRITEONLY buffer creation
in [`VBPoolStaticPermanent_4.cpp`](../src/Game/VBPoolStaticPermanent_4.cpp#L33)
and [`Unsorted_10942D20.cpp`](../src/Game/Unsorted_10942D20.cpp#L136), while
[`cD3DDynamicVertexBuffer.cpp`](../src/Game/cD3DDynamicVertexBuffer.cpp) uses
DISCARD/NOOVERWRITE lock flags. The isolated fixture does not establish coverage
for those game paths.

Add `-DT3RENDER_BUILD_D3D12_PROBE=ON` to build `t3render_d3d12_probe.exe`, a
separate offscreen D3D12 device test. It creates a queue, pipeline and render
target, draws a synthetic triangle, waits on a fence and validates readback.
`--warp` explicitly selects the software adapter; `--output <new-image.ppm>`
optionally saves the result. It requires the Windows D3D12 and DXGI runtimes
and `d3dcompiler_47.dll`, and uses SM 5.0 shaders. Capability queries are
capped by the available SDK headers; DXR is not tested. This probe is not the
D3D8 compatibility backend.

To test a 64-bit D3D12 runtime, build the probe alone in a separate directory:

```sh
cmake -S t3render -B build/t3render-d3d12-x64 -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows -DT3RENDER_D3D12_PROBE_ONLY=ON \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
cmake --build build/t3render-d3d12-x64
```

With Visual Studio, use `-A x64 -DT3RENDER_D3D12_PROBE_ONLY=ON` in a new
build directory instead. Probe-only mode also supports Win32, does not require
`d3d8.h`, and never builds the proxy. The normal TDS proxy remains x86-only;
an x64 probe result does not validate that proxy or the game's x86 runtime.

On 2026-11-12 the x86 probe passed on Apple M5 Pro through Highball's Wine
D3D12/vkd3d, Vulkan, MoltenVK and Metal. Its process used Wine's DXGI by
excluding the DXMT path; with the original Highball path the mixed DXMT/Wine
interfaces failed to create a device, and WARP was unavailable. The x64 probe
also passed through D3DMetal. Both produced identical synthetic pixels. See
the [result manifest](benchmarks/d3d12/2026-11-12/result.json) and
[image](benchmarks/d3d12/2026-11-12/triangle.png). Neither run is a native
Windows test, a TDS frame, or a replay.

A separate [synthetic D3D8-to-DX12 replay](tools/README_synthetic_replay.md)
validates captured UP vertices in screen space, ordered draws with nonidentity
camera transforms, and an indexed fixture-owned readback. Reference, captured and
replay images match on the tested Wine runtime. General buffered capture,
textures and actual TDS frames remain unsupported.

To repeat the x86 test on the same Highball setup, pass the engine directory,
the bottle directory, the built x86 executable and an unused absolute output
prefix to `tools/run_highball_probe.sh`. It omits DXMT from the child process's
DLL search path, writes `<prefix>.log` and `<prefix>.ppm` without overwriting
existing files, and leaves the bottle configuration unchanged. It does not
establish that the same DLL selection is suitable for TDS.

The proxy forwards to the system D3D8 implementation and, when capture is
enabled, observes the original COM objects. It is specific to T3Main's
`Direct3DCreate8` import and does not implement every possible D3D8 export or
chain with arbitrary existing wrappers. T3SDK also hooks device creation,
reset and presentation; coexistence must be checked in a real game run.
Wine's native/builtin selection and D3DMetal chain also require verification.
Do not replace an existing wrapper without first understanding that chain.
The per-object vtable interception is experimental: a distinct COM alias can
bypass it and, if released last, leave an entry allocated until process exit.

`src/com_identity_registry.hpp` is an independent identity-table prototype for
a future COM facade. Its weak entries, generation checks and strong temporary
pins are host-tested; it is not connected to the proxy. A pin is not a COM
reference. The future facade must coordinate native lifetime and an atomic,
non-resurrecting COM `TryAddRef` with final release before retiring an entry.
The table alone does not close resource/device alias paths or make buffer
capture safe for TDS.

Separate [fixture-only factory and device facades](tests/README_device_facade.md)
have also been compared with native D3D8 on Highball. They forward native
resources, so their `GetDevice` still escapes the facade; they are not
integrated into this proxy and must not be used with wrapped buffers in TDS.

## Capture and inspect

Set `T3RENDER_CAPTURE_PATH` to a new absolute Windows path and select exactly
one trigger before launching the game:

- `T3RENDER_CAPTURE_KEY=F11`: on a key-down edge, capture the next frame.
  Input is observed, not consumed. Detection depends on the Present thread's
  input queue; if it does not see the key, use the numbered trigger.
- `T3RENDER_CAPTURE_FRAME=N`: capture frame `N`, counting from zero at device
  creation. A game restart or replacement device starts the count again.
- `T3RENDER_CAPTURE_TRIGGER_PATH=C:\\path\\capture.ready`: after the game has
  reached the desired scene, create this file to capture the next frame. The
  parent directory must exist and the marker must not exist at startup. The
  proxy only observes the marker; remove it after the test. A directory,
  inaccessible marker or stale marker fails closed. This avoids relying on the
  Present thread's keyboard queue but does not verify which scene is active.
  If the device is released after the marker appears, capture is stopped rather
  than transferring the request to a replacement device.

Optionally set `T3RENDER_CAPTURE_FRAMES=1`, `2` or `3` to capture that many
consecutive Present-delimited frames in the same JSONL file. The default is
one; any other value disables capture. Each frame records observed draw order,
results and byte budgets. A failed Present is recorded as a frame boundary
with its HRESULT, not silently treated as a successful display. If the device
is released or an operation remains pending before the window ends, the trace
is incomplete and has no `capture_end`. This option does not select or verify
the map: trigger it only after reaching the intended room. Boundaries follow
intercepted Present completions; concurrent Present calls can still be
observed in a different order from driver submission. The trace's
`concurrent_operations` flag covers overlapping draw/clear attempts, not
Present or Reset calls.

An existing output file is never overwritten. Environment variables must
reach the actual `T3Main.exe` process, including across a level restart.
Without a valid path and trigger no frame is captured. Capturing a frame can
be expensive: it queries D3D8 state on every draw and writes a potentially
large file. The trace is local game-derived data and is not to be committed.

### Experimental vertex-declaration observation

`T3RENDER_CAPTURE_DECLARATIONS=1` independently enables candidate declaration
observation from device creation, before a frame trigger. It does not enable
VB/IB wrapping. If the buffer observer is also enabled for a device, the
proxy suppresses declaration observation on that device because buffer
`GetDevice` can leak an untracked alias. The header reports the request, not
successful activation. The proxy copies at most 1024 DWORDs from each application
`CreateVertexShader` declaration using checked memory reads, and tracks
creation, deletion, binding, reset and draw windows. For an active draw the
usual `GetVertexShader` query is performed before the capture writer's mutex.
No `GetVertexShaderDeclaration` call is made by the observer.

A `vertex_declaration` with status `candidate` contains an observed numeric
handle match, generation and DWORDs. Match it by `op_id` to `draw_result` and
require a successful HRESULT and `shader_stable: true` before using it as a
candidate. Even then, the numeric value could be an FVF, the input memory can
change before the driver consumes it, and an unobserved native alias can bypass
the hooks. Detected device aliases, state blocks and overlapping shader
operations invalidate evidence conservatively. A nested draw, clear, reset or
present during a writer getter abandons the partial trace; payload retirement
is deferred until the writer lock has been released. Unknown, incomplete or
unsafe observations contain no declaration words. On Highball, a real TDS
bedroom capture produced 285 unavailable and 16 unknown observations, with no
usable candidate; this version did not record why the data was unavailable.
See the [result](benchmarks/d3d8/2026-11-12-tds-bedroom-declarations/result.json).
In a follow-up [capture](benchmarks/d3d8/2026-11-12-tds-declaration-reset/result.json),
415 unavailable observations had a complete token scan and were invalidated by
an earlier Reset attempt. Its HRESULT and shader survival were not recorded.
This does not validate unobserved aliases or gameplay. Declaration observation
alone does not capture shader bytecode or constants and cannot make the frame replayable.

#### Pre-reset history hypothesis

`T3RENDER_CAPTURE_DECLARATION_HISTORY=1`, together with declaration capture,
retains eligible complete application-memory snapshots across successful Reset
calls. It is independently opt-in and suppressed when buffer observation is
active. A later successful explicit `SetVertexShader` for the same numeric
value permits an additional `vertex_declaration_pre_reset` field, labelled
`hypothesis` with the pre-reset source and Reset/rebind sequence numbers. The
ordinary declaration remains `unavailable` with null words. The optional IR
field is available only after a matching successful, stable draw result; it is
never used as a standard candidate or replay evidence. The numeric match can
still be an FVF or reflect an unobserved alias, and even a completed scan can
differ from the driver input. A failed or unobserved Reset, failed Set, Delete,
collision, overlap or detected escape suppresses or discards the hypothesis.

History mode reduces the entire per-device declaration payload budget,
including live and retired/pinned data, to 8192 DWORDs (32 KiB); transient
snapshot copies are outside that budget. Serialization is limited to 1024
DWORDs per observation and 262144 per frame. A limit or allocation failure
omits evidence or invalidates the device; it never invents data. A successful
fixture Reset only characterizes its tested Highball runtime, not TDS or other
drivers. Existing traces cannot recover words that were discarded before this
option was enabled.

A later [TDS bedroom capture](benchmarks/d3d8/2026-11-13-tds-declaration-history/result.json)
with history enabled contained 305 stable declaration candidates in 321 draws,
and no pre-reset hypotheses. This is an observation of that run, not evidence
that the previous reset was avoided or that the candidates match GPU input. All
312 indexed draws bound WRITEONLY vertex and index buffers; reading them with
`D3DLOCK_READONLY` is not a supported substitute for safe write observation.
Buffer bytes and shader programs remain absent, so this frame cannot be replayed.

#### Vertex-program candidates

`T3RENDER_CAPTURE_VERTEX_PROGRAMS=1`, together with declaration observation,
samples bounded `vs_1_0` and `vs_1_1` application-memory programs at `CreateVertexShader`.
It does not query shader handles through the driver; the standard hook suppresses
it when buffer observation is active, while the experimental combined facade
can observe both. Other versions, malformed data, null
functions and exceeded budgets expose no program words. Programs share the
per-device declaration budget and are not exposed as candidates after Reset. The trace is
limited to 1024 words per observation and 262144 per frame; the offline IR
requires a matching declaration generation and successful, stable draw.
When a declaration is available but program sampling fails, the trace records
an explicit unavailable reason without partial words.
With declaration history enabled, an eligible pre-reset program can instead
appear as a separate hypothesis paired to the declaration's Reset and rebind.
It is not a program candidate or evidence of post-reset driver state.
These are candidates, not proof of shader identity or GPU input. Constants,
pixel programs and buffers are still required for a replay. See the
[program reader](ir/README_shader_program.md).
In a [TDS test](benchmarks/d3d8/2026-11-13-tds-vertex-program/result.json),
a prior Reset invalidated the declarations and no program was emitted. This
does not establish which shader versions the game uses.

An offline [kernel inventory](benchmarks/d3d8/2026-11-14-vs10-observation/result.json)
found 39 `vs_1_0` and 3 `vs_1_1` vertex programs with supported token framing.
That inventory does not bind any program to a particular runtime draw. A
synthetic `vs_1_0` Highball test matched HRESULTs and RGB with and without the
standard proxy and the experimental combined facade; it is not a replay of a
TDS shader. The combined facade invalidated shader evidence after the first
shader deletion, as required by the handle-reuse policy.
A [follow-up](benchmarks/d3d8/2026-11-13-tds-program-history/result.json)
retained 495 declaration and 94 program hypotheses across successful Reset
and rebind observations; they remain unverified pre-reset data.

#### Vertex shader constant samples

`T3RENDER_CAPTURE_VERTEX_CONSTANTS=1`, together with declaration observation,
samples registers 0 through `min(MaxVertexShaderConst, 96) - 1` before each
captured draw. It calls the runtime getter twice into differently initialized
buffers and checks both results, their contents and guard words. Failed or
inconsistent reads emit no values. The opt-in also intercepts constant writes:
an observed write overlapping a draw invalidates the shader observation.
The trace retains raw float bits, with a 262144-word frame budget; the offline
IR exposes them only after a successful draw with a stable observed window.
Caps must be available. The observer is suppressed with buffer observation.

These samples are not proof of GPU input: unobserved aliases, driver behavior
and mutations outside intercepted calls can still matter. Pixel shader
constants, buffers and texture bytes are absent. The additional getters are
diagnostic and may cost time or synchronize with the driver. The opt-in is
validated with host tests and a hidden synthetic Highball fixture, not TDS.

### Experimental buffered capture — isolated fixture only

Set `T3RENDER_CAPTURE_BUFFERS=1` in addition to the capture configuration to
observe application VB/IB writes from resource creation until capture ends.
The proxy passes native resources to D3D8 and does not replace resource vtables
or perform extra buffer locks. It tracks writes in bounded CPU shadows and
records selected stream 0 and index ranges in `buffer_data` for buffered draws.
The header reports whether the option was requested; requested does not imply
successful tracking. A draw's `buffer_stable` is true only if its intercepted
window stayed free of recognized hazards. Missing, unstable, partial or failed
observations are not replay evidence, and concurrent CPU writes through a
mapped pointer may still differ from what the driver consumes.

The shared shadow/staging budget is 32 MiB for at most 4096 resources, and a
single lock snapshot is limited to 256 KiB. The trace has separate 256 KiB per
component and 8 MiB per frame output limits. Unknown aliases, failed locks,
GPU `ProcessVertices` writes, reset, state blocks and recognized overlaps can
invalidate tracking. When capture stops, shadows are discarded but issued
wrappers remain active for forwarding until released.

**Do not enable this option in TDS yet.** An uninstrumented device alias can
escape through other resources and bypass the proxy's unwrap hooks. If the
application passes a buffer wrapper to that alias, Wine may assert or abort;
marking bytes unknown cannot prevent that crash. Device and resource alias
coverage, state-block ownership, COM identity and coexistence with T3SDK hooks
need further validation. The current Wine test is an isolated synthetic fixture.
The [buffer/device facade experiment](tests/README_buffer_facade.md) closes
`GetDevice` on wrapped VB/IB in isolation, but other resource aliases still
escape and the experiment is deliberately not enabled in the game proxy.
The [resource facade experiment](tests/README_resource_facade.md) additionally
tests 2D, cube and volume textures, their levels, and an additional swapchain;
other paths and native gameplay compatibility remain unproven.

```sh
python3 t3render/tools/trace_summary.py /path/to/capture.jsonl
python3 t3render/tools/trace_summary.py /path/to/capture.jsonl --json
python3 -B -m unittest discover -s t3render/tests -v
```

The validator checks the JSONL framing and summarizes draw calls. A valid
trace is not evidence that every D3D8 operation was captured, that a frame
can be replayed, or that its pixels match the original.

An [offline Render IR prototype](tools/README_render_ir.md) translates these
snapshots into API-independent camera, mesh, material, light and draw records.
It preserves missing data and legacy state; it is not yet the runtime IR.

`ir/buffer_shadow.hpp` and `ir/buffer_registry.hpp` provide a separate
CPU-side model for partially known buffer contents and resource generations.
The registry bounds resources and the shared shadow/staging byte budget. It
invalidates writable ranges before a lock attempt and commits staged bytes only
after a successful unlock; ambiguous resource identity or lock state is
abandoned. The caller must synchronize all operations and supply a fault-safe
reader; partial reads never become known data. The experimental opt-in uses
these models for observed resource writes, subject to the limitations above.

## Scope and next steps

For each observed draw, the snapshot includes its parameters, available
camera/world/texture matrices, viewport, render and texture-stage states,
material, a bounded set of lights, texture and buffer bindings/descriptors,
and shader handles. For UP draws, it also attempts a bounded, fault-safe copy
of the supplied vertex and index bytes before the draw. Each component records
whether it was captured, empty, unreadable or exceeded a limit; the maximum is
256 KiB per component and 8 MiB per frame. The captured memory may change
before the real draw. The indexed vertex range uses `MinVertexIndex * stride`,
which still needs a nonzero-index runtime test.

Clear operations and their rectangles are recorded before the driver call, and
each clear/draw attempt is paired with its HRESULT. A frame is left incomplete
if Present occurs while such an operation is pending; overlapping operations
are flagged because their actual submission order may differ from the log.

The header records the exact limits. IDs are transient process addresses, not
stable T3 asset names. Capture does not include texture bytes, complete
buffered-resource histories, shader code/constants, reliable complete declarations, all light or
world indices, render-target operations or complete resource lifetime. Snapshot order under concurrent
device calls is not guaranteed to
be the final GPU submission order. The first device is observed until its
final intercepted release; a subsequently created device can then be observed,
but a second device already alive at that point is not adopted. Device
creation/reset/presentation and frame boundaries are diagnostic observations,
not a GPU timer.

Next, validate the proxy and trace against the real x86 game, including startup,
reset, shutdown and the T3SDK display hooks. Capture the same bedroom camera
with and without the observer and compare pixels and behavior. Then record
resource contents, lifetimes, render targets, clears, state blocks and shader
programs so a frame can actually be replayed before defining the compatibility
backend and testing DX12 parity. Keep the modern lighting pipeline independent
of T3's stealth model.

For buffered geometry, tracking must start when each resource is created, not
when a frame trigger is pressed. An experimental per-object VB/IB vtable swap
was rejected: on the synthetic buffered fixture Highball/Wine asserted that the
vertex buffer still had its original runtime vtable. The hooks were removed;
see the [failure record](benchmarks/d3d8/2026-11-12-buffer-tracking-failure/result.json).

The experimental opt-in now uses COM wrappers and unwraps observed device
boundaries, but it does not cover every alias, state block or resource escape.
Those gaps must be resolved before testing it in TDS. The fixture's selective
READONLY readback does not establish support for arbitrary game buffers;
unknown aliases, GPU writes, ambiguous locks and resets remain unknown.

The existing Godot bedroom benchmark is useful visual reference; its
“Original” images are a Godot export, not captures from Legacy D3D8. A
permanent in-game benchmark still needs a repeatable save/camera, frame images,
hardware and driver metadata, and GPU timings. `GarrettsBed` is a possible
level entry point, not yet verified at runtime.
