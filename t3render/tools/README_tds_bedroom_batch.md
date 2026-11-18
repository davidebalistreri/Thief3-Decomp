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

For a coordinated live comparison, `--steam-launch --manual-trigger --delay 0`
starts the installed Steam game and waits until `capture.request` appears in
the new output directory. Create that marker only after the bedroom is loaded
and the requested camera movement is complete. The runner checks that exactly
one TDS process is present before creating its separate `capture.ready` marker.
The normal timeout still applies; a request is not proof of the map or pose.
Thief may restart its process while loading a level. The runner allows that
before the trigger, waits for one process, and binds its PID and start time for
the capture. `batch.json` records whether that process differs from the first
one, or `null` if no process was bound at the trigger. If no successor appears
for 30 seconds, it stops waiting. It also
watches the Wine log for an unhandled exception on the thread that loaded the
exact T3Main executable, provided that load is unambiguous. Multiple game
loads, worker-thread crashes and Steam dialogs can remain undetected. In
isolated mode cleanup stops only the private wineserver; outside that mode a
still-running game before the trigger is left for the operator to close, and
the proxy is retained until safe cleanup.

For M1, `trigger_live_m1.py` can create the existing `capture.request` while
the player is still moving, without switching to another application. Start it
in a separate terminal before launching the runner with `--manual-trigger`:

```sh
python3 -B t3render/tools/trigger_live_m1.py --watch "$T3RENDER_OUTPUT" --timeout 360
```

The output directory may not exist yet; the runner must create it. The watcher
ignores bytes already present when it discovers a viewer log. It requires one
fresh viewer session, matching bedroom-map catalog fingerprint, consecutive
scoped-draw frames in one map/device epoch and continuity, no frame gap over
one second, and at least 30 seconds both in the viewer log and on the host
clock after movement starts. It also requires at least 30 T3 units of camera
displacement, 15 degrees of rotation, a bed exit and re-entry, and at least
2 T3 units or 1 degree of movement in the last two seconds. A gap, reset,
ambiguous active session or invalid frame
restarts the observation. The request contains the measured interval; the
runner independently checks the TDS PID and start time before capture. The
watcher does not launch the game or verify the user's location, input, GPU
pixels or complete scene. The marker triggers the existing consecutive-frame
readbacks and, when requested, the runner's normal isolated cleanup.

`--replay LOG --start-frame N --end-frame N` evaluates a saved log without
writing a marker. On the earlier TDS log, frames 1314–2444 met the gate in
30.001 seconds, with eight bed exits and eight entries. Streaming the longer
saved interval in real time produced a request after 35.649 host seconds. One
earlier stream lasting only 30.59 seconds ended without a request; the reason
was not isolated. An existing finished log, a missing frame and an accelerated
stream produced no request. In a [subsequent TDS run](../../build/t3render-captures/scene-live-m1-auto-20261118-v1/M1-live-report.md),
the watcher requested capture after 95.981 seconds and the runner captured
three linked frames. A long stationary interval delayed the request until the
player resumed moving; this is a limitation of the recent-motion gate. The
session cleaned up its isolated saves and proxy.
`--resume-bedroom` additionally passes the URL observed when the game's own
launcher resumed the bedroom. It uses `Current Save`, so its map and player
position depend on that save; Steam argument forwarding and isolated saves
have not yet been verified with this option. It requires `--steam-launch`.
On macOS, `--readonly-saves` starts the Wine session under a sandbox profile
that denies writes beneath the existing `SaveGames` directory; it requires
`--steam-launch` and refuses existing Wine/Steam processes. A Wine probe has
confirmed read access and denied write access. A real launch with both
`--resume-bedroom` and `--readonly-saves` crashed while loading a static mesh;
the cause and the effect of either option separately remain unverified. The
save files were unchanged in that run. This is a read-only guard, not a writable
isolated save profile; the game may need writes to load or update `Current Save`.

