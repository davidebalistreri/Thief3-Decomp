# Synthetic D3D8 capture to D3D12 replay

This experiment connects a captured D3D8 draw to a D3D12 offscreen draw. It
is deliberately limited to isolated synthetic fixtures, not a TDS frame or
a general replay backend. The D3D8 fixture has these UP modes:

- Screen-space `XYZRHW|DIFFUSE` vertices produce a `T3R_UP_V1` packet.
- `--camera` uses `XYZ|DIFFUSE` vertices and nonidentity world, view and
  perspective projection matrices. Its `T3R_XYZ_V1` packet preserves the
  captured row-major matrices; the D3D12 vertex shader applies them in D3D8
  row-vector order, followed by a half-pixel adjustment.
- `--multi-camera` draws two overlapping, flat opaque `XYZ|DIFFUSE`
  triangles in order with different world, view and projection matrices for
  each draw. Its `T3R_MULTI_XYZ_V1` packet preserves that order and each
  matrix. Depth and blending are disabled.

The proxy records vertex bytes and state; the fixture-specific converter
validates the trace and selects the packet format from the observed FVF.

On 2026-11-12 the 256×256 reference D3D8 output, D3D8 output with capture, and
D3D12 replay output had identical PPM RGB bytes on the tested Highball runtime.
All three runs reported the same RGBA FNV-1a hash. See the [result manifest](../benchmarks/d3d12/2026-11-12-up-replay/result.json),
[input packet](../benchmarks/d3d12/2026-11-12-up-replay/fixture.txt), and
[matching image](../benchmarks/d3d12/2026-11-12-up-replay/triangle.png).
The image is the losslessly converted reference; it is not an in-game screenshot.

The second, camera-mode experiment produced matching RGB PPMs for the D3D8
baseline, captured D3D8 run and D3D12 replay on the same runtime. Its
[manifest](../benchmarks/d3d12/2026-11-12-up-camera/result.json),
[packet](../benchmarks/d3d12/2026-11-12-up-camera/fixture.txt) and
[image](../benchmarks/d3d12/2026-11-12-up-camera/triangle.png) record that
separate result. The input has a nontrivial perspective and distinct
noncommuting transforms; this exercises matrix order and the perspective divide,
but it does not prove general camera or clipping parity.
In a separate sensitivity check, increasing the VIEW X translation by 0.2 in a
temporary packet changed 4,810 pixels. That mutated packet and its output were
discarded; the check is recorded separately from the retained parity evidence.

A separate `--buffered` fixture draws the same screen-space triangle with
`DrawIndexedPrimitive`, partially written vertex and index buffers, and
nonzero base vertex, minimum vertex and start index. Its D3D8 reference and
proxy images matched on the tested runtime; see the
[buffered fixture result](../benchmarks/d3d8/2026-11-12-buffered/result.json).
The host runner's `--buffered` mode compares the two D3D8 images and validates
the indexed draw, buffer bindings and offsets in the trace. Separate opt-in
experiments can replay fixture-owned snapshots or a bounded proxy observation.

The [multi-camera benchmark](../benchmarks/d3d12/2026-11-12-multi-camera/result.json)
also produced identical D3D8 baseline, proxy-captured D3D8 and DX12 RGB images
on the tested Highball runtime. The runner validates the exact known fixture's
two draws, vertices, colors and matrices before replay. Separate changes to
the order and each second-draw transform changed replay pixels, as documented
in the manifest. The converter can represent 2–32 ordered camera draws, but
requires fixture attestation and cannot certify unobserved D3D8 operations.

The screen-space and camera UP replays do not exercise indexed draws. None of
these fixture replays tests depth, textures, game shader programs, lighting,
PBR, or TDS gameplay. The pixel-center adjustment is
tested only against these fixtures and this runtime. The
trace contains observations, not all possible D3D8 operations; the converter
requires explicit `--synthetic-fixture` attestation and rejects unsupported
observed state, but cannot make arbitrary captures safe to replay.

A later [combined resource-facade fixture](../tests/README_resource_facade.md)
captures a single indexed draw with VB/IB shadows, one programmable vertex
shader and constants in the same trace, together with a synchronized D3D8
image. Its fixture-specific conversion and DX12 comparison matched RGB on
Highball. It recognizes exactly one two-instruction shader and is not a general
shader translator or replay of TDS.

## Build and host tests

From the `t3sdk` directory, with an installed i686 MinGW-w64 compiler and
Ninja:

```sh
cmake -S t3render -B build/t3render-mingw -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++ \
  -DT3RENDER_BUILD_D3D8_UP_FIXTURE=ON \
  -DT3RENDER_BUILD_D3D12_REPLAY_UP=ON
cmake --build build/t3render-mingw
python3 -B -m unittest t3render/tests/test_trace_to_up_packet.py \
  t3render/tests/test_trace_to_camera_packet.py \
  t3render/tests/test_d3d12_replay_parser.py -v
```

The fixture binary is built under `d3d8_up_fixture/`, away from the proxy
`d3d8.dll`. The output is a Windows x86 executable and requires a compatible
D3D12 runtime and `d3dcompiler_47.dll`.

## Repeat on Highball

The host runner executes the fixture without and with the proxy, converts the
captured frame, runs the D3D12 replay, and compares their images. Supply an
existing Highball engine and bottle, the absolute x86 build directory and a
new absolute output directory:

