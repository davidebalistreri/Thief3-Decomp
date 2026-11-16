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
complete. Optional viewer diagnostics record bounded IDs, position and field
evidence per published frame. `--live-lights` in the bedroom runner enables
this path; it is disabled by default.

Matching reads cannot exclude concurrent ABA, deallocation/reuse or changes
outside the sampling interval. The selected list is neither a proven complete
inventory nor proof that a light contributes to every draw. The executable
identity is checked by the existing hook before it can call the adapter. The
adapter does not establish physical units, RGB encoding, enabled state or cone.
The names for `+0x24` and `+0x34` are candidates, not a calibrated coordinate
contract. It cannot enable the PBR path, which requires verified fields and
complete coverage. No TDS run has verified this adapter or its cost.

The synthetic fixture exercises the bounded list, actor identity and map,
mutations, conversion, and publication through a `Present` window. Separate
tests cover cancel, overlap, Reset and JSON diagnostics. These are host tests,
not TDS observations.
