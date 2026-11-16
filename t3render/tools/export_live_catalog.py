#!/usr/bin/env python3
"""Export an offline SceneIR asset catalog for the in-process scene bridge.

This contains game-derived geometry and remains local. It is not a runtime
frame, visibility observation, or proof of which assets the game loaded.
"""
import argparse
import hashlib
import math
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "ir"))
from scene_ir import Geometry, SceneSnapshot, Section  # noqa: E402
import scene_ir_to_textured_packet as packet  # noqa: E402
import scene_ir_to_d3d12_packet as preview  # noqa: E402
import scene_pbr_bed as generated_bed  # noqa: E402
import scene_material_overrides as material_overrides  # noqa: E402

MAX_BYTES = 64 * 1024 * 1024
MAX_VERTICES = 1_000_000
MAX_INDICES = 3_000_000
MAX_SECTIONS = 1024


def _text(value, *, optional=False):
    if value is None and optional:
        return struct.pack("<I", 0)
    if not isinstance(value, str) or not value or "\0" in value:
        raise ValueError("catalog text must be nonempty and contain no NUL")
    encoded = value.encode("utf-8")
    if len(encoded) > 512:
        raise ValueError("catalog text exceeds 512 bytes")
    return struct.pack("<I", len(encoded)) + encoded


def _u32(value):
    if type(value) is not int or not 0 <= value <= 0xffffffff:
        raise ValueError("catalog integer is outside uint32")
    return struct.pack("<I", value)