```sh
python3 -B t3render/tools/run_synthetic_up_replay.py \
  --engine '/absolute/path/to/highball/engine' \
  --bottle '/absolute/path/to/highball/bottle' \
  --build-dir '/absolute/path/to/t3sdk/build/t3render-mingw' \
  --output-dir '/absolute/path/to/new/output-directory'
```

This uses Wine and the bottle; it does not test launching TDS. Inspect its
result manifest and logs along with the images. On a different runtime,
compare results and image differences before inferring any parity failure.
Add `--camera` and use another new output directory to exercise the second mode.
Add `--multi-camera` instead to capture and replay the two-draw fixture; its
manifest validates both colors and the green overlap at the center.
For the indexed buffer capture test, use a new output directory and pass
`--buffered` instead. Without an additional replay option, the manifest records
`dx12_replay: not_run` and no replay executable is required.
`--camera`, `--multi-camera` and `--buffered` are mutually exclusive.

With a new output directory, add `--buffered --proxy-buffer-replay` to replay
the proxy's observed WRITEONLY application writes. The runner passes
`--writeonly-buffers` to both fixture processes, enables
`T3RENDER_CAPTURE_BUFFERS=1` only for the capture process, validates the trace,
converts its stable indexed draw to an UP packet and compares three images.
Its manifest records `proxy_application_lock_shadow`; no fixture snapshot is
used. This test requires an isolated fixture attestation and is not a TDS frame
or a general replay. `--proxy-buffer-replay` excludes fixture readback, fixture
wrappers and fixture snapshot replay. See the [proxy capture benchmark](../benchmarks/d3d12/2026-11-12-proxy-writeonly-replay/result.json).

**Do not enable experimental buffer capture in TDS yet.** Unobserved device
aliases obtained through other resources may bypass unwrapping and pass a
wrapper to Wine, causing an assertion or crash. COM identity, state-block
lifetime and all alias paths are not yet covered. See the main README's
experimental buffered capture section for limits.

For the buffered fixture only, add `--verify-buffer-readback` to have both
fixture runs use read locks and compare the known written vertex/index ranges
with their expected bytes. The runner requires a PASS with the expected ranges
in each log and records it in the manifest. These are driver calls: locks can
stall or fail on other resources or runtimes. The bytes are not serialized in
the capture trace, and this option alone does not replay them. See the
[managed readback result](../benchmarks/d3d8/2026-11-12-managed-readback/result.json)
for the tested runtime and image hashes.

Instead of `--verify-buffer-readback`, pass `--wrapped-buffers` with
`--buffered` to exercise fixture-owned wrappers around MANAGED, WRITEONLY
vertex and index buffers. The fixture observes its application writes and
unwraps the real resources at the device boundary. The runner requires both
write and lifetime PASS markers in each log and checks both buffer descriptions
in the proxy trace. This wrapper is isolated to the fixture,
not the proxy, and is not a general COM replacement or a test of TDS.

Add `--replay-buffer-snapshot` together with `--buffered` and either
`--verify-buffer-readback` or `--wrapped-buffers` to replay the fixture-owned
snapshot on DX12. The two buffer methods are mutually exclusive. The converter
resolves 16-bit indices, base vertex and offsets into a single UP triangle. It
requires an explicit fixture attestation: the sidecar contains no independently
verified render state or origin and is not a proxy trace. The manifest records
which fixture method produced the snapshot. See the [readback snapshot replay
result](../benchmarks/d3d12/2026-11-12-buffer-snapshot-replay/result.json) and
[WRITEONLY wrapper replay result](../benchmarks/d3d12/2026-11-12-wrapped-writeonly-replay/result.json).

An experimental buffer-vtable replacement failed a Wine assertion and was
discarded. The [failure manifest](../benchmarks/d3d8/2026-11-12-buffer-tracking-failure/result.json)
records the isolated test and its limits; it did not modify or launch TDS.

## Replay the saved packet on Windows

From PowerShell at the `t3sdk` directory, with the binaries built as above:

```powershell
$run = Join-Path $env:TEMP ("t3render-up-" + [guid]::NewGuid())
New-Item -ItemType Directory $run | Out-Null
& .\build\t3render-mingw\t3render_d3d12_replay_up.exe `
  .\t3render\benchmarks\d3d12\2026-11-12-up-replay\fixture.txt `
  --output (Join-Path $run 'replay.ppm')
if ($LASTEXITCODE -ne 0) { throw 'Replay failed' }
Get-FileHash -Algorithm SHA256 (Join-Path $run 'replay.ppm')
```

The replay creates a new file and will not overwrite an existing image. The
saved PPM hash in the manifest is useful on the tested runtime; do not assume
identical edge pixels on a different GPU, driver or translation layer. Remove
the new temporary directory with `Remove-Item -Recurse $run` after inspection.
Pass the camera-mode `fixture.txt` path to replay the captured matrices instead.

For a fresh capture, run the D3D8 fixture once without the proxy, then in a
separate directory containing a copy of the fixture and the proxy. Set
`T3RENDER_CAPTURE_FRAME=0` and `T3RENDER_CAPTURE_PATH` to a new absolute
Windows path for the latter process. Add `--camera` to both fixture runs to
capture the camera-mode fixture. Convert the resulting complete trace:

```sh
python3 -B t3render/tools/trace_to_up_packet.py capture.jsonl \
  --synthetic-fixture -o packet.txt
```

Replay that packet and compare the three PPM files on the same runtime. The
converter will not overwrite an existing packet. Capture files can contain
transient process addresses and should remain local.
