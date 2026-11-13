# Offline Render IR prototype

`trace_to_ir.py` converts a **complete diagnostic v1 JSONL trace** into a
versioned, API-independent JSON representation for inspection. This is an
offline prototype, not a runtime renderer interface or a replay: decoded
geometry, complete resource contents and complete shader state are not captured.
Newer diagnostic traces can contain opaque UP or bounded buffer-shadow bytes
and paired operation results; older traces may lack them.
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
- Immediate-mode (UP) draws use caller-supplied data. When the optional
  `up_data` record is present, `immediate_data_observed` exposes each validated
  vertex/index byte snapshot as opaque hex with its recorded offset, stride,
  counts and format. `captured` and `empty` require coherent hexadecimal data,
  byte lengths and counts; otherwise the component is `unknown` and its raw
  source remains in the draw's compatibility extension. Old traces with no UP
  data remain valid and produce unknown components. An index value of `null`
  is used only for non-indexed draws. These are snapshots before the call;
  concurrent writes could change the bytes seen by the driver. They do not
  make the capture replayable or assert a decoded mesh. The mesh has no
  inferred buffer bindings even if the diagnostic snapshot contains previously
  bound buffers. For buffered indexed draws, the
  index format comes only from the captured index buffer description; base
  vertex, minimum vertex, vertex count and start index remain separate.
- When a buffered draw includes `buffer_data`, the mesh gains
  `buffered_data_observed` only after its corresponding `draw_result` has been
  matched by operation ID within the same frame. It records the source,
  `buffer_stable`, and exact `draw_hresult`, plus independent vertex and index
  snapshots. The IR exposes bytes only when the header advertises the expected
  bounded shadow capture, the draw succeeded and the intercepted draw window
  was stable. It validates binding identity, getter success, stream, descriptor
  size, offsets, stride, topology-derived counts, index format and index values
  against the draw's declared vertex range. Inconsistent or unknown components
  stay `unknown` with null data; failed or unstable draws make both components
  unknown. It also checks the frame-wide budget against all emitted vertex and
  index snapshots, including later failed or unstable draws. If the frame
  exceeds that budget or a captured payload's size cannot be determined, no
  buffered bytes are exposed for that frame. A non-indexed draw has a null index
  component. Legacy draws and UP
  draws do not gain this field, and raw source data remains in the compatibility
  extension. These are pre-draw observations from application write locks:
  concurrent CPU writes can still differ from what the GPU consumes.
- An observed declaration is attached to its draw command as
  `vertex_declaration_observed` only after matching the `draw_result` in the
  same frame by operation ID. It retains the draw HRESULT and the independently
  observed `shader_stable` flag. A `candidate` exposes its numeric shader value,
  generation and DWORDs only if the header requests declaration capture, the
  draw succeeds, `shader_stable` is true, and the value and generation are
  valid unsigned integers. The words are bounded to 1024 and must form a
  complete declaration: CONST/EXT payloads are skipped before looking for the
  final END token. This checks framing, not declaration or shader semantics.
  Missing, unstable, failed or malformed observations produce `unknown` and
  no words; a stable observation may also report `unknown`, `unavailable` or
  `unsafe`. A stable unavailable generation may also carry an allowlisted
  `reason` and `scan_status`, describing the current observation's
  unavailability and pre-create scan rather than driver behavior. A copy
  error affects one observation and does not invalidate its generation. Missing, inconsistent,
  unstable or malformed metadata is omitted; failed scans expose no partial
  words. A result can contain `shader_stable` without a declaration, and old
  traces retain their previous shape. Raw source fields remain in the
  compatibility extension and are not validated observations.

  Even a stable declaration is a candidate: a shader handle can resemble an
  FVF, native aliases may bypass interception and concurrent writes can affect
  what the driver consumes. `shader_stable` does not establish alias coverage,
  shader identity, GPU input or replayability. Generations are scoped to the
  observed device lifetime and are not persistent asset identifiers.
