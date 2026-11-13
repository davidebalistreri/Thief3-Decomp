# D3D8 texture-type inventory

`trace_texture_inventory.py` extracts per-frame observed texture types from a
validated diagnostic trace. It counts only draws with a paired successful
result and distinguishes unbound, failed and unknown stage observations. It
does not inspect pixel data or resource COM paths. Addresses are counted only
within each frame: they can be reused or refer to different COM aliases and are
not stable T3 asset identities. Failed draws, missing stage snapshots, resets,
overlap and failed presentation must not be mistaken for complete GPU evidence.

```sh
python3 -B t3render/tools/trace_texture_inventory.py /absolute/path/to/trace.jsonl
python3 -B t3render/tools/trace_texture_inventory.py /absolute/path/to/trace.jsonl -o /absolute/path/to/new-inventory.json
python3 -B -m unittest t3render.tests.test_trace_texture_inventory -v
```

The JSON output records the source SHA-256 and frame outcome; `-o` refuses to
overwrite a file. The tool reads and validates an immutable copy through the
existing offline Render IR converter. It does not make the trace replayable.

For the earlier TDS bedroom program-history frame, the
[inventory](../benchmarks/d3d8/2026-11-13-bedroom-texture-types/inventory.json)
reports 518 successful draw attempts, with 2D texture bindings in 344 draws and
cube texture bindings in 156 draws. The original trace is local to
`build/t3render-captures/bedroom-program-history-20261113-1650/bedroom.jsonl`.
This identifies cube textures as a real input class for the benchmark; it does
not establish which resource methods the game calls or whether an alias escapes.
The facade boundary must cover cube textures and their surfaces alongside 2D
textures, surfaces and remaining D3D8 resource types before enabling VB/IB
wrappers in TDS.
