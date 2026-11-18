# Candidate light probe: static evidence and limits

The source build inspected is x86 `T3Main.exe`, SHA-256
`09e0be902062c53cbb1736cffa1bd3ec883bd5e864f8e7f3f99bc0d06f6ad0f8`.
The following is static analysis, not an observation of a running light.
REA/Hopper evidence IDs identify the original analysis session.

| Observation | Evidence |
|---|---|
| `AActor::m_FleshLight` is declared at `+0xb8`; `0x10979670` reads it. | `include/Engine/EngineClasses.h`; `ev_e92b232e284230a6784aa21c9c94f11f95d74721fa71379d78459c4f087d1bdb` |
| `0x1097b7d0` calls `0x1094c4a0`, assigns its result to actor `+0xb8` and writes the actor at light `+0x54`. | `ev_b9ba4d5b2da21f553fec55744fc8c6032baf34a121751b80bb6aaedc2a1b7b6b` |
| The constructor at `0x1094c4a0` writes vtable `0x10e4aa80`. | `ev_f36b3fb499e0de7738069464fbd8988bcbb48b0cb560ced8ae07694c75a7125c`; `ev_888145ce708805de6cf12445fc5bd8c273efe83c817cfd0af07bedf82e5bb462` |
| In the light cleanup branch, `0x1097afb0` clears light `+0x54` and actor `+0xb8` before conditional destruction. | `ev_99ce64640e1e5fdd6e2d7fd0d47c0b4dfc098f9b1fe98c40bc26e70d2bd8b9f3` |
| `0x1094cb10` updates fields at `+0x24`, `+0x34`, `+0x64`, `+0x68`, `+0x74..+0x90`; `0x109794a0` copies these into a descriptor. | `ev_a5ef9bc63cb9c18ae2ec6d9528fdd8a180d6d309c9587fc51394eab6efdadc6a`; `ev_1e3ef97aebb152a239e069a68ae02e80289aabcc2471505bed642373abc60f41` |
| `0x10992300` uses actor position and hardpoint inputs; property paths mention `LightColor`, `LightShape`, and `bLightOn`. | `ev_b9370e4f755fdcbd771bd6b165447f33304b37133795b0b78bf1ac09d0a615e7`; `ev_6b2c4691f9cead44d432f14f688157f6032f448b9230db84f6a6f82f3947c216` |
| Light registration adds non-type-1 objects to a renderer-owned `+0x3c` array; cleanup removes them. This is not all lights. | `ev_975072d29ddfe9cb8e07059b92768a0b8f6ed0fe65fe8ea7c8ce7f14a1b80006`; `ev_5691c034b7d4b327cd82cc23bab74aec82114454ee28ce27da13d775130d3759` |
| During culling `0x109231a0` can pass objects to `0x10925c00`, which adds unique objects to the output `+0x74/+0x7c` array. Its middle word `+0x78` is capacity in bytes. | `ev_fed492d91a86306e4f8713490004541095b5c90588e65c6a33587064e9b04df5`; `ev_bad1b36b9c19a246cbcdc8b97c4bc1695721a268fa434464e97bbc3d186aaee8`; `ev_594d3efa725823c6d51daceb3a826ae8db843270abe930b4cec63f0a328bc9ab` |

`scene_bridge_light_candidate.hpp` implements a fault-safe-reader adapter for
one actor already identified and known live by its caller. It samples the
vtable, backlink and raw words twice, checking the actor link three times.
It returns raw words only after all reads match. `NoLight` means that one read
found a null pointer, not that a complete light collection is empty. The
result contains no addresses and has no conversion to `VisualLight`.

The opt-in live adapter `T3RENDER_SCENE_LIVE_LIGHTS=1` samples the selected
renderer output list after the probe, inside `BeginLightSample` / end or cancel
on the selected thread. Present, Reset and overlapping callbacks invalidate
the interval. It checks at most 128 list entries and copies at most 32
candidate actors, verifying the light vtable, backlink, actor/outer slots,
names and catalog map witness. The UObject table slot is checked, not globally
scanned for uniqueness. The catalog map hash is an offline fingerprint, not a
runtime package measurement. The light generation changes each Present
interval, so it does not claim lifetime continuity.
IDs preserve FName base and number separately: `Light:16` denotes the
numbered instance displayed as `Light__15`.

The published light coverage is always `Partial`; only the finite `+0x34`
position is transferred as `Candidate`. Other raw words remain out of SceneIR.
Invalid or unreadable samples are discarded without making light coverage
complete. `--live-lights` in the bedroom runner enables this path; it is
disabled by default.

With `T3RENDER_SCENE_LIVE_DIAGNOSTIC_DIR` and the viewer enabled, the frame
JSONL can also include `light_raw`. It contains actor IDs and the raw integer
bits copied at `+0x24`, `+0x34`, `+0x58..+0x7c`, `+0x88`, `+0x8c` and `+0x90`;
unknown words `+0x80/+0x84` and process addresses are excluded. The words are
diagnostic observations, not verified color, intensity, enabled state, radius
or cone. They never enter SceneIR or activate PBR.

