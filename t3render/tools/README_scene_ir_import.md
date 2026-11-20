# Import a T3 map into SceneIR v0

`import_t3_scene.py` reads the SDK's offline `t3map.py` actor export and the
matching GMP/IBT game assets, then emits an API-neutral semantic scene. It does
not run the game. The fixture covers the **entire map**, not the bedroom's
runtime visibility or a captured frame. `source.kind` is `offline`, frame and
camera are null, and no runtime camera synchronization is asserted.

From the `t3sdk` root, after generating the actor export with `t3map.py`:

```sh
python3 -B t3render/tools/import_t3_scene.py SouthQuarter1_int1 \
  --game-dir "$T3_GAME_DIR" \
  --actors build/assets/godot/SouthQuarter1_int1/SouthQuarter1_int1.actors.json \
  --output build/t3render-offline/SouthQuarter1_int1.scene.json
```

The output must not exist; game-derived files stay local under `build/`. The
importer checks the actor export's GMP size and SHA-1, and records SHA-256
fingerprints for the source files. It reads mesh geometry, section and skin
materials from `t3mesh.py`; map actor transforms and visual light properties
from `t3map.py`; and world-space BSP blocks from `t3bsp.py`. BSP discovery is
heuristic and block-to-zone membership is unknown. Block IDs can repeat, so
SceneIR IDs also include the source index, offset, and map fingerprint.

Use `--bundle /path/to/SouthQuarter1_int1.ibt` to read a verified map-bundle
backup without replacing the installed bundle. The kernel bundle still comes
from the game directory and both fingerprints are recorded; this produces an
offline scene, not evidence of which bundle a live game loaded.

Geometry remains in native T3 coordinates and units. Left-handed Z-up and the
row-vector rotation convention follow the SDK. Its scale of 0.01905 meters per
unit is inferred from a 4x8 asset and is not calibrated at runtime. Winding,
UV origin, tangent space and clip depth remain unknown. Mesh resources are shared;
per-instance material references preserve alternate skins. Actors with
unresolved attachments are excluded, and a missing skin keeps null material
references. Each exclusion is listed in `source.limitations`. Visual light
colors use `t3map.py`'s approximate conversion; uncalibrated radius and
intensity are null. BF marks legacy hidden geometry; v0 does not encode its
visibility, so consumers must handle it explicitly. No remaster materials or
runtime state are inferred.

For an actor with a static mesh, the light position is the unique
`hp_lighttarget` plus `LightHardpointOffset`, rotated and translated without
applying `DrawScale`, as documented in `docs/assets.md`. A missing, ambiguous or
invalid hardpoint/rotation leaves the light's position null and is recorded in
limitations. An unattached light without a mesh uses its actor position only
when no nonzero hardpoint offset is present. Older fixtures used the mesh actor
origin instead; regenerate them to obtain corrected light positions. This is an
offline asset calculation, not observation of a saved runtime state.
