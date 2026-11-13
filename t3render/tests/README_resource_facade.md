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
at final release. Private-data methods return `E_NOTIMPL`, unsupported IIDs
return `E_NOINTERFACE`, and failures can differ from native COM behavior.
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
CrossOver 26.3 r30. The latest run checked 87 recorded HRESULT/state observations and
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
a vertex buffer in a texture also preserved its identity and device. The
facade returned `E_NOTIMPL` in all those cases, so this is a measured
compatibility gap, not private-data parity. Forwarding resource interfaces
must also address facade/native identity and lifetime cycles; neither
cross-resource lifetime nor unknown external interfaces were fully tested.
The runner validates those intentionally different observations separately
from image and state parity. Full local logs and PPMs are under
`build/t3render-captures/2026-11-13-private-data-alias-final/`.

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
