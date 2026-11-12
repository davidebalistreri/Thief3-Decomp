# D3D8 state blocks and WRITEONLY buffers

This isolated fixture compares native D3D8 against the actual T3Render proxy
with experimental buffer observation enabled. It never launches TDS or loads
game assets. It creates MANAGED WRITEONLY vertex/index buffers, exercises
`CreateStateBlock`, `BeginStateBlock`, `EndStateBlock`, `ApplyStateBlock`,
`CaptureStateBlock` and `DeleteStateBlock`, then compares restored binding
identities and a red indexed triangle. A separate state block retains buffers
after the application's wrapper references have been released.

The current proxy intentionally invalidates buffer observations at every
state-block call. The runner therefore requires `buffer_stable=false` and
`window_unavailable` for both buffer components; equal RGB PPMs demonstrate
only the selected fixture's rendering, not safe replayable shadow bytes.
The operations are exercised together; the experiment does not isolate each
operation's effect, establish concurrency safety, or validate unknown COM
aliases, resets, other runtimes, TDS or gameplay.

On 2026-11-12, the Highball baseline and proxy runs passed and produced equal
RGB PPMs; the conservative trace contained unavailable buffer windows. See
the [benchmark manifest](../benchmarks/d3d8/2026-11-12-stateblock-buffers/result.json).

With the existing i686 MinGW build and a Highball bottle, from `t3sdk`:

```sh
T3RENDER_STATEBLOCK_TMP=$(mktemp -d /private/tmp/t3render-stateblock.XXXXXX)
i686-w64-mingw32-g++ -std=c++17 -Wall -Wextra -Werror -municode -static \
  -static-libgcc -static-libstdc++ t3render/tests/d3d8_stateblock_buffer_fixture.cpp \
  -ld3d8 -ldxguid -luser32 -o "$T3RENDER_STATEBLOCK_TMP/fixture.exe"
python3 -B t3render/tools/run_d3d8_stateblock_fixture.py \
  --engine '/absolute/path/to/Highball/engine' \
  --bottle '/absolute/path/to/Highball/bottle' \
  --fixture "$T3RENDER_STATEBLOCK_TMP/fixture.exe" \
  --proxy '/absolute/path/to/t3sdk/build/t3render-mingw/d3d8.dll' \
  --output-dir '/absolute/path/to/new/output-directory'
rm -r "$T3RENDER_STATEBLOCK_TMP"
python3 -B -m unittest t3render/tests/test_d3d8_stateblock_runner.py -v
```

The fixture, proxy and output paths must be absolute, and the output directory
must be new and outside the binaries, engine and bottle. Wine may update its
bottle state; the runner does not modify the TDS installation.
