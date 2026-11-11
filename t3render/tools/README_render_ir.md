# Offline Render IR prototype

`trace_to_ir.py` converts a **complete diagnostic v1 JSONL trace** into a
versioned, API-independent JSON representation for inspection. This is an
offline prototype, not a runtime renderer interface or a replay: geometry,
resource contents, shader programs and draw results are not captured.
Neither visual parity nor gameplay behavior can be established from this IR.

```sh
python3 -B t3render/tools/trace_to_ir.py capture.jsonl -o /tmp/render-ir.json
```

Without `-o` the JSON is written to standard output. Output files must not
already exist. Conversion validates a private snapshot of the exact input bytes
with `trace_summary.py` first; its SHA-256 identifies that source. Temporary
data is removed after validation. No third-party modules are needed.

## Schema v1

- `frames` preserve frame and draw order. Each command refers to a separate
  `camera`, `mesh` and `material` snapshot and its observed `lights`. Snapshot
  IDs are local to this IR; they do not identify persistent game assets.
- Camera view/projection matrices and world/texture matrices are **observed
  source state**, stored as 16 row-major values. Their coordinate convention,
  handedness and actual use by a draw are unknown. For example, pre-transformed
  vertices or shaders may bypass those matrices. A viewport is likewise only
  the observed state.
- Mesh records describe observed buffer bindings, not geometry. Every mesh has
  `geometry_available: false`. `bound`, `unbound`, and `unknown` distinguish
  successful bindings, successful null bindings, and missing/failed getters.
  Getter HRESULTs are interpreted as signed or unsigned 32-bit values; values
  outside that range are unknown. The v1 producer omits `texture_hr` on a
  successful texture getter.
  Resource tokens are opaque process addresses scoped to the source capture;
  they may be reused and must not be treated as stable asset names or IDs.
- Immediate-mode (UP) draws use caller-supplied data that the trace does not
  include. Their mesh has no inferred buffer bindings even if the diagnostic
  snapshot contains previously bound buffers. For buffered indexed draws, the
  index format comes only from the captured index buffer description; base
  vertex, minimum vertex, vertex count and start index remain separate.
- Material values are captured legacy parameters and texture slots, not PBR
  values or texture roles. Lights are reported only when observed: the list is
  explicitly incomplete even when empty, since capture samples a limited range
  and failed getters are omitted. `null` means unavailable or unknown, never a
  generated default.
- Draw calls are attempted operations; their success is not captured. Unknown
  call types and primitive types stay `null` in the portable representation.
  Source-specific raw draw, frame, light and miscellaneous events are preserved
  under `compatibility.d3d8`; `source_line` allows original event order to be
  reconstructed. The raw extension also retains unrecognized values.

The IR can help design and test a future runtime representation, but the
capture lacks the resource bytes, identity, pipeline state and execution data
required for a compatibility renderer. The schema does not imply those missing
pieces exist.
