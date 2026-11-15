# Scene IR v0 and v1

`scene_ir.py` describes a semantic scene as plain data. It is distinct from the
diagnostic draw trace and its Render IR. It has no graphics API types or process
pointers, and does not infer visibility, resource lifetime, GPU input, or
physically calibrated lights.

`SceneSnapshot.from_dict` validates a document; `from_json` also rejects
duplicate JSON keys and non-finite numbers. `to_dict` and `to_json` revalidate
the complete object, including references. Unknown fields are rejected, so a
consumer cannot silently carry a device, COM or UObject field through the
schema. Identifiers are semantic strings, not process addresses. A producer is
responsible for constructing stable identifiers rather than encoding pointers
inside them. `source.fingerprints` maps source roles to lowercase SHA-256.

`source.kind` is `offline` or `runtime`. Offline data has no frame ID and may
not claim camera synchronization; runtime data requires a frame ID. A true
`camera_synchronized` must be established by the producer and requires a camera
marked runtime from the same frame. Each camera separately records its own
source kind, optional trace fingerprint and runtime frame ID: an offline scene
can include a runtime camera while remaining an unsynchronized offline fixture.
For `camera_synchronized=true`, `source.fingerprints.trace` and the camera's
fingerprint must both be present and equal; equal frame numbers alone do not
prove synchronization. These checks enforce necessary conditions; the producer
still has to establish actual synchronization.
`source.limitations` records known omissions or assumptions. Offline imports are
fixtures and are not a game frame, even if they contain a map and geometry.

The snapshot contains an optional `camera`, `static_mesh_resources`, separate
`mesh_instances`, `bsp_blocks`, `materials` and visual `lights`. Geometry owns
positions, optional normals/UV0/tangents, triangle indices and sections.
Each section's index count is a multiple of three and its indices must reference
existing vertices; its start may be any index-buffer offset. With no sections,
the entire index array is treated as triangles.
Section materials may be unknown, or an instance can select a material per
section for its skin. BSP entries include source index and optional source
offset because original block IDs may repeat. Null light fields, including
`enabled`, mean unknown; radius/intensity do not imply a physical calibration.

## Scoped submissions in v1

Version 1 adds a `submissions` array to the v0 fields. Each entry refers to a
mesh instance or BSP block and one of its sections, with `render_pass`, `stage`,
the trace's `draw_id`, and `lock_shadow` (`matched` or `unverified`). The source
frame and trace fingerprint identify the capture. A submission is a producer's
claim of direct scoped ownership, not an API command or a persistent resource
ID. Target and section references must resolve, draw IDs must be unique, and a
nonempty array requires a synchronized runtime camera and trace fingerprint.
Offline v1 scenes may have an empty array. Existing v0 documents continue to
round-trip unchanged. No process addresses, COM or UObject pointers belong in
either format.

The scoped snapshot producer described in
[the runtime checkpoint](../tools/README_bridge_runtime_checkpoint.md) emits
only verified submissions for one pass and stage with the anchor's camera,
draw viewport and thread. Geometry remains asset-backed and an unlisted section
is not claimed visible. Lock shadows apply only to the same draw and frame;
separate reference captures cannot upgrade their status. The v1 preview
requires one unambiguous pass/stage and renders only its submitted sections.
The format supports lights, but an empty runtime list is not evidence that no
lights exist: the producer documents missing light coverage in limitations.

## Conventions

- Matrices contain 16 values in row-major order and multiply row vectors on
  the left. Translation is elements 12–14. `transform_point` returns homogeneous
  coordinates and does not divide by W or perform an API projection conversion.
- `length_unit`, `meters_per_unit`, axes, up axis, handedness, winding,
  UV origin, tangent space and clip depth are explicit fields. Unknown values
  remain unknown. `distance_in_meters` requires an explicit scale. The existing
  T3SDK asset export estimates 0.01905 metres per T3 unit from content; this is
  not a runtime measurement and is not silently applied here.
- Existing T3SDK documentation describes the native Unreal basis as X forward,
  Y right, Z up, left-handed. Its Y/Z swap for a Y-up export flips orientation;
  reversing each triangle preserves the alignment of `(b-a) × (c-a)` with its
  correspondingly transformed normal. The source's absolute front-face winding
  and UV origin are not inferred from that conversion. Tangents are preserved
  as three components; tangent handedness and bitangent reconstruction remain
  unknown. No implicit UV flip, tangent sign or unit conversion is performed.
- Winding is `unknown`, `normal_aligned`, or `normal_opposed`: for a
  non-degenerate triangle, `normal_aligned` means the dot product of
  `(b-a) × (c-a)` and the sum of its three vertex normals is positive;
  `normal_opposed` means it is negative. Zero is indeterminate. This definition
  does not depend on a viewport or a clockwise screen convention. Producers
  may declare either orientation only when supported by their data.
- Clip-space depth is independently declared (`unknown`, `zero_to_one`, or
  `minus_one_to_one`). It is not the viewport depth range: the latter belongs to
  the camera viewport, can be missing, and is never inferred from projection.

Run the synthetic contract tests from the SDK root:

```sh
python3 -B -m unittest t3render.tests.test_scene_ir
```
