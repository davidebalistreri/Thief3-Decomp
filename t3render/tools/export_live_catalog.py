#!/usr/bin/env python3
"""Export an offline SceneIR asset catalog for the in-process scene bridge.

This contains game-derived geometry and remains local. It is not a runtime
frame, visibility observation, or proof of which assets the game loaded.
"""
import argparse
import hashlib
import math
import re
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
BEDROOM_ORIGINAL_NAMES = ("BF", "CIVGoodness_D", "GENbeamA_D", "GENbreadcrust_D", "GENgoodnessA_D", "GENgoodnessBOOK1_D",
                          "GENtarget_D", "GENwindowint_D", "HSEfloorB_D", "HSEGoodnessA_D", "STMtrimA_D", "STMwallB_D")


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


def _readable_wool_mips(mips):
    # The existing wool is nearly black under the bounded candle preview.
    # Lift only this albedo in linear light; never modify the source PNG.
    table = []
    for value in range(256):
        x = value / 255
        linear = x / 12.92 if x <= .04045 else ((x + .055) / 1.055) ** 2.4
        linear = min(1, linear * 5)
        table.append(round(255 * (12.92*linear if linear <= .0031308 else 1.055*linear**(1/2.4)-.055)))
    result = []
    for mip in mips:
        if not isinstance(mip, bytes) or len(mip) % 4:
            raise ValueError("invalid wool albedo mip")
        pixels = bytearray(mip)
        for i in range(0, len(pixels), 4):
            pixels[i], pixels[i+1], pixels[i+2] = table[pixels[i]], table[pixels[i+1]], table[pixels[i+2]]
        result.append(bytes(pixels))
    return result


def _material_library(textures, bed_textiles=False, readable_bed_wool=False):
    names = ("stone_wall_v2", "wood_floor_v2") + (("bed_wool_v2", "pillow_linen") if bed_textiles else ())
    body = bytearray(_u32(len(names)))
    manifest = {}
    expected_size = None
    for name in names:
        identifier = {"bed_wool_v2": "bed_high:textured:GENiFABRICwoolA2_D", "pillow_linen": "bed_high:textured:PillowLinen"}.get(name, name)
        body += _text("remaster:material:" + identifier)
        sources = {}
        for kind in ("albedo", "normal", "roughness", "ao"):
            constant = (name in ("bed_wool_v2", "pillow_linen") and kind == "ao") or (name == "pillow_linen" and kind == "roughness")
            if constant:
                if expected_size is None:
                    raise ValueError("bed textile constants require the base material size")
                pixel = bytes((230, 230, 230, 255) if kind == "roughness" else (255, 255, 255, 255))
                value = {"width": expected_size, "height": expected_size,
                         "mips": [pixel * max(1, expected_size >> level)**2 for level in range(expected_size.bit_length())],
                         "source_sha256": hashlib.sha256(b"generated:constant-rgba:" + pixel).hexdigest()}
            else:
                value = textures.resolve(name, kind)
            width, height, mips = value["width"], value["height"], value["mips"]
            digest = value["source_sha256"]
            if readable_bed_wool and name == "bed_wool_v2" and kind == "albedo":
                mips = _readable_wool_mips(mips)
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


def _wall_height(textures):
    value = textures.resolve("stone_wall_v2", "height")
    ao = textures.resolve("stone_wall_v2", "ao")
    width, height, mips, digest = value["width"], value["height"], value["mips"], value["source_sha256"]
    if (type(width) is not int or width != height or width != ao["width"] or width != ao["height"] or
            not 1 <= width <= 1024 or width & (width-1) or len(mips) != width.bit_length() or
            not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest)):
        raise ValueError("invalid bedroom wall height metadata")
    body = bytearray(_text(digest) + struct.pack("<3I", width, height, len(mips)))
    for level, mip in enumerate(mips):
        size = max(1, width >> level)
        if not isinstance(mip, bytes) or len(mip) != size*size*4:
            raise ValueError("invalid bedroom wall height mip")
        body += mip
    return body, digest


