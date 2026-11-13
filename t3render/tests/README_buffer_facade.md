# Isolated D3D8 buffer/device facade

This experiment combines the existing VB/IB shadow observer with the separate
device facade. **It is not integrated into the proxy and must not be used in
TDS.** Texture, surface and other native resources still expose the native
device through `GetDevice`/`GetContainer`; sending a wrapped buffer through
such an alias may crash the runtime.

The experimental `Adopt*ForFacade` path retains the facade for each buffer.
Buffer `GetDevice` calls the native method, verifies the returned device's
canonical `IUnknown` against the facade's native owner, and returns a facade
reference. A mismatch or missing identity fails closed. The caller must keep
the native owner alive through the facade and intercept every method that
accepts a buffer. The fixture unwraps only recognized buffers for stream,
index and `ProcessVertices` calls; unknown inputs fail rather than forwarding a
possibly wrapped pointer to D3D. Live getters recover the existing wrapper.
After its final external `Release`, a dormant wrapper retains its native
resource and shadow without retaining the facade. A later getter can reactivate
the wrapper; a state block or another hazard can still invalidate its shadow.
This deliberately keeps native resources alive until facade teardown. The
facade must call `CloseFacadeDevice` at its final release; it closes the
registry and releases dormant native resources outside the lock. A premature
close returns false and requires a later retry after live wrappers are released.

This is intentionally incomplete COM behavior: unknown buffer interfaces
return `E_NOINTERFACE` and all buffer private-data methods return `E_NOTIMPL`.
The fixture invalidates observation at state-block and reset calls. It cannot
provide transparent gameplay or preserve private-data behavior. Addressing
native resource aliases, private-data behavior, complete state-block tracking,
reset and concurrent calls remains necessary before integration.

## Highball observation (2026-11-13)

In a hidden x86 fixture using Highball CrossOver 26.3 r30, native and facade
runs returned the same 81 recorded HRESULT/state values and identical RGB
images for four synthetic triangles. The facade run verified VB/IB `GetDevice`
and `IUnknown` round trips, set bindings and drew through the returned facade,
and compared the complete `WRITEONLY` shadow bytes during a stable draw
window. After a state block captured a vertex binding, the fixture unbound it,
released its wrapper, restored the state block and reacquired the same wrapper.
The state-block call conservatively invalidated its bytes. The fixture also
confirmed the facade's final close completed. Steam remained open; TDS was not run.

The [manifest](../benchmarks/d3d8/2026-11-13-buffer-reacquisition/result.json)
records source, executable, log and image hashes. Full logs and PPMs are in
`build/t3render-captures/2026-11-13-buffer-facade-reacquire-v2/`. The
fixture compares RGB, not alpha, and does not exercise all state-block types,
reset, runtime concurrency, native Windows, TDS or gameplay.

The earlier [fail-closed benchmark](../benchmarks/d3d8/2026-11-13-buffer-facade/result.json)
records the prior version before dormant-wrapper reacquisition was added.
The [bedroom texture inventory](../tools/README_texture_inventory.md) also
observed cube textures in the existing TDS trace, so a 2D-only resource boundary
would not cover the benchmark's recorded texture types.

With an existing i686 MinGW build and Highball engine, from `t3sdk`:

```sh
cmake -S t3render -B build/t3render-mingw -DT3RENDER_BUILD_D3D8_BUFFER_FACADE_FIXTURE=ON
cmake --build build/t3render-mingw --target t3render_d3d8_buffer_facade_fixture
python3 -B -m unittest t3render.tests.test_buffer_proxy \
  t3render.tests.test_d3d8_device_facade_runner -v
python3 -B t3render/tools/run_d3d8_device_facade.py \
  --engine '/absolute/path/to/Highball/engine' \
  --bottle '/absolute/path/to/Highball/bottle' \
  --fixture '/absolute/path/to/t3sdk/build/t3render-mingw/d3d8_buffer_facade_fixture/t3render_d3d8_buffer_facade_fixture.exe' \
  --output-dir '/absolute/path/to/new/output-directory' --buffer-facade
```

The runner does not edit the game installation. Wine may update the bottle;
use an idle bottle and do not run the fixture concurrently with TDS.
