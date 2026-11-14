# Isolated D3D8 resource facade

This is a standalone synthetic experiment, not part of the game proxy.
**Do not install it in TDS.** It combines the existing buffer facade with
wrappers for 2D textures, cube textures, volume textures and their levels,
surfaces, and additional swapchains. These resource wrappers
route `GetDevice` to the facade after verifying the native parent's canonical
identity, preserve known `IUnknown`/resource/texture identities, and map
`GetContainer` only to a known resource or device facade. Surface-level and
cube-face getters adopt the returned surface; volume-level getters adopt the
returned volume. Swapchain backbuffer getters adopt the returned surface.
Unknown interfaces, unknown
containers and unrecognized wrapper inputs fail closed.

The experimental device facade unwraps known resources for texture binding,
texture updates, target selection and surface copies; getters and resource
creation adopt returned native references. Resource wrappers retain their
native objects while dormant so a state block can restore the same wrapper.
They do not retain the facade while dormant, and the facade closes the router
at final release. Unsupported IIDs return `E_NOINTERFACE`, and failures can
differ from native COM behavior.

Private data support opaque blobs and known live buffer or resource facades
from the same device. The combined fixture shares its bridge with VB/IB
facades; the separate buffer-only fixture continues to reject COM values. The bridge pins the
facade during Set, stores the native object in the native resource, and maps a
native Get back to the facade using the registries. It never stores the
wrapper in native private data. The default retaining fixture rejects external
`IUnknown`; the separate persistent eager fixture forwards external values and
invalidates buffer capture evidence because those objects can hide native
aliases. Unknown GUIDs still return `E_NOTIMPL`. The wrapper tracks at most
64 GUIDs and refuses overlapping or
reentrant private-data calls. Failed writes or frees discard provenance. Reset
retirement can discard a source wrapper's provenance even if the native
resource survives. This subset assumes the fixture's closed boundary and is
not a complete COM implementation.
Before `Reset`, the fixture discards existing buffer shadows and retires dormant
resource and buffer wrappers, releasing their native references. After a
stable successful reset, newly created buffers can be tracked again. Surviving
or reacquired buffers retain unknown contents, and their lock windows still
prevent a supposedly stable draw or reset. A failed reset leaves observation
unavailable until another stable reset succeeds; a detected overlap or escape
permanently invalidates observation. The fixture guards native buffer Create/Get
calls through adoption to detect overlap with reset. An explicit resource limit
can also cause allocation failure.

In this combined fixture, the six state-block calls are bracketed by buffer
mutation windows; a failed call invalidates observation. Successful sequential
calls can preserve existing shadows, including for dormant wrappers later
reacquired from bindings. This relies on the fixture's closed boundary and is
not the policy of the production proxy or the buffer-only fixture.

The boundary remains incomplete: only selected paths are exercised and unknown
interfaces are rejected, not transparently forwarded. Complete reset and
state-block coverage, private-data semantics, native Windows, runtime
concurrency and T3SDK coexistence need more work. The raw device accessor in
the fixture is only for verifying buffer shadow bytes; it is not exposed to an
application or used to submit wrapped inputs. None of this establishes safe
gameplay or a complete TDS capture.

## Highball observation (2026-11-13)

The hidden x86 fixture compared separate native and facade runs on Highball
CrossOver 26.3 r30. The latest run checked 159 recorded HRESULT/state observations and
identical RGB PPMs for six synthetic triangles. Two sample texture pixels came
from a 2D texture uploaded with `UpdateTexture` and a cube map. It also checked
texture/cube/surface parent and container round trips, buffer bindings through
a device obtained from a texture surface, texture and cube identity after a
state-block restore, surface copying, target identity, rejection of unknown
inputs, and final facade cleanup. A later run also checked volume texture and
level identity, parent/container queries, binding and state-block restore,
plus bytes written through the texture and read through its volume level.
It also exercised an additional swapchain, its backbuffer and `Present`, and
bindings through the device obtained from that backbuffer. On this runtime,
both native and facade returned `E_NOINTERFACE` for swapchain `QueryInterface`
to the device and backbuffer `GetContainer` for the swapchain; backbuffer
`GetContainer` for the device succeeded in both runs. Supported swapchain
container identity is covered by the host fake, not by that runtime result.
In a separate device test, a reset after releasing default-pool resources
succeeded in both runs and a post-reset indexed draw produced the same checked
pixel. With live default-pool resources, both runs returned `0x88760868`; after
release, both recovered with a successful reset. A managed surface reacquired
its parent after reset. New managed WRITEONLY buffers produced the expected
shadow bytes after both a successful reset and recovery from a failed reset; a
surviving buffer's old bytes remained unknown. These observations do not
establish complete reset parity.
VB/IB `WRITEONLY` shadows matched expected
bytes during the selected draw window. Alpha, other resource types and the
complete game are not tested. Steam remained open; TDS was not run.