def _bedroom_originals(materials, resolver):
    """Pack just the original diffuse maps needed by the captured bedroom pose."""
    body = bytearray(_u32(len(BEDROOM_ORIGINAL_NAMES)))
    manifest = {}
    for name in BEDROOM_ORIGINAL_NAMES:
        matches = [material for material in materials if material.name == name]
        if len(matches) != 1:
            raise ValueError(f"bedroom original material must be unique: {name}")
        value = resolver.resolve(name)
        width, height, mips = value["width"], value["height"], value["mips"]
        if (type(width) is not int or type(height) is not int or not 1 <= width <= 1024 or
                not 1 <= height <= 1024 or width & (width-1) or height & (height-1) or
                len(mips) != max(width, height).bit_length()):
            raise ValueError(f"invalid bedroom original mip chain: {name}")
        for level, mip in enumerate(mips):
            w, h = max(1, width >> level), max(1, height >> level)
            if not isinstance(mip, bytes) or len(mip) != w*h*4 or any(a != 255 for a in mip[3::4]):
                raise ValueError(f"invalid or nonopaque bedroom original mip: {name}")
        digest = hashlib.sha256(b"".join(mips)).hexdigest()
        body += _text(matches[0].id) + _text(digest) + struct.pack("<3I", width, height, len(mips))
        for mip in mips:
            body += mip
        manifest[name] = {"texture": value["name"], "bundle": value["bundle"],
                          "decoded_mips_sha256": digest, "width": width, "height": height}
    return body, manifest


def _candlestick_metal(textures):
    value = textures.resolve("candlestick_metal_v2", "albedo")
    width, height, mips, digest = value["width"], value["height"], value["mips"], value["source_sha256"]
    if (type(width) is not int or width != height or not 1 <= width <= 1024 or width & (width-1) or
            not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest) or
            len(mips) != width.bit_length()):
        raise ValueError("invalid candlestick metal albedo")
    body = bytearray(_text("remaster:material:candlestick_metal_v2") + _text(digest) +
                     struct.pack("<3I", width, height, len(mips)))
    for level, mip in enumerate(mips):
        size = max(1, width >> level)
        if not isinstance(mip, bytes) or len(mip) != size*size*4 or any(alpha != 255 for alpha in mip[3::4]):
            raise ValueError("invalid or nonopaque candlestick metal mip")
        body += mip
    return body, digest


def _candlestick_wax(resolver):
    value = resolver.resolve("GENcandlewax_ON")
    width, height, mips = value["width"], value["height"], value["mips"]
    if (type(width) is not int or type(height) is not int or not 1 <= width <= 1024 or
            not 1 <= height <= 1024 or width & (width-1) or height & (height-1) or
            len(mips) != max(width, height).bit_length()):
        raise ValueError("invalid original candle wax mip chain")
    for level, mip in enumerate(mips):
        w, h = max(1, width >> level), max(1, height >> level)
        if not isinstance(mip, bytes) or len(mip) != w*h*4:
            raise ValueError("invalid original candle wax mip")
    digest = hashlib.sha256(b"".join(mips)).hexdigest()
    def luminance(mip, i):
        return (54*mip[i] + 183*mip[i+1] + 19*mip[i+2]) / 256
    reference = sum(luminance(mips[0], i) for i in range(0, len(mips[0]), 4)) / (width*height)
    if reference <= 0:
        raise ValueError("original candle wax is black")
    # Keep the original surface variation, tinted to the existing generated
    # wax material (linear 0.85, 0.75, 0.55) and made opaque for this mesh.
    def srgb(linear):
        return round(255 * (12.92*linear if linear <= .0031308 else 1.055*linear**(1/2.4)-.055))
    tint = tuple(srgb(value) for value in (.85, .75, .55))
    body = bytearray(_text("remaster:material:candlestick_wax_original") + _text(digest) +
                     struct.pack("<3I", width, height, len(mips)))
    for mip in mips:
        for i in range(0, len(mip), 4):
            ratio = luminance(mip, i) / reference
            body += bytes((*[min(255, round(value*ratio)) for value in tint], 255))
    return body, {"texture": value["name"], "bundle": value["bundle"], "decoded_mips_sha256": digest,
                  "width": width, "height": height, "appearance": "original_luminance_tinted_opaque_preview"}