The driver publishes this side channel only after the associated `Present`
succeeds and the selected snapshot is published. Frame, session, epochs,
continuity, camera and candidate values must match. A conflicting repeated
sample, cancellation, Reset, rejected window or loss discards it; the viewer
may also omit it when a newer snapshot supersedes that frame. A false
`light_raw_available` is therefore unknown, not evidence of zero lights.
The sample occurred within the Present interval, not necessarily at its end.
This path performs bounded copying and optional worker-thread file output; its
individual runtime cost has not been isolated.

The opt-in `--candle-link-diagnostics` also follows the current visible
`D_384:6` actor's `+0xb8` link, independently of the renderer's partial light
list. It is restricted to the bedroom catalog fingerprint and reports
`light_raw.candle_link` in the same validated frame record. `candidate` includes
the raw `+0x8c` bits; `no_link` means repeated reads saw a null link;
`unknown` includes absence from the visual list, incomplete reads, changed
identity or ambiguity. Neither `no_link` nor `unknown` establishes the
`bLightOn` property. When the partial light list omits the candidate and the
same-frame actor sample reports a zero factor, the viewer retains the dim
ambient fallback but labels it `preview_candle_zero_candidate`. This is not a
verified disabled state. The new label has only been checked in host tests.
The optional double-read visual-element transform is recorded as raw float
bits. A missing or changed transform suppresses the replacement candlestick
and flame but does not turn a valid factor into a guessed transform. This
transform is also retained for `no_link` when the visual actor remains stable,
so the replacement candlestick can remain visible without asserting that the
light is disabled. The transform and this case are not yet verified in TDS.
No actor address is retained across frames; matching reads cannot exclude ABA.

In a [subsequent TDS run](../../build/t3render-captures/scene-live-m3-candle-link-20261118-mac-v1/M3-link-report.md),
the direct actor sample matched the listed light's `+0x8c` in all 93 frames
where both were observed. After the user extinguished the candle, it read
zero in 153 frames even though the light was no longer in that partial list.
The observed zero is consistent with the interaction, but does not by itself
distinguish disabled from fully damped. The user reported slowdown and noted
that macOS Low Power Mode was active; this run does not isolate the cost of
the sampler or viewer.

After a future capture, `tools/compare_live_light_diagnostics.py` selects
exactly one viewer record by `--session` and `--frame`, verifies the raw bits
against the candidate position, and compares names and positions with an
explicit offline SceneIR fixture. It rejects mismatched map fingerprints,
duplicate frame records, and ambiguous normalized FNames; matching names are
still candidates. The result includes the input hashes and camera position
when the view is invertible. It does not use timestamps as synchronization or
turn the report into a runtime SceneSnapshot. `--synthetic-fixture` is only
for records explicitly marked synthetic; the resulting report preserves that
origin. `--output` creates a new file rather than overwriting one.

From the `t3sdk` root, with values copied from the viewer's frame record:

```sh
python3 -B t3render/tools/compare_live_light_diagnostics.py \
  "$T3_VIEWER_LOG" "$T3_OFFLINE_SCENE" --session "$T3_LIVE_SESSION" --frame "$T3_PRESENT_FRAME"
```

Matching reads cannot exclude concurrent ABA, deallocation/reuse or changes
outside the sampling interval. The selected list is neither a proven complete
inventory nor proof that a light contributes to every draw. The executable
identity is checked by the existing hook before it can call the adapter. The
adapter does not establish physical units, RGB encoding, enabled state or cone.
The names for `+0x24` and `+0x34` are candidates, not a calibrated coordinate
contract. It cannot enable the PBR path, which requires verified fields and
complete coverage. A TDS run has exercised the adapter, but has not verified
its physical interpretation or isolated its cost.

In a [live bedroom capture](../../build/t3render-captures/scene-live-m3-candle-20261117-mac-v1/M3-report.md),
the selected candidate `D_384:6` matched the offline `D_384__5` name and
position within 0.000287 T3 units. The same-frame shader candidate position
matched the raw sample. The `+0x68` floats agree with an HSV-to-RGB conversion
of the offline actor's color bytes, and `+0x78` is 96 versus its offline radius
byte of 12; these associations, color space and radius semantics remain
candidates. `+0x8c` varied from 0.8 to 1.0. Neither the runtime enabled state
nor physical intensity or cone was verified. The user reported no evident lag;
an 18.28-second interval measured 33.64 game Present entries per second with
proxy, viewer and light sampling. This does not isolate sampling overhead.