A subsequent [private-data observation](../benchmarks/d3d8/2026-11-13-private-data/result.json)
compared vertex and index buffers, textures, backbuffers, texture levels,
cube textures, volume textures and volume levels. On this Highball runtime,
native private data round-tripped bytes and an application `IUnknown`; storing
a vertex buffer in a texture also preserved its identity and device. At the
time, the facade returned `E_NOTIMPL` for all those cases. Forwarding resource interfaces
must also address facade/native identity and lifetime cycles; neither
cross-resource lifetime nor unknown external interfaces were fully tested.
Full local logs and PPMs are under
`build/t3render-captures/2026-11-13-private-data-alias-final/`.

The earlier [opaque-blob observation](../benchmarks/d3d8/2026-11-13-private-blobs/result.json)
compares native and facade results for those eight resource types, including
size queries, short buffers and reads after free. Its `IUnknown` and unknown
GUID cases differed, and were checked separately. Its full local logs and
PPMs are under `build/t3render-captures/2026-11-13-private-blobs-verified/`.

The [private-resource lifetime observation](../benchmarks/d3d8/2026-11-13-private-lifetime/result.json)
shows why `IUnknown` cannot simply be forwarded: a native texture retains a
vertex buffer until the texture is released, while deliberately storing a
wrapped vertex buffer in the fixture's native texture keeps the facade device
alive after external releases. The test breaks that cycle and confirms final
cleanup. It uses a test-only borrowed native resource and is not an application
path.

The [vertex private-data bridge observation](../benchmarks/d3d8/2026-11-14-private-vertex-bridge/result.json)
now covers this specific texture-to-VB reference. The native and facade runs
both recovered the vertex interface and its parent after the VB wrapper became
dormant, then after a successful Reset retired that wrapper. Size queries,
short buffers, replacement with a blob and replacement back to the VB were
checked. The facade device and both registries closed with the texture's
private reference still present. The 159 recorded state observations and RGB
images also matched. Host fakes exercise rejected values, failures, reentrancy
and overlapping calls. This does not solve external `IUnknown`, other resource
types, cross-device or self references, runtime concurrency, native Windows,
or the longer retention of dormant native resources. Full local logs and PPMs
are under `build/t3render-captures/2026-11-14-private-vertex-bridge-v4/`.

The subsequent [buffer bridge observation](../benchmarks/d3d8/2026-11-14-private-buffer-bridge/result.json)
also translates a texture-to-index-buffer reference. It switches the same GUID
from VB to IB, and checks both buffers after dormancy and a successful Reset
with the private references still present during facade teardown. It has the
same restrictions as the vertex-only experiment. Logs and PPMs are under
`build/t3render-captures/2026-11-14-private-buffer-bridge-v2/`.

The [resource bridge observation](../benchmarks/d3d8/2026-11-14-private-resource-bridge/result.json)
adds texture, cube, volume texture, volume and surface targets. It checks a
texture self-reference and a texture/cube cycle, explicitly freeing those
references as required by native COM. A managed cube held only by a texture's
private data was reacquired before and after Reset. Device/registry teardown
completed with that native private reference still stored. At that milestone,
buffer sources were still unsupported. Swapchain private targets, external COM
objects, cross-device inputs and general runtime concurrency remain unverified
or unsupported. Full local logs and PPMs
are under `build/t3render-captures/2026-11-14-private-resource-bridge-v2/`.

