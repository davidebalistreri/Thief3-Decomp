# T3Render: D3D8 diagnostic capture

T3Render is a separate renderer experiment. Its first step observes the
original D3D8 device; it does not replace the renderer or change the game's
stealth calculations. This is a diagnostic snapshot, **not a frame replay** or
a DX12 backend.

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

Add `-DT3RENDER_BUILD_SMOKE_TEST=ON` at configuration time to build a
standalone D3D8 program in the `smoke/` subdirectory. It draws four synthetic
triangles, checks one pixel of each and prints a backbuffer hash. Compare that
hash with and without the proxy only on the same adapter, format and runtime.
`-DT3RENDER_BUILD_D3D8_UP_FIXTURE=ON` instead adds a single-draw synthetic
fixture in `d3d8_up_fixture/`, with optional PPM readback for replay testing.

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

## Capture and inspect

Set `T3RENDER_CAPTURE_PATH` to a new absolute Windows path and select exactly
one trigger before launching the game:

- `T3RENDER_CAPTURE_KEY=F11`: on a key-down edge, capture the next frame.
  Input is observed, not consumed. Detection depends on the Present thread's
  input queue; if it does not see the key, use the numbered trigger.
- `T3RENDER_CAPTURE_FRAME=N`: capture frame `N`, counting from zero at device
  creation. A game restart or replacement device starts the count again.

An existing output file is never overwritten. Environment variables must
reach the actual `T3Main.exe` process, including across a level restart.
Without a valid path and trigger no frame is captured. Capturing a frame can
be expensive: it queries D3D8 state on every draw and writes a potentially
large file. The trace is local game-derived data and is not to be committed.

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
invalidates writable ranges at lock and commits staged bytes only after a
successful unlock; ambiguous resource identity or lock state is abandoned.
The caller must synchronize all operations and provide a fault-safe copy.
Neither model is connected to D3D8 resource locks, so buffered geometry still
cannot be recovered from a trace.

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
stable T3 asset names. Capture does not include texture or buffered-resource
bytes, shader code/declarations/constants, all light or world indices,
render-target operations or resource lifetime. Snapshot order under concurrent
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
when a frame trigger is pressed. A future D3D8 adapter should invalidate a
writable lock's range immediately, invalidate the entire buffer on DISCARD,
copy through fault-safe staging before Unlock, and mark bytes known only after
a successful Unlock. Unknown aliases, GPU writes, ambiguous locks, resets and
untracked resources must remain unknown. Resource generations, synchronization
and a process-wide memory budget are still required in the adapter. The isolated
`BufferRegistry` models generations and a shared budget, but does not itself
instrument COM objects or enforce process-wide synchronization.

The existing Godot bedroom benchmark is useful visual reference; its
“Original” images are a Godot export, not captures from Legacy D3D8. A
permanent in-game benchmark still needs a repeatable save/camera, frame images,
hardware and driver metadata, and GPU timings. `GarrettsBed` is a possible
level entry point, not yet verified at runtime.