def _static_mesh_candidates(scene, bed, materials, bedroom_props=False):
    """Collect eligible offline actors without copying their transforms."""
    map_name = scene.source.map
    map_sha = scene.source.fingerprints['map']
    fingerprints = {scene.source.fingerprints.get('bundle'), scene.source.fingerprints.get('kernel')}
    by_id = {item.id: item for item in scene.static_mesh_resources}
    names = {}
    for resource in scene.static_mesh_resources:
        names.setdefault(resource.name.lower(), set()).add(resource.id)
    actor_prefix = f'actor:{map_name}:{map_sha}:StaticMeshActor__'
    crate_id = f'actor:{map_name}:{map_sha}:D_1224__21'
    crate_ids = {f'actor:{map_name}:{map_sha}:D_1224__{suffix}' for suffix in (0, 21, 22, 23)}
    cabinet_id = f'actor:{map_name}:{map_sha}:D_443__1'
    bread_id = f'actor:{map_name}:{map_sha}:D_125__3'
    target_id = f'actor:{map_name}:{map_sha}:D_548__0'
    resource_prefix = f'mesh:{map_name}:{map_sha}:'
    actors, resources, excluded = [], {}, []
    for instance in scene.mesh_instances:
        crate = bedroom_props and instance.id in crate_ids
        cabinet = bedroom_props and instance.id == cabinet_id
        bread = bedroom_props and instance.id == bread_id
        target = bedroom_props and instance.id == target_id
        if not instance.id.startswith(actor_prefix) and not crate and not cabinet and not bread and not target:
            continue
        suffix = instance.id.rsplit('__', 1)[-1] if crate else '1' if cabinet else '3' if bread else '0' if target else instance.id[len(actor_prefix):]
        resource = by_id.get(instance.resource_id)
        if not re.fullmatch(r'0|[1-9][0-9]*', suffix) or int(suffix) > 65534 or resource is None:
            excluded.append(instance.id); continue
        name = resource.name
        name_ok = bool(name) and name.isascii() and all(33 <= ord(c) <= 126 and c != ':' for c in name)
        fingerprint = resource.id[len(resource_prefix):].split(':', 1)[0] if resource.id.startswith(resource_prefix) else ''
        expected = resource_prefix + fingerprint + ':' + name.lower()
        ids = instance.material_ids or tuple(section.material_id for section in resource.geometry.sections)
        section_materials = (section.material_id for section in resource.geometry.sections)
        geometry = resource.geometry
        if (not name_ok or (crate and name.lower() != 'genxgoodscrateb') or
                (cabinet and name.lower() != 'seainfurncabinetc1') or
                (bread and name.lower() != 'genifoodbreada1') or
                (target and name.lower() != 'genxgoodsarchtarg') or
                fingerprint not in fingerprints or resource.id != expected or
                len(names.get(name.lower(), ())) != 1 or len(ids) != len(resource.geometry.sections) or
                not 1 <= len(geometry.positions) <= 65536 or not 1 <= len(geometry.indices) <= 65536 or
                not 1 <= len(geometry.sections) <= 16 or
                not ids or any(item is None or item not in materials for item in ids) or
                any(item is not None and item not in materials for item in section_materials)):
            excluded.append(instance.id); continue
        actors.append((instance.id, resource.id, tuple(ids)))
        if bed is None or resource.id != bed.id:
            resources[resource.id] = (resource, fingerprint)
    target = actor_prefix + '1520'
    if bed is not None and target not in {actor for actor, _, _ in actors}:
        raise ValueError('target bed is not a qualified static mesh candidate')
    if bedroom_props and crate_id not in {actor for actor, _, _ in actors}:
        raise ValueError('bedroom crate is not a qualified mesh candidate')
    if len(actors) > 8192 or len(resources) > 4096:
        raise ValueError('catalog static mesh candidates are empty or exceed bounds')
    return sorted(actors), [resources[key] for key in sorted(resources)], sorted(excluded)