The [buffer-source observation](../benchmarks/d3d8/2026-11-14-private-buffer-sources/result.json)
checks VB and IB private-data sources against both buffers and selected
textures/surfaces/volumes, including buffer self-references and a deliberately
broken VB/texture cycle. A live VB source retained its cube reference across a
Reset. If a source wrapper itself is retired, its private-data provenance is
still lost and subsequent access fails closed. Full local logs and PPMs are
under `build/t3render-captures/2026-11-14-private-buffer-sources-v2/`.

The [external-object lifetime observation](../benchmarks/d3d8/2026-11-14-private-external-lifetime/result.json)
tests an application `IUnknown` that retains its device. Native D3D8 released
the object when its texture was released. The facade rejected it, as intended;
the test then deliberately stored it through a borrowed native texture. The
dormant texture wrapper kept that native texture alive, and the object kept
the facade device alive in a cycle. The test broke the cycle and verified both
registries closed. Translating known buffer/resource pointers does not solve
this case: blindly forwarding external objects is unsafe. Full local logs and
PPMs are under `build/t3render-captures/2026-11-14-private-external-lifetime-v2/`.

The isolated [eager-resource retirement experiment](../benchmarks/d3d8/2026-11-14-eager-resource-lifetime/result.json)
releases the native resource at its facade's last external Release instead of
retaining a dormant wrapper. In the same external-object diagnostic, this
allowed the texture, external holder and device to close without breaking an
artificial facade cycle. Host tests exercise concurrent getter/final-Release
and reentrant native destruction. In that version, a resource retained by a
native binding could be wrapped again, but its previous private-data provenance
was unavailable, while native D3D8 still returned the data. That version did
not enable external objects, did not change the default
fixture, did not retire dormant VB/IB, and did not solve state-block or game
compatibility. Logs and PPMs are under
`build/t3render-captures/2026-11-14-eager-resource-lifetime/`.

The [persistent marker experiment](../benchmarks/d3d8/2026-11-14-eager-private-marker/result.json)
attaches a COM marker under a reserved native private-data GUID. The marker owns
no source, facade or device reference; its registry is non-owning. In the eager
fixture a blob and a cube reference survived source-wrapper retirement and
reacquisition, including through a state block, while the external-holder
diagnostic still cleaned up without breaking an artificial cycle. A host test
checks reserved-GUID collisions, failed writes, concurrent initialization,
reentrancy and marker lifetime. The reserved GUID is deliberately rejected at
the facade, and an existing native value at that GUID is never overwritten.
This changes native private data and is not transparent to every application;
external `IUnknown` values are still rejected. Native alias escapes, dormant
buffer ownership, reset variants, concurrency in the runtime, Windows and TDS
remain unverified. Logs and PPMs are under
`build/t3render-captures/2026-11-14-eager-private-marker-v3/`.

The [eager-buffer retirement experiment](../benchmarks/d3d8/2026-11-14-eager-buffer-lifetime/result.json)
also retires VB/IB wrappers at their last external Release. For a managed
`WRITEONLY` VB held by a state block, the facade reacquired the native buffer
and an external holder of the device was released without a facade cycle.
However the previous shadow bytes and VB private-data provenance became
unknown; native D3D8 still returned the private blob. This is not sufficient
for a complete frame capture. The default fixture still retains dormant
buffers; the eager policy is isolated and must not be used in TDS. Host fakes
also exercise a getter racing final Release. Logs and PPMs are under
`build/t3render-captures/2026-11-14-eager-buffer-lifetime-v2/`.

The [persistent eager-buffer experiment](../benchmarks/d3d8/2026-11-14-persistent-eager-buffers/result.json)
extends the marker to retain budgeted VB/IB shadow state without owning the
native resource or device. Managed `WRITEONLY` VB and IB bytes and private
blobs survived wrapper retirement and state-block reacquisition in Highball.
For the interval without wrappers, the fixture resolves a valid, caller-owned
native getter reference by canonical `IUnknown`; the ordinary read API still
rejects unknown pointers without dereferencing them. After Reset, old shadow
bytes remain unknown. Host tests cover address reuse, marker collisions,
getter/final-Release races, reentrancy, reset and hazard invalidation. This is
still opt-in and fixture-only: the reserved GUID changes native private data,
the capture observer is not unified and
neither TDS nor native Windows have been validated. Logs and PPMs are under
`build/t3render-captures/2026-11-14-persistent-eager-buffers-v6/`.

