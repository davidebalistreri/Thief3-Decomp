# Isolated D3D8 buffer-alias fixture

This fixture tests COM paths that can return an `IDirect3DDevice8` while VB/IB
wrappers are live. It uses synthetic data only. **Do not enable the buffer
observer in TDS:** this test does not establish complete alias coverage or
safe gameplay. Native Windows has not been tested.

The fixture first draws a red triangle through the original device. The proxy
trace must show stable, captured WRITEONLY VB and IB bytes for that control draw;
requesting the observer alone is insufficient. It then obtains a device from
one selected COM path, checks the interface pointer, unchanged vtable, canonical
`IUnknown` and `IUnknown → IDirect3DDevice8` round trip. A different device
pointer is reported as `unsafe_alias`; the fixture does **not** pass wrappers
through it. For a verified same-pointer path it passes the wrappers through
`SetStreamSource`, `SetIndices` and `ProcessVertices`, checks retrieved buffer
identities, clears the target to black, and draws again. `ProcessVertices` may
fail on a runtime: its HRESULT is retained, compared to baseline and reported as
partial evidence. The fixture does not inspect its output buffer; even a
successful HRESULT does not prove the transformed bytes are correct. The second
clear is verified by its D3D result and trace, without intermediate readback.

The 20 cases cover device QI; VB/IB `GetDevice` and `IUnknown` round trips;
texture, resource, base-texture and bound-texture `GetDevice`; texture-level
surface `GetDevice` and `GetContainer`; backbuffer; cube/face; volume
texture/level; and additional-swapchain backbuffer. Failed acquisitions are
recorded with the stage and HRESULT; they do not prove the path is unsupported.
Baseline and proxy run in separate processes with the same
fixture; the runner compares the emitted HRESULTs, identity checks and RGB pixels,
validates native DLL loading and the proxy's bounded control capture, and
preserves logs and traces. `PASS` requires all cases exercised, successful
`ProcessVertices`, and stable, verified VB/IB bytes on the second draw;
otherwise incomplete evidence is `PARTIAL` (exit code 3). A failure stops the
run and keeps partial diagnostics.

This is a runtime-specific smoke test. It does not test distinct aliases,
unrecognized interfaces, private data, reset, concurrent lifetimes, state
blocks, alpha or native Windows behavior. Equal pointers in a fixture do not
guarantee interception of future aliases. COM reference counts are not assumed
to be meaningful; the fixture checks returned identities and balances its own
references. State-block lifetime evidence remains in the separate
[state-block experiment](README_stateblock_experiment.md).

## Highball observation (2026-11-12)

On the idle Games bottle using Highball x64-crossover26.3-r30, the 20-case
synthetic run returned `PARTIAL`. The 18 acquired aliases had the original
device pointer and vtable, successful `ProcessVertices` HRESULTs, and matching
baseline/proxy RGB images. All 20 control draws captured the expected WRITEONLY
VB/IB bytes. The four VB/IB `GetDevice` and round-trip cases then reported
`buffer_stable=false` and `window_unavailable` on the alias draw: the wrapper
intentionally invalidates observation after a successful `GetDevice`, even for
a same-pointer alias. Two surface-container acquisitions failed with
`0x80004002` in both modes; this does not establish general interface support.

The fixture binary was SHA-256 `06cb7ecc00910e2212f5cdd70ef8b8861144e41a4fc7cb205a6dc0f187387d78`;
the proxy copy used throughout was
`be7a3882694208d381ba26fdd3cd941ed76e28f8d139b0eca04edd470c22e4aa`
(commit `996e878`). The logs, raw images, traces and initial result manifest are
under `build/t3render-captures/buffer-alias-isolated-20261112-review-1`.
The manifest's initial `PARTIAL` result was caused by the two failed
acquisitions; the runner now also treats unstable alias draws as partial. A
[tracked summary](../benchmarks/d3d8/2026-11-12-buffer-alias-highball/result.json)
records the outcome and SHA-256 of each log, image and trace.

Compile using an installed i686 MinGW-w64 toolchain, from `t3sdk`:

```sh
i686-w64-mingw32-g++ -std=c++17 -Wall -Wextra -Werror \
  -DWIN32_LEAN_AND_MEAN -DNOMINMAX t3render/tests/d3d8_buffer_alias_fixture.cpp \
  -municode -static -static-libgcc -static-libstdc++ -ld3d8 -ldxguid -luser32 \
  -o /private/tmp/t3render-d3d8-buffer-alias-fixture.exe
python3 -B -m unittest t3render/tests/test_d3d8_buffer_alias_runner.py -v
```

Only when TDS is closed and the bottle is idle, use existing binaries and a
new output directory outside the engine, bottle and binary directories:

```sh
python3 -B t3render/tools/run_d3d8_buffer_alias_fixture.py \
  --engine '/absolute/path/to/Highball/engine' \
  --bottle '/absolute/path/to/Highball/bottle' \
  --fixture /private/tmp/t3render-d3d8-buffer-alias-fixture.exe \
  --proxy '/absolute/path/to/t3sdk/build/t3render-mingw/d3d8.dll' \
  --output-dir '/absolute/path/to/new/output-directory' --fixture-only
```

The runner never edits the game installation or Highball settings; Wine may
update its bottle state. It keeps one copied fixture per mode and one proxy,
rather than duplicating the binaries for every case. Output remains available
after failure for diagnosis.
