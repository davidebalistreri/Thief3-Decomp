# Isolated WINEPATH proxy probe

This test copies the existing synthetic D3D8 UP fixture and `d3d8.dll` into
different temporary directories. The baseline loads builtin D3D8; the capture
process sets `WINEPATH` to the proxy directory and uses `d3d8=n,b`. It requires
Wine `+loaddll` to identify that exact DLL as native, validates the complete
synthetic capture and compares the RGB PPM against the baseline. Only the
newly-created scratch directory is removed; logs and results remain in a new
output directory, including on failure.

The test does not start TDS or Steam and does not enable buffer observation.
On the tested Highball CrossOver 26.3 r30 runtime it **failed**: Wine loaded
its builtin system `d3d8.dll` even with the proxy directory in `WINEPATH`.
See the [failure record](../benchmarks/d3d8/2026-11-12-winepath-proxy-failure/result.json).
This is not a supported deployment method on that runtime. Even a pass on a
different runtime would not prove that Steam or the game's launcher propagates
the environment, or that buffer wrappers are safe in TDS.

With an existing x86 MinGW build that includes
`T3RENDER_BUILD_D3D8_UP_FIXTURE=ON`, run from `t3sdk` when the bottle is idle:

```sh
python3 -B t3render/tools/run_winepath_proxy_probe.py \
  --engine '/absolute/path/to/Highball/engine' \
  --bottle '/absolute/path/to/Highball/bottle' \
  --fixture '/absolute/path/to/t3sdk/build/t3render-mingw/d3d8_up_fixture/t3render_d3d8_up_fixture.exe' \
  --proxy '/absolute/path/to/t3sdk/build/t3render-mingw/d3d8.dll' \
  --output-dir '/private/tmp/new-winepath-proxy-result'
python3 -B -m unittest t3render/tests/test_winepath_proxy_probe.py -v
```

The fixture, proxy and output paths must be absolute ASCII paths. The output
directory must not already exist, and must be outside the bottle, engine and
the directories containing the two input binaries. The runner does not alter
the game's installation or saved Highball settings; Wine may update its own
bottle state. Remove only the output directory created for this test when it
is no longer needed.