The [eager external COM experiment](../benchmarks/d3d8/2026-11-14-eager-external-com/result.json)
forwards unknown application `IUnknown` values only in the persistent eager
variant. Texture and VB sources round-tripped the same external pointer after
wrapper reacquisition; size queries, short buffers, replacement with a blob and
cleanup were checked. A cross-device texture/VB cycle preserved identities and
parents and was explicitly broken. A holder retaining its source device was
released without the artificial dormant-wrapper cycle. On any external write,
buffer evidence is invalidated conservatively; future capture must also account
for possible hidden device aliases. Reentrant/overlapping private-data calls,
reserved-GUID collisions, native Windows and TDS parity remain unresolved.
Logs and PPMs are under `build/t3render-captures/2026-11-14-eager-external-com-v4/`.

The [prepared-buffer capture experiment](../benchmarks/d3d8/2026-11-14-prepared-buffer-capture/result.json)
resolves native VB/IB identity before entering the trace writer, while holding
native getter references. A prepared reader only reads its registered shadow
and cannot call COM; it becomes unavailable when its draw ticket ends or is
invalidated. In the persistent eager fixture, a managed `WRITEONLY` VB/IB pair
was released by the application, restored through a state block, and captured
in one actual indexed D3D8 draw on Highball. The trace contains the exact 60
vertex and 6 index bytes with a successful, stable paired draw and Present.
The runner validates those bytes and the event sequence. A host writer test
also serializes pre-supplied declaration/program/constant candidates alongside
buffer bytes, and checks foreign-owner and failed getter snapshots fail closed.
That host test does not demonstrate simultaneous real observation. The 159
native/facade state comparisons and equal RGB images come from the separate
default fixture run, not from a synchronized image of this captured draw.
The offline converter still reports this trace as incomplete and not replayable.
Shader instrumentation, texture content, full alias safety, concurrency,
native Windows and TDS remain unverified. This is not a unified game facade;
do not enable it in TDS. Full local logs, images and the synthetic trace are
under `build/t3render-captures/2026-11-14-prepared-capture-verified/`.

The subsequent [combined-observer experiment](../benchmarks/d3d8/2026-11-14-combined-observer-dx12/result.json)
intercepts shader creation, binding, constant writes and an indexed draw in
the resource facade fixture. Following state-block restore and wrapper
retirement, one draw produced both exact VB/IB shadow bytes and candidate
declaration/program plus sampled constants in the same trace. The writer
recorded the clear, draw, paired stability flags and Present. An RGB image was
read from the D3D8 backbuffer after that draw and before the same Present.

