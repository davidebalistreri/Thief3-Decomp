# Synthetic pre-Reset declaration hypothesis fixture

This optional comparison extends the isolated [shader Reset fixture](README_shader_reset_fixture.md).
It runs its builtin/proxy reference first, then enables
`T3RENDER_CAPTURE_DECLARATION_HISTORY=1` together with declaration capture on
two additional synthetic processes. It does not launch TDS or use game assets.

In `valid-only`, a successful Reset followed by an explicit successful shader
rebind must produce a separately labeled pre-Reset hypothesis. The ordinary
declaration remains `unavailable` with null words. In `recovery`, a failed
Reset discards the retained data, so no hypothesis may appear after recovery or
the later successful Reset. The test validates the raw records, paired draw
result and normalized offline IR, while comparing HRESULTs and RGB images with
the builtin run. A failed shader rebind or incomplete reference remains
`PARTIAL`. This experiment does not observe the failed Reset in isolation.
If the deliberately invalid Reset unexpectedly succeeds, the fixture may
retain hypotheses as after any successful Reset, but the result remains
`PARTIAL` and does not claim failed-Reset coverage.

A hypothesis is application memory sampled **before** Reset: it is not proof
of shader identity, driver input, or gameplay safety. Aliases, concurrent memory
mutation and unobserved device calls can invalidate assumptions. This fixture
does not establish native Windows behavior, nor authorize enabling buffer
wrappers or declaration history in TDS.

On 2026-11-12, the Highball CrossOver 26.3 r30 run passed: `valid-only`
produced two hypotheses while ordinary observations stayed unavailable; after
the failed Reset and recovery, no hypothesis reappeared. HRESULTs and RGB
images matched the reference modes. The [benchmark manifest](../benchmarks/d3d8/2026-11-12-declaration-history/result.json)
records the tested binary and exact source hashes. Local logs, traces and one
copy of each binary are in `build/t3render-diagnostics/2026-11-12-declaration-history`.
This is fixture-specific evidence and does not observe a failed Reset alone.

Use the x86 fixture build from the linked README and an idle Highball bottle:

```sh
python3 -B t3render/tools/run_d3d8_shader_reset_history_fixture.py \
  --engine '/absolute/path/to/Highball/engine' \
  --bottle '/absolute/path/to/Highball/bottle' \
  --fixture '/absolute/path/to/fixture.exe' \
  --proxy '/absolute/path/to/t3sdk/build/t3render-mingw/d3d8.dll' \
  --output-dir '/absolute/path/to/new/output-directory'
python3 -B -m unittest t3render.tests.test_d3d8_shader_reset_history_fixture -v
```

The paths must be absolute and the output directory new and outside the
binaries, bottle and engine. Wine may update its bottle state; the runner keeps
partial logs if a fixture process fails.

Append `--vertex-programs` to additionally check a separate pre-reset VS 1.1
program hypothesis against the fixture input. A failed Reset/recovery must
still suppress it; it is never promoted to a post-reset program candidate.
