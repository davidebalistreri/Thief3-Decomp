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
If a wrapper has died, an attempt to adopt a later native getter reference
fails and consumes that reference; it does not reconstruct a shadow.

This is intentionally incomplete COM behavior: unknown buffer interfaces
return `E_NOINTERFACE` and all buffer private-data methods return `E_NOTIMPL`.
The fixture invalidates observation at state-block and reset calls. It cannot
provide transparent gameplay or preserve private-data behavior. Addressing
native resource aliases, resource identity and retained buffers across state
blocks, reset and concurrent calls remains necessary before integration.

## Highball observation (2026-11-13)

In a hidden x86 fixture using Highball CrossOver 26.3 r30, native and facade
runs returned the same 81 recorded HRESULT/state values and identical RGB
images for four synthetic triangles. The facade run verified VB/IB `GetDevice`
and `IUnknown` round trips, set bindings and drew through the returned facade,
and compared the complete `WRITEONLY` shadow bytes during a stable draw
window. After a state block captured a vertex binding, the fixture unbound it,
released its wrapper, restored the state block and confirmed that reacquiring
the now-untracked resource failed closed. Steam remained open; TDS was not run.

The [manifest](../benchmarks/d3d8/2026-11-13-buffer-facade/result.json)
records source, executable, log and image hashes. Full logs and PPMs are in
`build/t3render-captures/2026-11-13-buffer-facade-restored-hidden/`. The
fixture compares RGB, not alpha, and does not exercise all state-block types,
reset, runtime concurrency, native Windows, TDS or gameplay.

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
