# TDS bedroom observer capture

This is a real TDS frame observed through the diagnostic D3D8 proxy, not a
DX12 replay. The sanitized [result manifest](result.json) records the runtime,
draw counts and hashes. The game-derived trace, partial IR, Wine log and an
earlier user-provided framing reference remain local in
`build/t3render-captures/bedroom-observer-20261112-031847/` and are not tracked.
The reference image is not an image of the captured frame.

To repeat the diagnostic, follow the proxy build and capture instructions in
the [T3Render README](../../../README.md), with buffer observation disabled.
Start TDS with a fresh output path and a fresh external marker path, load the
`Garrett’s Building` save, orient the camera toward the bed in
`SouthQuarter1_int1`, and create the marker while the game remains foreground.
The game can pause presentation in the background. Confirm the proxy's native
load in the Wine log, validate the JSONL with `trace_summary.py`, then use
`trace_to_ir.py` to produce a partial IR. Remove the temporary proxy after the
game closes; do not replace an existing wrapper. Record the actual camera and
a screenshot from the same frame before treating a later run as a pixel
benchmark.

This capture contains draw order and observed state, but no VB/IB bytes,
texture content, shader declarations or shader code. It does not reconstruct
the room or establish visual, gameplay or stealth parity. A missing fixed-
function light record does not rule out lighting implemented by shaders.