def _float(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or abs(value) > 1_000_000:
        raise ValueError("catalog float is outside backend bounds")
    try:
        return struct.pack("<f", value)
    except (OverflowError, struct.error) as exc:
        raise ValueError("catalog float cannot be represented") from exc


def _geometry(geometry):
    positions, indices, sections = geometry.positions, geometry.indices, geometry.sections
    if (not 1 <= len(positions) <= MAX_VERTICES or not 1 <= len(indices) <= MAX_INDICES or
            not 1 <= len(sections) <= MAX_SECTIONS or any(i >= len(positions) for i in indices)):
        raise ValueError("catalog geometry is empty, invalid or too large")
    attributes = (geometry.normals, geometry.uv0, geometry.tangents)
    flags = sum(1 << i for i, values in enumerate(attributes) if values is not None)
    body = bytearray(struct.pack("<4I", len(positions), len(indices), len(sections), flags))
    for rows in (positions, *attributes):
        if rows is not None:
            for row in rows:
                for value in row:
                    body += _float(value)
    for index in indices:
        body += _u32(index)
    for section in sections:
        if not section.index_count or section.index_count % 3 or section.first_index + section.index_count > len(indices):
            raise ValueError("catalog section is not an in-range triangle list")
        body += _text(section.name) + _u32(section.first_index) + _u32(section.index_count)
        body += _text(section.material_id, optional=True)
    return body


def _resource(identifier, name, source_sha, geometry):
    return _text(identifier) + _text(name) + _text(source_sha) + _geometry(geometry)


def _bed_override(scene, bed, raw):
    scale = scene.conventions.meters_per_unit
    if scale is None or not math.isclose(scale, .01905, rel_tol=0, abs_tol=1e-9):
        raise ValueError("generated bed requires the SDK unit scale")
    primitives = generated_bed._primitives(raw)
    source = [(x*scale, z*scale, y*scale) for x, y, z in bed.geometry.positions]
    generated = [point for positions, _, _, _ in primitives.values() for point in positions]
    if not source or not generated or any(abs(f(source, key=lambda p:p[axis])[axis]-
                                             f(generated, key=lambda p:p[axis])[axis]) > .3
                                         for axis in range(3) for f in (min, max)):
        raise ValueError("generated bed bounds do not match the source asset")
    positions, normals, uv, indices, sections = [], [], [], [], []
    for name in generated_bed.MATERIALS:
        points, directions, texcoords, part_indices = primitives[name]
        base, first = len(positions), len(indices)
        positions.extend((x/scale, z/scale, y/scale) for x, y, z in points)
        normals.extend((x, z, y) for x, y, z in directions)
        uv.extend(texcoords)
        # The glTF-to-T3 Y/Z reflection reverses winding.
        for at in range(0, len(part_indices), 3):
            a, b, c = part_indices[at:at+3]
            indices.extend((base+a, base+c, base+b))
        sections.append(Section("generated:"+name, first, len(part_indices), "remaster:material:bed_high:"+name))
    digest = hashlib.sha256(raw).hexdigest()
    geometry = Geometry(tuple(positions), tuple(normals), tuple(uv), None, tuple(indices), tuple(sections))
    return "remaster:mesh:bed_high:"+digest, digest, geometry


def _material_library(textures):
    body = bytearray(_u32(2))
    manifest = {}
    expected_size = None
    for name in ("stone_wall_v2", "wood_floor_v2"):
        body += _text("remaster:material:" + name)
        sources = {}
        for kind in ("albedo", "normal", "roughness", "ao"):
            value = textures.resolve(name, kind)
            width, height, mips = value["width"], value["height"], value["mips"]
            digest = value["source_sha256"]
            if (type(width) is not int or width != height or not 1 <= width <= 1024 or width & (width-1) or
                    not isinstance(digest, str) or len(digest) != 64 or
                    any(c not in "0123456789abcdef" for c in digest) or len(mips) != width.bit_length()):
                raise ValueError("invalid bedroom material texture metadata")
            if expected_size is None:
                expected_size = width
            if width != expected_size:
                raise ValueError("bedroom material textures must have matching sizes")
            body += _text(digest) + struct.pack("<3I", width, height, len(mips))
            for level, mip in enumerate(mips):
                size = max(1, width >> level)
                if (not isinstance(mip, bytes) or len(mip) != size*size*4 or
                        (kind == "albedo" and any(alpha != 255 for alpha in mip[3::4]))):
                    raise ValueError("invalid or nonopaque bedroom material mip")
                body += mip
            sources[kind] = digest
        manifest[name] = sources
    return body, manifest


def export(raw, bed_high=None, bedroom_materials=None):
    if not isinstance(raw, bytes) or not raw or len(raw) > preview.MAX_INPUT:
        raise ValueError("offline SceneIR input is empty or too large")
    scene = SceneSnapshot.from_json(raw)
    source, conventions = scene.source, scene.conventions
    if source.kind != "offline" or source.frame_id is not None or source.camera_synchronized is True:
        raise ValueError("catalog requires an offline SceneIR fixture")
    if (conventions.length_unit != "t3_unit" or conventions.axes != "t3_xyz" or conventions.up_axis != "z" or
            conventions.matrix_layout != "row_major" or conventions.matrix_vectors != "row" or
            conventions.tangent_space != "unknown" or conventions.clip_depth == "minus_one_to_one"):
        raise ValueError("catalog requires supported T3 coordinates")
    map_name, map_sha = source.map, source.fingerprints.get("map")
    if ":" in map_name or not map_sha:
        raise ValueError("catalog requires a qualified map fingerprint")
    if bedroom_materials is not None and (map_name != material_overrides.MAP or
            conventions.meters_per_unit is None or
            not math.isclose(conventions.meters_per_unit, material_overrides.SCALE, rel_tol=0, abs_tol=1e-9)):
        raise ValueError("bedroom materials require SouthQuarter1_int1 and the SDK unit scale")
    actor_id = f"actor:{map_name}:{map_sha}:StaticMeshActor__1520"
    instances = [item for item in scene.mesh_instances if item.id == actor_id]
    if len(instances) != 1:
        raise ValueError("offline fixture must contain one target bed actor")
    instance = instances[0]
    beds = [item for item in scene.static_mesh_resources if item.id == instance.resource_id and item.name.lower() == "innbed1a1"]
    if len(beds) != 1:
        raise ValueError("offline fixture must contain the target bed mesh")
    bed = beds[0]
    prefix, suffix = f"mesh:{map_name}:{map_sha}:", ":innbed1a1"
    if not bed.id.startswith(prefix) or not bed.id.endswith(suffix):
        raise ValueError("bed resource ID is not qualified by map and bundle")
    bed_sha = bed.id[len(prefix):-len(suffix)]
    if bed_sha not in (source.fingerprints.get("bundle"), source.fingerprints.get("kernel")) or not bed_sha:
        raise ValueError("bed resource fingerprint is not an offline source fingerprint")
    if not 1 <= len(scene.bsp_blocks) <= 4096:
        raise ValueError("catalog requires a bounded offline BSP collection")
    for block in scene.bsp_blocks:
        if block.source_offset is None or block.id != f"bsp:{map_name}:{map_sha}:{block.source_index}:{block.source_offset}":
            raise ValueError("BSP ID must retain offline index and offset")
    material_by_id = {m.id: m for m in scene.materials}
    required = {s.material_id for r in [bed, *scene.bsp_blocks] for s in r.geometry.sections if s.material_id is not None}
    required.update(x for x in instance.material_ids if x is not None)
    bed_materials = instance.material_ids or tuple(s.material_id for s in bed.geometry.sections)
    if (not bed_materials or any(x is None or x not in material_by_id for x in bed_materials) or
            any(s.material_id is None for b in scene.bsp_blocks for s in b.geometry.sections) or
            not required or any(x not in material_by_id for x in required)):
        raise ValueError("catalog requires known bed skin and BSP materials")
    materials = [m for m in scene.materials if m.id in required]
    replacement = _bed_override(scene, bed, bed_high) if bed_high is not None else None
    extra_materials = [("remaster:material:bed_high:"+name, name) for name in generated_bed.MATERIALS] if replacement else []
    if any(identifier in material_by_id for identifier, _ in extra_materials):
        raise ValueError("generated bed material IDs collide with the offline scene")
    if bedroom_materials is not None and any("remaster:material:"+name in material_by_id
                                             for name in ("stone_wall_v2", "wood_floor_v2")):
        raise ValueError("bedroom material IDs collide with the offline scene")
    if len(materials) + len(extra_materials) > 8192:
        raise ValueError("catalog material count exceeds bounds")
    enum = lambda value, choices: choices.index(value)
    c = conventions
    values = (enum(c.handedness, ("unknown", "left", "right")),
              enum(c.winding, ("unknown", "normal_aligned", "normal_opposed")),
              enum(c.uv_origin, ("unknown", "top_left", "bottom_left")),
              enum(c.clip_depth, ("unknown", "zero_to_one")))
    body = bytearray(b"T3LCAT03" if bedroom_materials is not None else b"T3LCAT02" if replacement else b"T3LCAT01")
    for value in (map_name, map_sha, hashlib.sha256(raw).hexdigest(), actor_id):
        body += _text(value)
    body += struct.pack("<4I", *values)
    body += _u32(int(c.meters_per_unit is not None))
    if c.meters_per_unit is not None:
        body += _float(c.meters_per_unit)
    body += _u32(len(materials) + len(extra_materials)) + _u32(len(scene.bsp_blocks))
    for material in materials:
        body += _text(material.id) + _text(material.name)
    for identifier, name in extra_materials:
        body += _text(identifier) + _text(name)
    body += _resource(bed.id, bed.name, bed_sha, bed.geometry)
    body += _u32(len(instance.material_ids))
    for item in instance.material_ids:
        body += _text(item, optional=True)
    for block in scene.bsp_blocks:
        body += _resource(block.id, f"BSP {block.source_index}", map_sha, block.geometry)
    if bedroom_materials is not None:
        body += _u32(int(replacement is not None))
    if replacement:
        identifier, digest, geometry = replacement
        body += _resource(identifier, "bed_high", digest, geometry)
    material_sources = None
    if bedroom_materials is not None:
        packed, material_sources = _material_library(bedroom_materials)
        body += packed
    if len(body) + 32 > MAX_BYTES:
        raise ValueError("catalog exceeds 64 MiB")
    data = bytes(body) + hashlib.sha256(body).digest()
    manifest = {"schema": "t3.scene.live-catalog", "version": 3 if bedroom_materials is not None else 2 if replacement else 1, "source": "offline",
                "offline_scene_sha256": hashlib.sha256(raw).hexdigest(), "catalog_sha256": hashlib.sha256(data).hexdigest(),
                "map": map_name, "map_sha256": map_sha, "bed_resource_id": bed.id, "bed_actor_id": actor_id,
                "bsp_blocks": len(scene.bsp_blocks), "materials": len(materials) + len(extra_materials),
                "limitations": ["Offline assets only; no frame, visibility, transform or live binding is claimed.",
                                "BSP catalog completeness is inherited from the heuristic offline importer."]}
    if replacement:
        manifest["limitations"].append("Any selected source bed section causes the whole replacement to be drawn; partial visibility is not established.")
        manifest["bed_override"] = {"resource_id": replacement[0], "asset_sha256": replacement[1],
                                    "vertices": len(replacement[2].positions), "indices": len(replacement[2].indices),
                                    "sections": len(replacement[2].sections),
                                    "scope": "viewer_only_when_semantic_bed_is_selected"}
    if material_sources is not None:
        manifest["bedroom_materials"] = {"sources_sha256": material_sources,
                                         "scope": "viewer_only_when_same_frame_bed_anchor_is_selected"}
        manifest["limitations"].append("Materials are offline visual assets; the flat viewer does not infer runtime lighting.")
    return data, manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("offline_scene", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--bed-high", type=Path, help="optional generated Godot bed GLB for the live viewer")
    parser.add_argument("--bedroom-materials", type=Path, help="optional generated Godot material directory")
    parser.add_argument("--bedroom-texture-size", type=int, default=512, help="power-of-two material texture size, up to 1024")
    args = parser.parse_args(argv)
    try:
        raw = preview.read_limited(args.offline_scene, preview.MAX_INPUT)
        high = preview.read_limited(args.bed_high, generated_bed.generated.MAX_GLB_BYTES) if args.bed_high else None
        textures = (material_overrides.GodotTextures(args.bedroom_materials, args.bedroom_texture_size, True)
                    if args.bedroom_materials else None)
        body, manifest = export(raw, high, textures)
        packet.write_pair(args.output, body, manifest)
    except (OSError, ValueError, UnicodeError) as exc:
        parser.exit(2, f"{exc}\n")
    print(f"offline_catalog=ok bytes={len(body)} bsp_blocks={manifest['bsp_blocks']}")


if __name__ == "__main__":
    main()