`--isolated-saves` instead creates a private temporary Wine prefix with a
writable copy of `SaveGames`, redirects the game's `SaveGamePath` registry
value to it, denies writes to the original saves, Documents and known
VirtualStore/Steam Cloud locations, and blocks external TCP/UDP while allowing
local loopback and Unix sockets. Other Steam
and bottle files may still change. It requires `--steam-launch --terminate-after-capture`, refuses
existing Wine/Steam processes and cannot be combined with `--readonly-saves`.
At cleanup it stops and waits for that private wineserver, checks the original
save hashes and removes the temporary prefix. If the server cannot be drained,
the retained directory is reported in `batch.json` and must not be removed while
the server is active. Wine probes have confirmed registry redirection, the
redirected `GetTempPathA`, writes to the temporary directory and save copy,
denial of writes to the original saves, and cleanup. An initial launch failed
because Steam could not write to its own installation; another reported
transport error `0x3000` with loopback blocked. After allowing both, Steam
started the game, but an automatic `--resume-bedroom` load crashed at address
`0x109722cf` before producing viewer frames. Manual loading and the cause of
the crash remain unverified. This is not a general isolation boundary for
every game or system file.

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
more time; `--timeout` must exceed it. With `--terminate-after-capture`, a
non-isolated run terminates TDS only if it is still the original identified
process. If TDS restarted before capture, automatic termination is withheld;
while TDS remains open, the proxy is retained and the operator must close TDS
and run cleanup. Isolated mode instead stops its private Wine session, including
Steam, without signaling a globally discovered PID.
Without `--terminate-after-capture`, close TDS normally after the terminal
reports completion. The runner will wait and remove the proxy.

Add `--scene-bridge` to request the experimental read-only Flesh probe during
the same capture. It requires the verified x86 executable and entry prologue;
the trace header records whether installation succeeded. Frame synchronization
is limited to the observed Present interval and thread; camera, pass and actor
identity require separate checks. Probe data includes transient addresses and
stays in the local trace. Frame numbers are process-local: a new capture does
not become golden frame 3377 simply because it uses the same room.

For an experimental live flat viewer, first export a local offline catalog as
described in [scene live](../ir/README_scene_live.md), then add
`--scene-bridge --submission-scope --live-catalog /absolute/path/to/catalog.bin`.
The runner supplies an output directory for each session-qualified viewer log,
up to two initial images and up to eight more after the capture trigger per
process. The viewer is partial, asynchronous, and may drop frames. Use
`compare_live_present.py` as documented in scene live to
check the shared session, explicit Present link and camera; matching numbers
alone do not establish a match. These files contain scene-derived data and
remain local.

For a material preview with the existing v3 bedroom catalog, `--preview-pbr`
enables a neutral viewer-only headlight attached to the camera. The log marks
these frames `lighting_mode=preview_headlight`; this is artificial preview
illumination, not a sampled game light or a verification of the candle. The
normal-map convention and indirect lighting are still unverified. Do not add
`--gpu-metrics` to an ordinary preview: the earlier instrumented TDS session
was much slower, and the later single-memory-sample version is not yet tested
in the game.

The opt-in `--live-lights --preview-candle` instead draws an artistic PBR light
at the matching `D_384__5` candidate's same-frame sampled position. The RGB
words are normalized as a preview color; its artistic base 1 cd intensity is multiplied
by the same-frame candidate factor sampled at `+0x8c`. A 0.12 m source and 3 m
influence range are chosen values. A zero factor omits the shadow pass. One
bounded cubemap shadow otherwise uses the selected geometry. The sampled actor's enabled state, color encoding and physical
intensity remain unknown. If that exact sample is missing or invalid, the viewer
renders a dim artistic ambient without a candle contribution and does not
reuse an earlier sample. `preview_candle_unknown` identifies this situation;
it does not assert that the light was switched off. Sections without usable
normals are black throughout this candle preview. The diagnostic mode
`preview_candle_candidate` distinguishes the sampled preview from verified lighting. This path
passed synthetic offscreen GPU checks. An earlier 5 cd version ran in [TDS](../../build/t3render-captures/scene-live-m3-preview-20261117-mac-v1/M3-live-report.md):
903 viewer frames used the candle preview and 28 fell back to unlit. The user
reported no evident slowdown; an instrumented 17.24-second interval measured
28.95 game Present entries per second. These observations do not isolate the
cost of the preview or establish physical light parameters. The later
modulation was exercised in TDS; the revised ambient fallback was checked
offscreen against a captured pose but not yet in TDS. It cannot be
combined with `--preview-pbr`.