- A separate optional `vertex_declaration_pre_reset` is an **unverified
  hypothesis**, never a candidate for replay. It reports DWORDs sampled from
  application memory before a Reset, with the recorded generation, numeric
  shader value, reset/rebind epochs and Reset/draw HRESULTs. It does not
  establish shader survival, identity, the driver's declaration or the data
  used by the draw. A successful Reset and a later observed rebind are
  necessary, not sufficient, evidence.

  The header must request `declaration_history` and advertise positive limits
  of at most 1024 words per observation and 262144 words per frame, with the
  latter at least the former. The normal declaration observer must also be
  requested. The source record must identify itself as `hypothesis` from
  `application_memory_before_reset`, have a nonzero uint64 generation and
  reset epoch, a later uint64 rebind epoch, a successful Reset HRESULT and a
  framed declaration within the advertised observation limit. The same
  draw's separately validated declaration must be `unavailable` with
  `reason=reset_attempt`, `scan_status=complete`, and the same generation.
  The corresponding draw result must match by operation ID in the same frame,
  succeed and have `shader_stable=true`; the shader value must be uint32.
  Failure of any condition omits the hypothesis from the command.

  The frame budget counts every raw history word in all draw attempts, even
  failed, unstable or otherwise ineligible draws. If an emitted payload's
  words are missing, malformed, exceed the advertised per-observation limit or their
  size cannot be determined, or the total exceeds the frame limit, the IR
  omits **all** such hypotheses for that frame. Budgets are independent
  between frames. Raw fields are preserved without validation in the D3D8
  compatibility extension. Old traces retain their previous shape; no history
  makes the frame replayable.
- Optional `vertex_program_observed` describes a bounded `vs_1_1` program
  sampled from application memory at shader creation. It appears only when the
  program and declaration share a generation, the draw result is successful
  and stable, the opt-in header and frame budget are valid, and the complete
  program is framed. COMMENT and DEF payloads are skipped when framing. This
  does not validate instruction semantics, shader identity or GPU input, and
  is never recovered from pre-reset history. Pixel programs and constants are
  not captured.
- Material values are captured legacy parameters and texture slots, not PBR
  values or texture roles. Lights are reported only when observed: the list is
  explicitly incomplete even when empty, since capture samples a limited range
  and failed getters are omitted. `null` means unavailable or unknown, never a
  generated default.
- Draw calls are attempted operations; newer traces retain their outcomes in
  `draw_result` events, while older traces do not. Unknown
  call types and primitive types stay `null` in the portable representation.
  Source-specific raw draw, frame, light and miscellaneous events are preserved
  under `compatibility.d3d8`; `source_line` allows original event order to be
  reconstructed. The raw extension also retains unrecognized values.

The IR can help design and test a future runtime representation, but the
capture still lacks complete resource contents, persistent asset identity,
pipeline state and execution data required for a compatibility renderer. The
schema does not imply those missing pieces exist.

## Observed geometry candidates

`python3 -B t3render/tools/trace_geometry_candidates.py capture.jsonl -o /tmp/geometry-ir.json`
enriches the validated IR with a `geometry_candidate` for each draw. It accepts
stable buffered or immediate observations with a successful draw and Present,
no recorded reset or concurrent operations, and a bounded fixed-function FVF
layout. It describes XYZ or XYZRHW positions, normals, point size, colors and
up to eight texture coordinate sets. Blended vertices and unsupported FVF bits
remain unavailable. Indexed draws are rebased by the declared minimum vertex
onto the captured vertex window, including nonzero base vertices. It validates
the index range, stride and finite float attributes; missing or inconsistent
observations remain unavailable with a reason.

The numeric vertex-shader value may also be a programmable shader handle, so
the layout is explicitly a candidate. The output retains the original state,
draw order, source hash and `replayable: false`; UP data can change after the
snapshot, unobserved device operations and GPU writes remain possible, and no
texture contents or shader code are inferred. It is not a frame replay.
