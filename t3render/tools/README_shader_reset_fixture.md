# Synthetic D3D8 shader Reset fixture

This x86 fixture uses a synthetic vertex declaration and vs_1_1 shader. Red
vertices render green only when the shader and its green constant are active;
the explicit fixed-function fallback remains red and yields `PARTIAL`. It
records HRESULTs, binding before and after setting the original handle,
`GetVertexShaderDeclaration`, `GetVertexShaderFunction`, and RGB images. The
fixture queries each getter's size, performs two bounded reads into buffers
filled with different sentinels, checks the tails, and queries size again. It
exposes words only if reads and sizes agree. These checks cannot prove atomic
driver behavior or prevent every possible out-of-bounds write.

Two separate process scenarios are compared across builtin D3D8, proxy with
declaration capture disabled, and proxy with it enabled:

- `valid-only`: draw, successful Reset, draw, successful Reset, draw.
- `recovery`: draw, deliberately invalid Reset, immediate valid recovery
  Reset, draw, another successful Reset, draw. After a failed Reset the fixture
  does not call device getters, render, or access resources until recovery.

The recovery scenario cannot isolate the effect of the failed Reset from the
subsequent successful Reset. If the invalid Reset succeeds instead of failing,
the result is `PARTIAL`, not evidence of failed-Reset behavior. A fallback FVF,
unavailable or unexpected shader declaration/function, failed delete, or
unavailable binding likewise gives `PARTIAL` when the fixture can still finish.
RGB differences between the initial and post-Reset draws also give `PARTIAL`,
even if baseline and proxy match each other at each individual phase.
An unsuccessful recovery, crash or timeout is a failure with retained logs.
The valid-only scenario provides independent successful-Reset observations.
The manifest marks the corresponding conclusion `inconclusive` after an error
or crash; it does not infer that the shader failed to survive. No observation
is claimed for the failed Reset alone.

The proxy trace must retain the original candidate before Reset and report
`unavailable` with `reason=reset_attempt`, `scan_status=complete` afterwards
when the original handle is used. This is an observer limitation, not a claim
that the runtime discarded the shader. The runner compares logged HRESULTs,
bindings, declaration/function bytes and RGB images across the three modes,
and validates the trace. It never launches TDS or accesses game assets. The
fixture queries the old handle only after successful recovery, in its own
process; a runtime may still reject or crash on that probe.
These two native getters are used only by this synthetic fixture; the production
proxy does not call either getter in TDS.

On 2026-11-12, both scenarios passed on Highball CrossOver 26.3 r30. A valid
Reset cleared the current vertex shader binding, but the fixture could rebind
its original handle and obtain the same 16-byte declaration, 32-byte shader
program and RGB output. The invalid Reset returned `0x8876086c`; an immediate
valid Reset recovered the device and the same observations followed. This does
not isolate the invalid Reset's effect or establish behavior on other runtimes.
The first fixture run was `PARTIAL`: this Wine getter left the supplied buffer
capacity unchanged, so it did not establish the returned token length. The
subsequent size-query and double-read run passed. See the [benchmark manifest](../benchmarks/d3d8/2026-11-12-shader-reset/result.json)
for source/binary hashes. Both runs' logs, traces and fixture binaries, plus the
tested proxy DLL, are retained locally under
`build/t3render-diagnostics/2026-11-12-shader-reset` (ignored by Git).

From `t3sdk`, using a built x86 proxy and an idle Highball bottle:

```sh
T3RENDER_RESET_TMP=$(mktemp -d /private/tmp/t3-shader-reset.XXXXXX)
i686-w64-mingw32-g++ -std=c++17 -Wall -Wextra -Werror -municode -static \
  -static-libgcc -static-libstdc++ t3render/tests/d3d8_shader_reset_fixture.cpp \
  -ld3d8 -ldxguid -luser32 -o "$T3RENDER_RESET_TMP/fixture.exe"
python3 -B t3render/tools/run_d3d8_shader_reset_fixture.py \
  --engine '/absolute/path/to/Highball/engine' \
  --bottle '/absolute/path/to/Highball/bottle' \
  --fixture "$T3RENDER_RESET_TMP/fixture.exe" \
  --proxy '/absolute/path/to/t3sdk/build/t3render-mingw/d3d8.dll' \
  --output-dir '/absolute/path/to/new/output-directory'
rm -r "$T3RENDER_RESET_TMP"
python3 -B -m unittest t3render.tests.test_d3d8_shader_reset_runner -v
```

Paths must be absolute; output must be a new directory outside binary, bottle
and engine directories. Wine may update its bottle state. This smoke test does
not verify native Windows, COM alias coverage, stealth or TDS replayability.

Add `--vertex-constants --hidden` to sample the fixture's known green `c0`
constant and compare each paired IR observation, HRESULT and RGB image while
leaving the fixture window hidden. The option may yield `PARTIAL` if the
runtime does not expose the requested registers. It does not launch TDS or
validate shader constants in a game frame.
The [2026-11-13 Highball run](../benchmarks/d3d8/2026-11-13-vertex-constants/result.json)
passed in both scenarios, with three paired `c0` samples each.