Add `--candle-link-diagnostics` with `--live-lights` to sample the visible
bedroom candle actor's light link independently of the partial light list.
This experimental, bounded read is disabled by default. It records candidate
bits or a null link, not the enabled flag. A same-frame zero outside the light
list is labelled `preview_candle_zero_candidate` while retaining the same dim
ambient fallback; that label is not yet verified live. The reader was exercised
in TDS but its cost was not isolated;
see [probe limits](../ir/README_scene_light_probe.md).
With `--preview-candle`, this option also enables the generated metal and wax
candlestick on a stable visual-actor transform. The HDR flame requires both
samplers to agree and a positive factor. Its small HDR glow is computed from
visible emissive pixels. These additions and the new intensity were checked
offscreen, but not yet inside TDS. The replacement uses untextured artistic
colors and does not recreate the missing bedside furniture.

A bedroom catalog exported with `--wall-height` additionally opts into the
head-wall POM preview when a lit path is active. The same-frame bed anchor
and fingerprinted map restrict its selection; `candidate_head_wall_triangles`
reports candidate ranges, not proof of visible relief. The depth and normal
convention are artistic assumptions and the live result and cost remain
unverified. `--wall-displacement` switches the selected wall to the optional
geometry experiment and requires the v6 catalog. See the [offscreen comparison](../../build/t3render-captures/scene-live-m4-wall-20261118-v6/M4-wall-report.md).

`--preview-gi` enables the separate screen-space wall-to-bed bounce on a lit
viewer path and also requires the v6 catalog. It gathers only visible direct
wall diffuse light and affects only `bed_high`; its limited sampling can leak
or flicker. The [GI report](../../build/t3render-captures/scene-live-m5-gi-20261118-v4/M5-gi-report.md)
contains an offscreen A/B and controls, not a live performance result.

Add `--live-only --terminate-after-capture` to run the viewer for the specified
`--delay` without enabling the D3D8 trace. This is useful for checking viewer
behavior and responsiveness without the full diagnostic scan; it produces no
trace or synchronized D3D8 screenshot. This mode disables the viewer's two
automatic initial image readbacks; the window and per-frame diagnostics remain
active. The viewer log contains elapsed time
only for performance diagnostics, never as evidence of frame synchronization.
In a [previous capture](../../build/t3render-captures/scene-live-m1-20261117-001712/M1-report.md),
the interval between the first two viewer images was 90 ms; the median in the
later pre-capture period was 21 ms. This association does not isolate the
cost of the images from other work at startup.

With viewer diagnostics enabled, the same log now samples D3D8 Present entry
intervals using a monotonic performance counter. Counters and a bounded
frametime histogram are written by the viewer worker roughly once a second;
there is no per-Present file I/O. After a run, select a known bedroom period
using the log's elapsed milliseconds and summarize it offline:

```sh
python3 -B t3render/tools/report_live_performance.py /path/to/scene-live-session.jsonl \
  --start-ms 30000 --end-ms 90000
```

The report uses the first and last telemetry snapshots within that range and
gives the actual snapshot times, mean FPS and frametime, and histogram bounds
for median, p95 and maximum. It reports the total measured interval duration
alongside the wall-clock telemetry window: failures, overlapping Presents,
device Reset, timing errors and thread changes break the sequence and are
counted rather than bridged. Boundaries are sampled asynchronously. This is
D3D8 Present cadence with the proxy and viewer enabled, including readbacks
when they occur; it is not GPU time, an uninstrumented game baseline, proof
of movement or proof of synchronization. Capture mode can still disturb
timing with readbacks. Old logs do not contain these counters.

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
