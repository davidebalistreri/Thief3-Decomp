# ShellExecuteExA environment probe

This fixture starts two successive x86 child processes using
`ShellExecuteExA`. Each records five known capture-related environment values
in its own new file. The runner verifies all three snapshots exactly and checks
that no capture file was created. No game, proxy or Steam process is started.

On 2026-11-12 this passed on Highball CrossOver 26.3 r30/Wine: the marker,
capture path, `KEY=F11`, `BUFFERS=1`, and absence of `FRAME` survived both
synthetic launches. See the [result](../benchmarks/runtime/2026-11-12-shell-environment/result.json).
This does not establish whether Steam or the real Ion Launcher preserves an
environment supplied by the host, or whether a level actually loaded.
`BUFFERS=1` is used here only as a string; the experimental buffer observer
must not be enabled in TDS until its COM resource boundary is safe.

Build and run from `t3sdk` with an installed i686 MinGW-w64 compiler, an
existing Highball engine/bottle and a new ASCII output directory:

```sh
i686-w64-mingw32-g++ -std=c++17 -Wall -Wextra -Werror -static \
  -static-libgcc -static-libstdc++ t3render/tests/shell_environment_fixture.cpp \
  -lshell32 -o /private/tmp/t3render-shell-environment-fixture.exe
python3 -B t3render/tools/run_shell_environment_probe.py \
  --engine '/absolute/path/to/Highball/engine' \
  --bottle '/absolute/path/to/Highball/bottle' \
  --fixture /private/tmp/t3render-shell-environment-fixture.exe \
  --output-dir /private/tmp/t3render-shell-environment-result
python3 -B -m unittest t3render/tests/test_shell_environment_probe.py -v
```

The output directory must not already exist. The runner never alters saved
Highball settings, though Wine may update its own bottle state. Remove the
fixture executable and output directory when they are no longer needed.