The runner recognized this fixture's **one exact** `vs_1_1` program (`oPos =
v0`, `oD0 = c0`), converted its indexed vertices and constant into a synthetic
flat-color packet, then compared the offscreen DX12 RGB output with that
synchronized D3D8 image. All pixels matched on Highball. The converter does not
execute or generally translate shader bytecode; its three identity matrices
represent this particular shader, not a captured camera. Depth, blending,
textures and gameplay are not tested in that replay. The separate default
fixture's 159 native/facade observations and RGB comparison are not that draw.
The converter rejects unsupported states and requires fixture attestation.

In that experiment the single-threaded observer was attached after the state
block was applied and detached before Reset. A sequential state-block call or
unsupported draw while attached abandons capture. It is not a complete
facade or a transparent game observer: native alias escapes, other resource
operations, concurrency and reentrancy remain open, and texture contents are not captured.
The offline diagnostic IR is still incomplete and marks the frame unreplayable.
Do not install the facade in TDS. Full local artifacts are under
`build/t3render-captures/2026-11-14-combined-observer-dx12/`.

The [combined external-object observation](../benchmarks/d3d8/2026-11-14-combined-external-invalidation/result.json)
checks external `IUnknown` writes from both a texture and a VB. The fixture
invalidated buffer and shader observation before forwarding either object;
the object's reentrant `AddRef` checked that the shader window was already
unavailable. A fresh post-Reset buffer shadow was available before the first
write and unavailable afterwards. The same run retained the synchronized
synthetic D3D8/DX12 RGB match. This is a sequential fixture test, not proof of
safe observer lifetime under concurrent or arbitrary reentrant calls. The
local logs and images are under
`build/t3render-captures/2026-11-14-combined-external-invalidation-v3/`.

The later [combined reset/recovery observation](../benchmarks/d3d8/2026-11-14-combined-reset-recovery/result.json)
keeps a new fixture observer attached across a deliberately failed Reset and
a successful recovery Reset. Shader sampling remains unavailable during
recovery; the old shader remains unavailable after recovery. A newly created
shader and a fresh managed `WRITEONLY` buffer become observable. Surviving
pre-reset buffer bytes remain unknown. This reset sequence is separate from
the earlier captured draw; no reset is present in that replayed frame. The test
does not establish parity for every Reset, state block or concurrent call.
Local logs and images are under
`build/t3render-captures/2026-11-14-combined-reset-recovery/`.

The [shader/state-block lifetime observation](../benchmarks/d3d8/2026-11-14-shader-stateblock-reuse/result.json)
shows why numeric shader handles alone cannot establish the active program.
On Highball, a state block captured shader A; after A was deleted, shader B
reused its handle. Before applying the block, B rendered a red sample. After
applying it, `GetVertexShaderFunction` returned B's program, but the sample was
green, matching shader A. The native and facade runs showed the same results.
This is one runtime and state sequence, not a general claim about every D3D8
implementation. The proxy continues to invalidate shader evidence at state
blocks; a host regression test covers handle reuse followed by an apply. The
local evidence is under
`build/t3render-captures/2026-11-14-shader-stateblock-before-after/`.

The later [state-block observation](../benchmarks/d3d8/2026-11-13-stateblock-facade/result.json)
exercised Create, Capture, Begin/End, Apply and Delete on the combined facade.
After VB/IB wrappers became dormant and were reacquired, their `WRITEONLY`
shadow bytes still matched during an indexed draw and after deletion. An invalid
Delete invalidated the shadow. The separate buffer-only fixture continued to
invalidate observations conservatively. Runtime concurrency, all state-block
variants and real TDS remain untested.

The [reset epoch manifest](../benchmarks/d3d8/2026-11-13-facade-reset-epoch/result.json)
records its hashes and limitations. Full local logs and PPMs are under
`build/t3render-captures/2026-11-13-facade-reset-epoch-final/`. The earlier
[reset manifest](../benchmarks/d3d8/2026-11-13-facade-reset/result.json),
[swapchain manifest](../benchmarks/d3d8/2026-11-13-swapchain-facade/result.json),
[volume manifest](../benchmarks/d3d8/2026-11-13-volume-facade/result.json) and
[resource manifest](../benchmarks/d3d8/2026-11-13-resource-facade/result.json)
record previous versions of the experiment.

With an existing i686 MinGW build and Highball engine, from `t3sdk`:

```sh
cmake -S t3render -B build/t3render-mingw -DT3RENDER_BUILD_D3D8_RESOURCE_FACADE_FIXTURE=ON
cmake --build build/t3render-mingw --target t3render_d3d8_resource_facade_fixture
python3 -B -m unittest t3render.tests.test_d3d8_resource_facade \
  t3render.tests.test_d3d8_device_facade_runner -v
python3 -B t3render/tools/run_d3d8_device_facade.py \
  --engine '/absolute/path/to/Highball/engine' \
  --bottle '/absolute/path/to/Highball/bottle' \
  --fixture '/absolute/path/to/t3sdk/build/t3render-mingw/d3d8_resource_facade_fixture/t3render_d3d8_resource_facade_fixture.exe' \
  --output-dir '/absolute/path/to/new/output-directory' --resource-facade \
  --dx12-replay '/absolute/path/to/t3sdk/build/t3render-mingw/t3render_d3d12_replay_up.exe'
```

The runner does not edit the game installation. Wine may update the bottle;
do not run the fixture concurrently with TDS. Omit `--dx12-replay` to skip
the synthetic DX12 comparison. Build that executable with
`T3RENDER_BUILD_D3D12_REPLAY_UP=ON` before requesting it.