def _generic_map_catalog(scene, raw):
    """Export BSP and generic candidates without claiming a bedroom or frame."""
    source, c = scene.source, scene.conventions
    map_name, map_sha = source.map, source.fingerprints['map']
    if len(map_name.encode('utf-8')) > 128:
        raise ValueError('catalog map name exceeds 128 bytes')
    if len(scene.bsp_blocks) > 4096:
        raise ValueError('catalog BSP collection exceeds bounds')
    for block in scene.bsp_blocks:
        if block.source_offset is None or block.id != f'bsp:{map_name}:{map_sha}:{block.source_index}:{block.source_offset}':
            raise ValueError('BSP ID must retain offline index and offset')
    material_by_id = {item.id: item for item in scene.materials}
    actors, resources, excluded = _static_mesh_candidates(scene, None, material_by_id)
    if not scene.bsp_blocks and not actors:
        raise ValueError('catalog requires BSP or an eligible static mesh actor')
    if len(resources) + len(scene.bsp_blocks) > 4096:
        raise ValueError('catalog resource count exceeds residency bounds')
    required = {section.material_id for block in scene.bsp_blocks for section in block.geometry.sections}
    for _, _, material_ids in actors:
        required.update(material_ids)
    for resource, _ in resources:
        required.update(section.material_id for section in resource.geometry.sections if section.material_id is not None)
    if not required or None in required or any(item not in material_by_id for item in required):
        raise ValueError('catalog requires known BSP and candidate materials')
    materials = [item for item in scene.materials if item.id in required]
    if len(materials) > 8192:
        raise ValueError('catalog material count exceeds bounds')
    fingerprints = sorted(value for value in {source.fingerprints.get('bundle'), source.fingerprints.get('kernel')} if value)
    values = (("unknown", "left", "right").index(c.handedness),
              ("unknown", "normal_aligned", "normal_opposed").index(c.winding),
              ("unknown", "top_left", "bottom_left").index(c.uv_origin),
              ("unknown", "zero_to_one").index(c.clip_depth))
    body = bytearray(b'T3LCAT05')
    for value in (map_name, map_sha, hashlib.sha256(raw).hexdigest()):
        body += _text(value)
    body += struct.pack('<4I', *values) + _u32(int(c.meters_per_unit is not None))
    if c.meters_per_unit is not None:
        body += _float(c.meters_per_unit)
    body += _u32(len(materials)) + _u32(len(scene.bsp_blocks))
    for material in materials:
        body += _text(material.id) + _text(material.name)
    for block in scene.bsp_blocks:
        body += _resource(block.id, f'BSP {block.source_index}', map_sha, block.geometry)
    body += _u32(len(fingerprints))
    for fingerprint in fingerprints:
        body += _text(fingerprint)
    body += _u32(len(resources))
    for resource, fingerprint in resources:
        body += _resource(resource.id, resource.name, fingerprint, resource.geometry)
    body += _u32(len(actors))
    for actor, resource_id, ids in actors:
        body += _text(actor) + _text(resource_id) + _u32(len(ids))
        for material_id in ids:
            body += _text(material_id)
    if len(body) + 32 > MAX_BYTES:
        raise ValueError('catalog exceeds 64 MiB')
    data = bytes(body) + hashlib.sha256(body).digest()
    manifest = {'schema': 't3.scene.live-catalog', 'version': 5, 'source': 'offline',
                'offline_scene_sha256': hashlib.sha256(raw).hexdigest(), 'catalog_sha256': hashlib.sha256(data).hexdigest(),
                'map': map_name, 'map_sha256': map_sha, 'bed_resource_id': None, 'bed_actor_id': None,
                'bsp_blocks': len(scene.bsp_blocks), 'materials': len(materials),
                'static_mesh_candidates': {'actors': len(actors), 'extra_resources': len(resources),
                                           'excluded_actors': excluded,
                                           'scope': 'offline_candidates_without_transforms_or_visibility'},
                'limitations': ['Offline assets only; no frame, visibility, transform or live binding is claimed.',
                                'BSP catalog completeness is inherited from the heuristic offline importer.',
                                'No bedroom-specific bed, mesh override or material library is included.']}
    return data, manifest