Across four saved bedroom sessions, 2,059 viewer frames contain exactly one
listed `D_384:6` sample. All report the same `+0x68` RGB values
`(0.4392157, 0.3242598, 0.1240138)`, `+0x78` value `96`, and raw type `3`.
These samples do not cover frames where the light was absent from the partial
list and do not establish color space, light intensity, radius units or a
cutoff function. The preview's 3 m range and 1 cd maximum remain artistic.
Using the project scale, the 96-unit field would be 1.8288 m if it is the
outer radius. For the captured pose, the preview cutoff averaged over the
generated bed's triangle areas is 0.302 at that candidate radius versus 0.792
at 3 m. This is not an image or a measurement of the game's attenuation; the
[radius analysis](../../build/t3render-captures/scene-live-m3-radius-analysis-20261118-v1/M3-radius-report.md)
records the assumptions and remains inconclusive about the correct radius.

In frame 1530, all 12 scoped draws associated with the candle and all 119
selected draws across nine candidate lights have `c21.y/z` bitwise equal to
`float32(1 / +0x78)` and `c20.xyz` equal to the sampled `+0x34` position.
The two application-memory vertex-program candidates associated with the
candle multiply the vertex-to-light displacement by `c21.y` before writing
texture coordinates. The [draw analysis](../../build/t3render-captures/scene-live-m3-radius-draw-analysis-20261118-v1/M3-radius-draw-report.md)
revalidates the source, successful draws and frame budgets, and checks Present
and camera. These samples are not GPU readback, may have been taken at
different times within the Present interval, and do not verify the original
cutoff or outer radius. The preview retains its artistic 3 m range.

Static analysis provides a narrower interpretation of `+0x8c`: in the analyzed
build `0x10979670` responds to the property IDs for `bLightOn` and
`LightDamping`, calls `0x109907e0`, and passes its return to the `0x200`
descriptor setter, which writes `+0x8c`. The getter returns zero when the
`bLightOn` property is found and false, otherwise it uses the damping value
and a clamp before subtraction from one. Relevant evidence is retained in
[rea-modulation-evidence.json](../../build/t3render-captures/scene-live-m3-candle-20261117-mac-v1/rea-modulation-evidence.json)
(`ev_e92b232e284230a6784aa21c9c94f11f95d74721fa71379d78459c4f087d1bdb`,
`ev_d4bebeed154e63e93e029417090de18e93cbd69f4922ff8566e9da95d1dc2111`).
This is static control-flow evidence, not a runtime on/off experiment. The
opt-in preview uses finite `+0x8c` values in `[0,1]` as a relative artistic
brightness factor; zero does not establish whether the actor is disabled or
fully damped. Across 903 preview frames in the later TDS run, the sampled factor
ranged from 0.8 to 1.0 (mean 0.907); that run used a constant preview intensity.

The local script-property table identifies `bLightOn` as inherited boolean ID
1487 (`0x5cf`); `LightDamping` is inherited float ID 904 (`0x388`). The offline
`D_384__5` export has no local `bLightOn` entry, so its initial state can come
from an inherited default rather than a local slot. In the offline archetype
chain, `D_2` supplies `bLightOn=true` and `LightDamping=0`; `D_6` supplies a
slow pulse from 0.8 to 1.0. The saved runtime candidate has the same range
in 903 samples, and nine selected peak intervals have a median of 2.873 s;
the four configured pulse durations sum to 2.79 s. This comparison does not
observe the flag or prove the runtime pulse parameters. The [analysis and
graph](../../build/t3render-captures/scene-live-m3-pulse-analysis-20261118-v1/M3-pulse-report.md)
retain the source hashes and limitations. Static analysis of the
getter (`ev_cf800960c99d852354cf31dab2d8d069a7e10e61db3b4d2c7e817bd32e8c8753`)
and resolver (`ev_2367fc2a425b7375649886e01bf35e75864e7fa87ab6237889ece73ba1e4e129`)
shows lookup through the object's property hash, a possible class/default
chain and a descriptor-specific copy through vtable `+0xb4`. Thus an absent
local slot is not proof of an off value, and blindly treating a raw slot as a
boolean would not reproduce the getter. The focused
[evidence bundle](../../build/t3render-captures/scene-live-m3-candlestick-replay-20261118-v2/rea-enabled-property-evidence.json)
contains the shipped-binary observations; it does not observe the live flag or
prove the runtime descriptor representation.

The opt-in candle-link diagnostic now also attempts a bounded, read-only
`bLightOn` lookup for that actor. It follows the observed local and class-default
property path, checks the candidate boolean descriptor and mask, and compares
two complete traversals. JSON reports `enabled_candidate` and
`enabled_inherited`; either is null when the lookup is unavailable or unsafe.
The renderer does not yet use this value. These fields have passed host fixtures
for local and inherited values, but their descriptor, mask and on/off behavior
have not been observed in TDS. They are candidate evidence, not a verified
enabled state. As with other repeated reads, concurrent ABA remains possible.

The synthetic fixture exercises the bounded list, actor identity and map,
mutations, conversion, and publication through a `Present` window. Separate
tests cover cancel, overlap, Reset and JSON diagnostics. These are host tests,
not TDS observations.
