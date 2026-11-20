# Remaster v0.2 benchmark

For a packaged fixed-camera bedroom v0.1 benchmark and its opt-in live mode,
see [t3bench](README_t3bench.md). The reconstruction below remains a separate
historical visual-quality experiment.

The benchmark reconstructs the recorded v0.1 bedroom pose without launching
TDS. Its inputs are the recorded callback trace, viewer telemetry and the
unchanged v10 catalog. Actor/resource bindings remain candidates; the OFF
image is simulated. It is not a replacement for a live candle-switch test.

For the capture `remaster-v01-live-20261120-010825`, generate a standalone
fixture outside the repository:

```sh
python3 -B t3render/tools/reconstruct_live_benchmark.py \
  --trace build/t3render-captures/remaster-v01-live-20261120-010825/bedroom.jsonl \
  --telemetry build/t3render-captures/remaster-v01-live-20261120-010825/scene-live-live-b1f4a4814fcf8530e324865b4624a0f6.jsonl \
  --catalog build/remaster-v01/bedroom-v01-door-crates.bin \
  --trace-frame 1406 --lighting-frame 1411 --output /private/tmp/bedroom-benchmark.cpp
i686-w64-mingw32-g++ -std=c++17 -O2 -municode -static -static-libgcc -static-libstdc++ \
  -It3render/src /private/tmp/bedroom-benchmark.cpp \
  build/t3render-mingw/CMakeFiles/t3render_live_dx12_fixture.dir/src/scene_live_dx12.cpp.obj \
  build/t3render-mingw/libt3render_scene_live.a -ld3d12 -ldxgi -ldxguid -luser32 -ladvapi32 \
  -o /private/tmp/bedroom-benchmark.exe
```

The source object and library must be built from the current renderer first.
Run the fixture with Wine/D3D12 and arguments `catalog on.ppm flat.ppm off.ppm
0`; append `gi` to enable the screen-space bounce. Append `warm` or `cool`
to isolate the candle or doorway direct lighting, and `wall`, `door` or `none`
to isolate bounce sources. The three-frame timing includes readback and is not
a live performance benchmark.

For the wall experiment add `--wall-height-materials
build/assets/godot/remaster/garrett_home/materials --height-output
/private/tmp/bedroom-wall-height.rgba` when generating. Pass modes `1`, `2`,
or `3` instead of `0`, followed by the raw height path, for PBR, POM or
displacement. The fixture attaches this data to an in-memory material copy;
it does not modify the golden catalog. Compare the resulting images and
triangle counts before choosing a mode.

The v0.2 preview uses the generated OpenGL normal maps for stone, wood,
wool and linen. The original diffuse-only materials do not inherit the wood
normal. Candlestick metal has a separate roughness and metallic response.
The doorway fill and indirect-light gains are artistic; the screen-space
bounce can only use visible selected surfaces and cannot reconstruct off-screen
lighting. `--preview-gi` with the v10 catalog and remaster toggle is available
for a subsequent live run; validate that run with `compare_live_v01.py
<directory> --version 0.2`.
