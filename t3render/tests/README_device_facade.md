# Fixture-only D3D8 device facade

This is an isolated COM forwarding experiment. It is not integrated into the
proxy or TDS and must not be used in TDS. It does not wrap resources: a native
resource's `GetDevice`, `GetContainer` or `QueryInterface` can expose native
interfaces. Passing wrapped resources through those aliases is unsafe.
State blocks retain native bindings and are forwarded without identity tracking.

`CreateDeviceFacadeFactory(native)` adopts the caller's native factory +1 only
on success; on null or allocation failure it returns null and leaves ownership
with the caller. Use it once per native factory identity: it does not deduplicate
separate aliases. The factory calls native `CreateDevice` and wraps its returned
device; on wrapper allocation failure the successfully returned native device
is released and `E_OUTOFMEMORY` is returned.

`IUnknown` and the corresponding D3D8 interface resolve to the same facade
identity. Other IIDs fail closed, even if the native runtime supports them.
The device retains the factory facade until its own final release, and
`GetDirect3D` returns that facade after calling native `GetDirect3D` and
releasing its returned reference. This assumes the native runtime returns the
creating factory as the device parent; no native identity registry verifies it.
Invalid output arguments and allocation failures can differ from native COM
behavior. Callers must hold valid references; a raw pointer is not safe when
another thread performs its final `Release`.

All other factory and device methods forward to native objects and preserve
native resource pointers. This does not establish general D3D8 fidelity,
complete COM identity, Wine/Windows parity, or gameplay behavior.

Run the fake contract and i686 SDK signature checks from `t3sdk`:

```
python3 -B -m unittest discover -s t3render/tests -p test_d3d8_device_facade.py -v
```

The [Highball comparison](../benchmarks/d3d8/2026-11-12-device-facade/result.json)
ran native and facade devices in separate x86 processes. The 81 recorded
HRESULT/state observations and four-triangle RGB PPMs matched; alpha was not
compared. Surface, texture,
vertex-buffer and index-buffer `GetDevice` calls all exposed the native device;
this confirms the boundary remains open, not that TDS is safe to test.

With an existing i686 MinGW build configured as in the main README, repeat it
using an existing Highball engine and bottle and a new output directory:

```sh
cmake -S t3render -B build/t3render-mingw -DT3RENDER_BUILD_D3D8_DEVICE_FACADE_FIXTURE=ON
cmake --build build/t3render-mingw --target t3render_d3d8_device_facade_fixture
python3 -B t3render/tools/run_d3d8_device_facade.py \
  --engine '/absolute/path/to/Highball/engine' \
  --bottle '/absolute/path/to/Highball/bottle' \
  --fixture '/absolute/path/to/t3sdk/build/t3render-mingw/d3d8_device_facade_fixture/t3render_d3d8_device_facade_fixture.exe' \
  --output-dir '/absolute/path/to/new/output-directory'
```

The runner compares selected results and image bytes, creates its output in a
new directory and never launches TDS. Wine may update its bottle state.
