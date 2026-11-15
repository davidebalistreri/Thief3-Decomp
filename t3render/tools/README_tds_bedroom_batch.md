# One-session TDS bedroom capture

This tool runs optional offline/synthetic checks, opens TDS once in the Wine
virtual desktop, and captures **three consecutive frames of one experimental
configuration** after a countdown. It then validates the trace and writes a
payload-free summary of the buffer, shader, Reset and draw observations. It
also requests a backbuffer image immediately before each recorded Present.
It does not capture multiple proxy configurations, identify the map or camera,
or render the room in DX12. Pre-Reset
shader history is explicitly a hypothesis, not the active program.

The countdown starts when the first TDS process appears. Load the Garrett's
Building save, face the bed, and stay in the game until it closes or the
terminal reports completion. **If the save is not loaded before the timer,
the capture is not a bedroom benchmark.** The capture cannot verify the scene;
the batch records it as `unconfirmed` until the user confirms it separately.
No switch back to Codex or hotkey is needed during the countdown.

## On this Highball installation

From `t3sdk`, with the existing MinGW build configured:

```sh
cmake --build build/t3render-mingw --target t3render_experimental_combined_proxy \
  t3render_d3d8_resource_facade_fixture t3render_d3d12_replay_up

T3RENDER_ENGINE="$HOME/Library/Application Support/Highball/engines/x64-crossover26.3-r30"
T3RENDER_BOTTLE="$HOME/Library/Application Support/Highball/bottles/Games"
T3RENDER_SYSTEM="$T3RENDER_BOTTLE/drive_c/Program Files (x86)/Steam/steamapps/common/Thief Deadly Shadows/System"
T3RENDER_OUTPUT="$PWD/build/t3render-captures/bedroom-batch-$(date +%Y%m%d-%H%M%S)"

python3 -B t3render/tools/run_tds_bedroom_batch.py run \
  --engine "$T3RENDER_ENGINE" --bottle "$T3RENDER_BOTTLE" --system "$T3RENDER_SYSTEM" \
  --proxy "$PWD/build/t3render-mingw/experimental_combined_proxy/d3d8.dll" \
  --output-dir "$T3RENDER_OUTPUT" --delay 120 --timeout 360 --frames 3 \
  --preflight \
  --fixture "$PWD/build/t3render-mingw/d3d8_resource_facade_fixture/t3render_d3d8_resource_facade_fixture.exe" \
  --dx12-replay "$PWD/build/t3render-mingw/t3render_d3d12_replay_up.exe" \
  --terminate-after-capture
```

`--preflight` runs the host test suite before the game starts. The paired
`--fixture`/`--dx12-replay` options run the existing synthetic Highball test
before TDS starts, including a comparison of the new pre-Present readback with
the fixture's direct D3D8 image; its RGB match is not evidence of game-frame parity. Omit
these options for a faster run. Increase `--delay` if loading the save needs
more time; `--timeout` must exceed it. With `--terminate-after-capture`, the
runner terminates TDS only if the process at capture time is still the same;
Steam is left running. Without that option, close TDS normally after the
terminal reports completion. The runner will wait and remove the proxy.

Add `--scene-bridge` to request the experimental read-only Flesh probe during
the same capture. It requires the verified x86 executable and entry prologue;
the trace header records whether installation succeeded. Frame synchronization
is limited to the observed Present interval and thread; camera, pass and actor
identity require separate checks. Probe data includes transient addresses and
stays in the local trace. Frame numbers are process-local: a new capture does
not become golden frame 3377 simply because it uses the same room.

The runner refuses an already-running TDS or existing `System/d3d8.dll`. It
installs the exact built DLL temporarily and removes it only after the game has
exited and its hash and file identity still match. If interrupted or if cleanup cannot be
verified, close TDS and run:

```sh
python3 -B t3render/tools/run_tds_bedroom_batch.py cleanup "$T3RENDER_OUTPUT/deployment.json"
```

The runner refuses to remove a changed DLL, a symlink, or a DLL while TDS is
running. `batch.json` records completion and cleanup; `summary.json` contains
frame counts and structural observations without game payloads or resource
addresses. The files `bedroom.jsonl.frame-N.ppm` correspond to trace frame `N`;
the `frame_image` event records success or its failing stage, dimensions,
format and whether Present used source/destination rectangles. `summary.json`
checks each file's PPM header and size and records its SHA-256. A failed
readback is not an image and an incomplete file may remain. Only non-multisampled
32-bit X8R8G8B8/A8R8G8B8 backbuffers within the configured size limit are
supported. A successful `Present` must also be checked in that frame's
`frame_end`. This is the full pre-Present backbuffer, not proof of the final
display pixels after source/destination rectangles, presentation effects or
concurrent Present calls. The trace and image share a frame number but do not
independently establish the map or camera. `bedroom.jsonl`, images and `wine.log`
remain local under the output folder.
The raw trace contains game-derived data and transient process addresses: do
not publish it without reviewing it. An optional offline IR can be produced
later with `trace_to_ir.py`; neither the summary nor IR makes a trace replayable.

## Verification status

The analyzer was checked against prior single-frame captures and the runner
was exercised with a simulated process. A [live TDS batch](../benchmarks/runtime/2026-11-14-tds-bedroom-batch/result.json)
subsequently completed three partial frames and removed the temporary proxy.
The user confirmed that run was in the bedroom; the trace cannot identify the
scene itself and the frames do not establish replay parity. A
[subsequent live capture](../benchmarks/runtime/2026-11-14-tds-provenance-screen/result.json)
recorded synchronized pre-Present images and buffer provenance. A
[partial DX12 reconstruction](README_trace_reconstruction.md) of its frame
3377 makes the room recognizable, with substantial documented differences.
The experimental proxy's menu text truncation and general COM/gameplay parity
remain unresolved.
