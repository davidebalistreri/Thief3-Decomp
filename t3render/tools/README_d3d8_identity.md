# D3D8 COM identity probe

This isolated x86 fixture creates a synthetic D3D8 device and queries the COM
identity of devices returned by surfaces, textures and volume resources. It
also tests `GetContainer` on texture levels, primary and additional swapchain
backbuffers, render targets and depth surfaces, and binds a native texture to
test `GetTexture`. It issues a clear and present, but no draws, buffer
operations, wrappers or TDS calls.

Build with an installed Win32 MSVC or MinGW-w64 toolchain. For example:

```sh
cmake -S t3render -B /private/tmp/t3render-identity-build -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++ \
  -DT3RENDER_BUILD_D3D8_IDENTITY_FIXTURE=ON
cmake --build /private/tmp/t3render-identity-build --target t3render_d3d8_identity_fixture t3render
python3 -B t3render/tools/run_d3d8_identity_probe.py \
  --engine '/absolute/path/to/Highball/engine' \
  --bottle '/absolute/path/to/Highball/bottle' \
  --fixture /private/tmp/t3render-identity-build/d3d8_identity_fixture/t3render_d3d8_identity_fixture.exe \
  --proxy /private/tmp/t3render-identity-build/d3d8.dll \
  --output-dir /private/tmp/t3render-identity-result
```

All paths must be absolute and the output directory must not exist. Without
`--proxy`, only the Wine builtin baseline runs. With it, a second run loads
the supplied proxy from a private output subdirectory and enables buffer
observation, without creating buffers. The runner verifies the proxy wrote a
complete clear/present trace. Results, SHA-256 hashes, individual observations
and logs are saved in `result.json` and the output directory. It does not edit
the game installation or change saved Highball settings; Wine may update its
own bottle state. Running Wine may require access to the bottle's wineserver.
`PASS` means the measurement completed; it does not assert that every pointer
matched or that every optional interface was supported.

On 2026-11-12, the baseline and proxy runs produced the same address-free
observations on Highball's CrossOver 26.3 r30 runtime. All 23 successful device
queries and their 23 `IUnknown` round trips returned the `CreateDevice`
interface, canonical identity and vtable within their respective process; the
recovered bound texture shared its parent identity. Unsupported `GetContainer`
and `QueryInterface` calls are recorded alongside successes in the
[benchmark](../benchmarks/d3d8/2026-11-12-com-identity/result.json).

`same_interface` compares the returned `IDirect3DDevice8*` with the device
returned by `CreateDevice`; `same_iunknown` compares their `QI(IUnknown)`
identities; `same_vtable` compares the current interface vtable pointers.
`GetContainer` reports whether its canonical identity matches the device or
its known parent. Where the container exposes a device, the fixture also
measures that device interface directly. Each returned device is also tested
through `QI(IUnknown)` followed by `QI(IDirect3DDevice8)`. Each attempted call
records its HRESULT; unsupported formats, interfaces or optional resources are
reported, not silently treated as successful. Addresses are process-local and
cannot be compared across runs.

These measurements apply only to the selected Wine/runtime, adapter, resource
types and execution. Equal pointers or vtables do not prove interception of
all COM aliases or establish behavior on Windows or inside TDS. In particular,
no calls pass a wrapped VB/IB through a recovered device alias. Resource alias
round trips via `IDirect3DResource8` or `IDirect3DBaseTexture8`, vertex/index
buffers, private data, reset, state blocks, multithreaded lifetimes, unknown
game paths and gameplay remain untested.