def export(raw, bed_high=None, bedroom_materials=None, static_meshes=False, general_map=False, wall_height=False,
           bedroom_props=False, bed_textiles=False, bedroom_originals=None, candlestick_metal=False,
           readable_bed_wool=False):
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
    if general_map:
        if bed_high is not None or bedroom_materials is not None or wall_height or bedroom_props or bed_textiles or bedroom_originals is not None or candlestick_metal or readable_bed_wool:
            raise ValueError('general-map catalogs cannot include bedroom-specific overrides')
        return _generic_map_catalog(scene, raw)
    if type(wall_height) is not bool or (wall_height and bedroom_materials is None):
        raise ValueError("wall height requires bedroom materials")
    if bedroom_props and (not static_meshes or wall_height or map_name != 'SouthQuarter1_int1'):
        raise ValueError('bedroom props require static meshes and the bedroom map, without wall height')
    if type(bed_textiles) is not bool or (bed_textiles and (not bedroom_props or bed_high is None or bedroom_materials is None)):
        raise ValueError('bed textiles require bedroom props, the high-poly bed and bedroom materials')
    if bedroom_originals is not None and not bed_textiles:
        raise ValueError('bedroom original materials require bed textiles')
    if type(candlestick_metal) is not bool or (candlestick_metal and bedroom_originals is None):
        raise ValueError('candlestick metal requires bedroom original materials')
    if type(readable_bed_wool) is not bool or (readable_bed_wool and not bed_textiles):
        raise ValueError('readable bed wool requires bed textiles')
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
    mesh_actors, mesh_resources, excluded = _static_mesh_candidates(scene, bed, material_by_id, bedroom_props) if static_meshes else ([], [], [])
    if static_meshes and len(mesh_resources) + len(scene.bsp_blocks) + 1 > 4096:
        raise ValueError('catalog resource count exceeds residency bounds')
    required = {s.material_id for r in [bed, *scene.bsp_blocks] for s in r.geometry.sections if s.material_id is not None}
    required.update(x for x in instance.material_ids if x is not None)
    for _, _, ids in mesh_actors:
        required.update(ids)
    for resource, _ in mesh_resources:
        required.update(s.material_id for s in resource.geometry.sections if s.material_id is not None)
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
    if (static_meshes or bedroom_materials is not None) and any("remaster:material:"+name in material_by_id
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
    body = bytearray(b"T3LCAT10" if candlestick_metal else b"T3LCAT09" if bedroom_originals is not None else b"T3LCAT08" if bed_textiles else b"T3LCAT07" if bedroom_props else b"T3LCAT06" if wall_height else b"T3LCAT04" if static_meshes else b"T3LCAT03" if bedroom_materials is not None else b"T3LCAT02" if replacement else b"T3LCAT01")
    if wall_height:
        body += _u32(int(static_meshes))
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
    if static_meshes or bedroom_materials is not None:
        body += _u32(int(replacement is not None))
    if static_meshes or wall_height:
        body += _u32(int(bedroom_materials is not None))
    if replacement:
        identifier, digest, geometry = replacement
        body += _resource(identifier, "bed_high", digest, geometry)
    material_sources = None
    if bedroom_materials is not None:
        packed, material_sources = _material_library(bedroom_materials, bed_textiles, readable_bed_wool)
        body += packed
    original_sources = None
    if bedroom_originals is not None:
        packed, original_sources = _bedroom_originals(materials, bedroom_originals)
        body += packed
    metal_source = None
    if candlestick_metal:
        packed, metal_source = _candlestick_metal(bedroom_materials)
        body += packed
        packed, wax_source = _candlestick_wax(bedroom_originals)
        body += packed
    height_source = None
    if wall_height:
        packed, height_source = _wall_height(bedroom_materials)
        body += packed
    if static_meshes:
        fingerprints = sorted(value for value in {source.fingerprints.get('bundle'), source.fingerprints.get('kernel')} if value)
        body += _u32(len(fingerprints))
        for fingerprint in fingerprints:
            body += _text(fingerprint)
        body += _u32(len(mesh_resources))
        for resource, fingerprint in mesh_resources:
            body += _resource(resource.id, resource.name, fingerprint, resource.geometry)
        body += _u32(len(mesh_actors))
        for actor, resource_id, ids in mesh_actors:
            body += _text(actor) + _text(resource_id) + _u32(len(ids))
            for material_id in ids:
                body += _text(material_id)
    if len(body) + 32 > MAX_BYTES:
        raise ValueError("catalog exceeds 64 MiB")
    data = bytes(body) + hashlib.sha256(body).digest()
    manifest = {"schema": "t3.scene.live-catalog", "version": 10 if candlestick_metal else 9 if bedroom_originals is not None else 8 if bed_textiles else 7 if bedroom_props else 6 if wall_height else 4 if static_meshes else 3 if bedroom_materials is not None else 2 if replacement else 1, "source": "offline",
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
        if bed_textiles:
            manifest["bedroom_materials"]["generated_constant_channels"] = {
                "bed_wool_v2": ["ao"], "pillow_linen": ["roughness", "ao"]}
        if readable_bed_wool:
            manifest["bedroom_materials"]["bed_wool_albedo_linear_gain"] = 5
        if height_source:
            manifest["bedroom_materials"]["wall_height_sha256"] = height_source
        manifest["limitations"].append("Materials are offline visual assets; the flat viewer does not infer runtime lighting.")
    if static_meshes:
        manifest["static_mesh_candidates"] = {"actors": len(mesh_actors), "extra_resources": len(mesh_resources),
                                              "excluded_actors": excluded,
                                              "scope": "offline_candidates_without_transforms_or_visibility"}
        manifest["limitations"].append("Static mesh actor/resource/skin candidates are offline; none establishes a runtime binding.")
    if bedroom_props:
        manifest["limitations"].append("Bedroom prop meshes remain candidates until their current mesh and draw are observed.")
    if original_sources is not None:
        manifest["bedroom_original_albedo"] = original_sources
        manifest["limitations"].append("Original diffuse maps are offline material candidates, not observed GPU bindings; PBR auxiliary channels use the wood preview.")
    if metal_source is not None:
        manifest["candlestick_metal_albedo_sha256"] = metal_source
        manifest["candlestick_wax_albedo"] = wax_source
        manifest["limitations"].append("Candlestick metal uses generated albedo; wax uses original luminance tinted to the generated wax color with opaque preview alpha. Both reuse wood-preview PBR auxiliary channels.")
    return data, manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("offline_scene", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--bed-high", type=Path, help="optional generated Godot bed GLB for the live viewer")
    parser.add_argument("--bedroom-materials", type=Path, help="optional generated Godot material directory")
    parser.add_argument("--bedroom-texture-size", type=int, default=512, help="power-of-two material texture size, up to 1024")
    parser.add_argument("--wall-height", action="store_true", help="include the bedroom head-wall height map for the POM preview")
    parser.add_argument("--static-meshes", action="store_true", help="include bounded offline static mesh actor candidates")
    parser.add_argument("--bedroom-props", action="store_true", help="include specific bedroom prop candidates; requires --static-meshes")
    parser.add_argument("--bed-textiles", action="store_true", help="include the generated bed's wool and pillow textures; requires --bedroom-props, --bed-high and --bedroom-materials")
    parser.add_argument("--bedroom-originals-game-dir", type=Path, help="include original diffuse maps for the captured bedroom pose")
    parser.add_argument("--candlestick-metal", action="store_true", help="include generated metal and original-derived wax albedos")
    parser.add_argument("--readable-bed-wool", action="store_true", help="lift the existing wool albedo for the candle preview")
    parser.add_argument("--general-map", action="store_true", help="export BSP and static mesh candidates without a bedroom bed")
    args = parser.parse_args(argv)
    try:
        raw = preview.read_limited(args.offline_scene, preview.MAX_INPUT)
        high = preview.read_limited(args.bed_high, generated_bed.generated.MAX_GLB_BYTES) if args.bed_high else None
        textures = (material_overrides.GodotTextures(args.bedroom_materials, args.bedroom_texture_size, True)
                    if args.bedroom_materials else None)
        originals = None
        if args.bedroom_originals_game_dir:
            from t3common import resolve_map
            from ibt import IBT
            scene = SceneSnapshot.from_json(raw)
            local = resolve_map(args.bedroom_originals_game_dir, scene.source.map, ".ibt")
            kernel = local.parent / "Kernel_GFXALL.ibt"
            if (packet.digest_file(local) != scene.source.fingerprints.get("bundle") or
                    packet.digest_file(kernel) != scene.source.fingerprints.get("kernel")):
                raise ValueError("game bundle fingerprints do not match the offline scene")
            originals = packet.TextureResolver([IBT(local), IBT(kernel)])
        body, manifest = export(raw, high, textures, args.static_meshes, args.general_map, args.wall_height, args.bedroom_props, args.bed_textiles, originals, args.candlestick_metal, args.readable_bed_wool)
        packet.write_pair(args.output, body, manifest)
    except (OSError, ValueError, UnicodeError) as exc:
        parser.exit(2, f"{exc}\n")
    print(f"offline_catalog=ok bytes={len(body)} bsp_blocks={manifest['bsp_blocks']}")


if __name__ == "__main__":
    main()
