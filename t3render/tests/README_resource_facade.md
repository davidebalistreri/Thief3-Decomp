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
wrapper in native private data. External `IUnknown` values, unknown GUIDs and
cross-device or unknown inputs return `E_NOTIMPL`. The wrapper tracks at most
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
  --output-dir '/absolute/path/to/new/output-directory' --resource-facade
```

The runner does not edit the game installation. Wine may update the bottle;
do not run the fixture concurrently with TDS.
