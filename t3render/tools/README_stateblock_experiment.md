# Test-only state-block mutation windows

This experiment builds a separate D3D8 proxy that brackets the six state-block
methods with `BeginBindingMutation`/`EndBindingMutation` instead of permanently
invalidating shadows. The ordinary `t3render` target still uses the original
conservative policy. The experimental DLL is **only for synthetic tests and
must never be installed in or used with TDS**.

The host mock invalidates observations when a state-block call overlaps an
observed Lock or Unlock, in either order. This is not a safe COM boundary:
resource/device aliases can bypass unwrap or lock observation, and the mock
does not establish concurrent safety on a real runtime. Failed state-block
calls also retain shadows in the isolated model, a behavior that needs runtime
evidence. No claim of safe gameplay is made.

The eight independent cases cover a control, Create, Begin/End recording,
Apply, Capture, Delete, state-block-held resources after wrapper release, and
application getter reacquisition. The last two exercise distinct outcomes:
untracked native resources should stay `unknown`; reacquisition should poison
the window. For each case the runner starts separate native, production-proxy
and test-proxy fixture processes, checks exact DLL loading, HRESULT/identity
markers, WRITEONLY buffer bytes and PPM equality. It checks expected conservative
and experimental statuses; a mismatch fails instead of being presented as
success. It does not launch TDS, mutate its installation, or alter saved
Highball configuration. Wine may update its own bottle state.

On 2026-11-12 all eight cases passed in Highball CrossOver 26.3 r30: each ran
against the native runtime, the production proxy and the test-only proxy, with
equal RGB images. The control captured VB/IB in both proxies. For the five
ordinary state-block cases, production reported unavailable shadows while the
test-only proxy captured the expected bytes. For retained resources the
test-only proxy reported unknown bytes; reacquiring them invalidated the window.
The [manifest](../benchmarks/d3d8/2026-11-12-stateblock-mutation-windows/result.json)
records the binary, trace and log hashes and the limitations. These are serial
synthetic Wine results, not TDS or native Windows evidence.

An initial run was recorded as **FAIL** because Wine logged the proxy using a
`D:` alias while the runner expected `Z:`. The corrected runner verifies file
identity through the bottle's drive mappings; the eight-case retry passed.
Both runs' full local logs remain under `build/t3render-captures/`.

With an existing i686 MinGW build, an idle Highball bottle and a new output
directory, from `t3sdk`:

```sh
cmake -S t3render -B build/t3render-mingw -DT3RENDER_BUILD_STATEBLOCK_TEST_PROXY=ON
cmake --build build/t3render-mingw --target t3render_stateblock_test_proxy t3render_stateblock_test_fixture
python3 -B t3render/tools/run_d3d8_stateblock_experiment.py \
  --engine '/absolute/path/to/Highball/engine' \
  --bottle '/absolute/path/to/Highball/bottle' \
  --fixture '/absolute/path/to/t3sdk/build/t3render-mingw/stateblock_test_fixture/t3render_stateblock_test_fixture.exe' \
  --production '/absolute/path/to/t3sdk/build/t3render-mingw/d3d8.dll' \
  --experiment '/absolute/path/to/t3sdk/build/t3render-mingw/stateblock_test_proxy/d3d8.dll' \
  --output-dir '/absolute/path/to/new/output-directory' --fixture-only
python3 -B -m unittest t3render/tests/test_stateblock_experiment_runner.py \
  t3render/tests/test_stateblock_experimental_proxy.py t3render/tests/test_proxy.py -v
```

All paths must be absolute; output must not exist and must lie outside the
engine, bottle and input binary directories. The runner may retain logs,
traces and fixture images in the new output directory after failure.
