# Isolated D3D8 resource facade

This is a standalone synthetic experiment, not part of the game proxy.
**Do not install it in TDS.** It combines the existing buffer facade with
wrappers for 2D textures, cube textures and surfaces. These resource wrappers
route `GetDevice` to the facade after verifying the native parent's canonical
identity, preserve known `IUnknown`/resource/texture identities, and map
`GetContainer` only to a known resource or device facade. Surface-level and
cube-face getters adopt the returned surface. Unknown interfaces, unknown
containers and unrecognized wrapper inputs fail closed.

The experimental device facade unwraps known resources for texture binding,
texture updates, target selection and surface copies; getters and resource
creation adopt returned native references. Resource wrappers retain their
native objects while dormant so a state block can restore the same wrapper.
They do not retain the facade while dormant, and the facade closes the router
at final release. Private-data methods return `E_NOTIMPL`, unsupported IIDs
return `E_NOINTERFACE`, and failures can differ from native COM behavior.
Keeping resources until facade teardown can change memory usage and reset
behavior. An explicit resource limit can also cause allocation failure.

The boundary remains incomplete: volume textures, volume levels, swapchains
and other native resources can still expose native interfaces. Reset,
state-block coverage, private-data semantics, native Windows, runtime
concurrency and T3SDK coexistence need more work. The raw device accessor in
the fixture is only for verifying buffer shadow bytes; it is not exposed to an
application or used to submit wrapped inputs. None of this establishes safe
gameplay or a complete TDS capture.

## Highball observation (2026-11-13)

The hidden x86 fixture compared separate native and facade runs on Highball
CrossOver 26.3 r30. It checked 81 recorded HRESULT/state observations and
identical RGB PPMs for six synthetic triangles. Two sample texture pixels came
from a 2D texture uploaded with `UpdateTexture` and a cube map. It also checked
texture/cube/surface parent and container round trips, buffer bindings through
a device obtained from a texture surface, texture and cube identity after a
state-block restore, surface copying, target identity, rejection of unknown
inputs, and final facade cleanup. VB/IB `WRITEONLY` shadows matched expected
bytes during the selected draw window. Alpha, other resource types and the
complete game are not tested. Steam remained open; TDS was not run.

The [manifest](../benchmarks/d3d8/2026-11-13-resource-facade/result.json)
records hashes and limitations. Full local logs and PPMs are under
`build/t3render-captures/2026-11-13-resource-facade-final/`.

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
