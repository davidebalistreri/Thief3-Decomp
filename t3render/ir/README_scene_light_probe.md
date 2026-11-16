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

`scene_bridge_light_candidate.hpp` implements a fault-safe-reader adapter for
one actor already identified and known live by its caller. It samples the
vtable, backlink and raw words twice, checking the actor link three times.
It returns raw words only after all reads match. `NoLight` means that one read
found a null pointer, not that a complete light collection is empty. The
result contains no addresses and has no conversion to `VisualLight`.

The caller must validate the executable identity and actor lifetime, and must
establish a same-thread `Present` window separately. Matching reads cannot
exclude concurrent ABA, deallocation/reuse, unobserved changes, or prove that
the actor is visible. The reader is not hooked into TDS, does not enumerate
lights, and does not establish physical units, RGB encoding, enabled state,
cone, or coverage. The names for `+0x24` and `+0x34` are candidates, not a
calibrated coordinate contract. It cannot enable the PBR path, which requires
verified fields and complete coverage.

The synthetic fixture tests successful copying, null, unreadable and
overflowing addresses, wrong vtable/backlink, and changes to each sampled
group or owner link. These are host tests, not TDS observations.
