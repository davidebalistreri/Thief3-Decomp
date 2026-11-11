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

## Scope and next steps

For each observed draw, the snapshot includes its parameters, available
camera/world/texture matrices, viewport, render and texture-stage states,
material, a bounded set of lights, texture and buffer bindings/descriptors,
and shader handles. The header records the exact limits. IDs are transient
process addresses, not stable T3 asset names. It does not include texture or
buffer bytes, UP geometry, shader code/declarations/constants, all light or
world indices, render-target operations, resource lifetime, or the outcome of
draw calls. Snapshot order under concurrent device calls is not guaranteed to
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

The existing Godot bedroom benchmark is useful visual reference; its
“Original” images are a Godot export, not captures from Legacy D3D8. A
permanent in-game benchmark still needs a repeatable save/camera, frame images,
hardware and driver metadata, and GPU timings. `GarrettsBed` is a possible
level entry point, not yet verified at runtime.
