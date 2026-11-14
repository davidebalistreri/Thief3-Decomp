# Partial DX12 diagnostic quad

This experiment displays **one observed indexed quad**, not a replay of a TDS
frame. The 2026-11-14 bedroom capture contains a color-writing draw that can be
interpreted as `XYZ|DIFFUSE|TEX2` FVF; the numeric value might instead be a
shader handle. Observed buffer bytes, FVF, transforms and viewport are not proof
of the inputs consumed by the GPU. The quad has not been identified as a room
object, and the capture does not establish its render target or provide a
synchronized image of the original. See the [result](../benchmarks/d3d12/2026-11-14-tds-quad-diagnostic/result.json).

The converter requires an explicit frame and global offline-IR draw number. It
checks the geometry candidate, draw/Present outcomes, stable buffered data,
two triangles sharing an edge, FVF shape, flat opaque vertex colors, observed
RGB writes, matrices and a bounded viewport at origin zero. It expands the
observed indices, and preserves WORLD0, VIEW, PROJECTION and viewport size and
depth range. An unsupported or unavailable observation is rejected. The
camera shader assumes D3D8 row-vector matrix order and the existing half-pixel
adjustment; that adjustment is not validated for this game draw.

The output uses a black background and the observed flat vertex color. Texture
contents and stages, depth testing/writing, blending, alpha testing, lighting,
stencil, other draw calls and postprocessing are omitted. Thus color, visibility,
occlusion and final position in the actual TDS frame are not established. The
original shader/program and any effects of render-target changes are not
replayed. This feature does not enable the experimental facade or modify the
game or stealth logic.

## Reproduce on the same local capture

From `t3sdk`, build the x86 replay target as described in
[the synthetic replay instructions](README_synthetic_replay.md), then:

```sh
python3 -B t3render/tools/trace_to_diagnostic_quad.py \
  build/t3render-captures/bedroom-combined-20261114/bedroom-policy.jsonl \
  --frame 1392 --draw 668 -o /absolute/new/path/quad.txt
```

Run `t3render_d3d12_replay_up.exe` under an existing compatible D3D12 runtime,
passing that packet and `--output` with a **new** PPM path. In Highball use the
same environment and Windows `Z:\\...` paths as the synthetic replay runner.
The executable will report `result=DIAGNOSTIC`, not a parity PASS. The packet
contains game-derived data; the source trace, generated packet and images are
kept locally rather than committed. Keep the original output at 1024×768 when
inspecting proportions; any enlarged detail is only a magnification.

The tested result is a small solid rectangle near the right side of a black
1024×768 image. Its pixel bounds agree with a CPU calculation using the same
assumed matrices; shifting the observed VIEW translation moves the output.
Neither observation establishes equivalence with TDS. The next substantive
blockers remain the unavailable large room vertex buffer, visible shaders,
texture data and the experimental facade's truncated menu text.
