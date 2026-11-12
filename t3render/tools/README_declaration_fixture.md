# Synthetic D3D8 declaration fixture

This x86 fixture creates two vertex shaders with the same position/color
declaration, binds and queries each handle, draws a triangle, restores FVF and
deletes the shader. Its default `program` mode uses a minimal vs_1_1 program;
`declaration-only` explicitly tests `CreateVertexShader` with a null function.
The two modes are different experiments: a failed program run must not be
reported as a programmable-shader success if declaration-only works.

The runner compares three separate processes: builtin D3D8, the proxy with
declaration observation off, and the proxy with it on. It checks all recorded
HRESULTs and RGB PPM bytes, two nonempty differently colored triangles, and
the opt-in trace's draw order, shader handles, declaration DWORDs, generation
and result stability. It creates a manifest with hashes, retaining partial
artifacts and a failure reason if a step fails. It never launches TDS or
loads game assets.

On 2026-11-12, `program` passed on Highball CrossOver 26.3 r30: 33 HRESULT
observations and the RGB image matched across all three runs, and both captured
declarations were paired with stable draw results. The [benchmark manifest](../benchmarks/d3d8/2026-11-12-declaration-observer/result.json)
records source and binary hashes. Local logs, traces and the fixture binary are
in `build/t3render-diagnostics/2026-11-12-declaration-observer`; this directory
is ignored by Git. The null-function mode was not tested.

From `t3sdk`, using a built x86 proxy and a Highball engine/bottle:

```sh
T3RENDER_DECLARATION_TMP=$(mktemp -d /private/tmp/t3-declaration.XXXXXX)
i686-w64-mingw32-g++ -std=c++17 -Wall -Wextra -Werror -municode -static \
  -static-libgcc -static-libstdc++ t3render/tests/d3d8_declaration_fixture.cpp \
  -ld3d8 -ldxguid -luser32 -o "$T3RENDER_DECLARATION_TMP/fixture.exe"
python3 -B t3render/tools/run_d3d8_declaration_fixture.py \
  --engine '/absolute/path/to/Highball/engine' \
  --bottle '/absolute/path/to/Highball/bottle' \
  --fixture "$T3RENDER_DECLARATION_TMP/fixture.exe" \
  --proxy '/absolute/path/to/t3sdk/build/t3render-mingw/d3d8.dll' \
  --output-dir '/absolute/path/to/new/output-directory'
rm -r "$T3RENDER_DECLARATION_TMP"
python3 -B -m unittest t3render.tests.test_d3d8_declaration_runner -v
```

For the separate null-function experiment, repeat with a new output directory
and append `--mode declaration-only`. Paths must be absolute; output must be a
new directory outside the binary, bottle and engine directories. Wine may
update its bottle state. The fixture covers no buffer wrappers, state blocks,
reset, alias escape, concurrency, alpha, native Windows or TDS. The trace is a
candidate observation, not a replay or proof of driver-consumed bytes.
