"""Package semantic bed variants generated from the same scoped SceneIR."""
import hashlib
import struct

MAX_PACKET = 64 * 1024 * 1024
MAX_KEY = 512


def packet_signatures(raw):
    """Canonicalize draw inputs so differing texture indices do not hide changes."""
    view = memoryview(raw)
    offset = 0
    def take(size):
        nonlocal offset
        if size < 0 or size > len(view)-offset:
            raise ValueError("truncated PBR variant packet")
        result = view[offset:offset+size]
        offset += size
        return result
    header_bytes = take(196)
    header = struct.unpack("<8sIIff32f4f4fIII", header_bytes)
    magic, texture_count, draw_count, light_count = header[0], *header[-3:]
    if (magic not in (b"T3PBR01\0", b"T3PBR02\0", b"T3PBR03\0", b"T3PBR04\0", b"T3PBR05\0", b"T3PBR06\0") or
            not 1 <= texture_count <= 64 or not 1 <= draw_count <= 8192 or not 1 <= light_count <= 16):
        raise ValueError("invalid PBR variant packet header")
    version = magic[6]-ord("0")
    flags = struct.unpack("<I", take(4))[0] if version >= 5 else (1 if version >= 2 else 0)
    if flags > (7 if version == 6 else 1) or (version == 6 and flags & 4 and not flags & 2):
        raise ValueError("invalid PBR variant flags")
    shadows = bool(flags & 1)
    casters = 0
    shadow_settings = b""
    if shadows:
        shadow_settings = bytes(take(20))
        if version >= 3:
            casters = struct.unpack("<I", take(4))[0]
            if casters > 8192:
                raise ValueError("PBR variant caster count exceeds bounds")
    light_data = bytes(take(light_count*(52 if version == 6 else 32)))
    context = bytes(header_bytes[:184]) + struct.pack("<II", flags, light_count) + shadow_settings + light_data
    textures = []
    for _ in range(texture_count):
        width, height, levels = struct.unpack("<III", take(12))
        if not 1 <= width <= 2048 or not 1 <= height <= 2048 or not 1 <= levels <= 12:
            raise ValueError("invalid PBR variant texture")
        digest = hashlib.sha256(struct.pack("<III", width, height, levels))
        for _ in range(levels):
            digest.update(take(width*height*4))
            width, height = max(1, width//2), max(1, height//2)
        textures.append(digest.digest())
    draws = []
    for _ in range(draw_count):
        tex0, tex1, tex2, tex3, count = struct.unpack("<5I", take(20))
        if any(index >= texture_count for index in (tex0,tex1,tex2,tex3)) or not count or count % 3 or count > 300000:
            raise ValueError("invalid PBR variant draw")
        mask = bytes(take(4)) if shadows else b""
        emission = bytes(take(12)) if (flags & 2 if version == 6 else version >= 4) else b""
        metallic = bytes(take(4)) if (flags & 4 if version == 6 else version >= 5) else b""
        vertex_offset = offset
        vertices = take(count*48)
        signature = (tuple(textures[index] for index in (tex0,tex1,tex2,tex3)), count, mask,
                     emission, metallic, hashlib.sha256(vertices).digest())
        draws.append((signature, vertex_offset))
    caster_signatures = []
    for _ in range(casters):
        mask, count = struct.unpack("<II", take(8))
        if not mask or not count or count % 3 or count > 300000:
            raise ValueError("invalid PBR variant caster")
        caster_signatures.append((mask, count, hashlib.sha256(take(count*12)).digest()))
    if offset != len(view):
        raise ValueError("unexpected trailing PBR variant data")
    return draws, caster_signatures, context


def build(scene, original, original_stats, replacement, replacement_stats):
    report = replacement_stats.get("generated_bed")
    if (not isinstance(original, bytes) or not isinstance(replacement, bytes) or
            not 1 <= len(original) <= MAX_PACKET or not 1 <= len(replacement) <= MAX_PACKET or
            original[:8] != replacement[:8] or not original.startswith(b"T3PBR0") or
            not isinstance(report, dict) or original_stats.get("generated_bed") is not None or
            scene.source.kind != "runtime" or scene.source.camera_synchronized is not True):
        raise ValueError("invalid semantic bed variant inputs")
    target, resource_id = report.get("target_id"), report.get("resource_id")
    instances = [i for i in scene.mesh_instances if i.id == target and i.resource_id == resource_id]
    resources = [r for r in scene.static_mesh_resources if r.id == resource_id and r.name.lower() == "innbed1a1"]
    if len(instances) != 1 or len(resources) != 1 or report.get("resource_name", "").lower() != "innbed1a1":
        raise ValueError("bed variant does not match the semantic resource")
    try:
        key = resource_id.encode("ascii")
    except (AttributeError, UnicodeEncodeError) as exc:
        raise ValueError("invalid semantic resource key") from exc
    if (not 1 <= len(key) <= MAX_KEY or any(c < 33 or c > 126 for c in key) or
            not (key.startswith(b"mesh:") or key.startswith(b"asset:"))):
        raise ValueError("invalid semantic resource key")
    for field in ("render_pass", "stage", "lights", "candle_flames", "generated_candlesticks", "generated_arch",
                  "generated_bedroom_details", "bedroom_prop_materials", "metallic_mode"):
        if original_stats.get(field) != replacement_stats.get(field):
            raise ValueError(f"bed variants disagree on {field}")
    old_shadow, new_shadow = original_stats.get("shadow_maps"), replacement_stats.get("shadow_maps")
    if (old_shadow is None) != (new_shadow is None):
        raise ValueError("bed variants disagree on shadow settings")
    if old_shadow is not None:
        for field in ("size", "near_m", "far_m", "bias_m", "softness", "casters"):
            if old_shadow.get(field) != new_shadow.get(field):
                raise ValueError("bed variants disagree on shadow settings")
        old_candidates, new_candidates = old_shadow.get("offline_candidates"), new_shadow.get("offline_candidates")
        if (old_candidates is None) != (new_candidates is None):
            raise ValueError("bed variants disagree on offline caster provenance")
        if old_candidates is not None:
            old_sections = [x for x in old_candidates["sections"] if x.get("target_id") != target]
            new_sections = [x for x in new_candidates["sections"] if x.get("target_id") != target]
            if old_sections != new_sections:
                raise ValueError("bed variants alter unrelated offline casters")
    old_draws, new_draws = original_stats.get("draws"), replacement_stats.get("draws")
    if not isinstance(old_draws, list) or not isinstance(new_draws, list):
        raise ValueError("bed variants require draw metadata")
    if not any(d.get("target_id") == target for d in old_draws) or not any(
            d.get("target_id") == target and d.get("replacement") == "bed_high" for d in new_draws):
        raise ValueError("both bed variants must contain visible geometry")
    if ([d for d in old_draws if d.get("target_id") != target] !=
            [d for d in new_draws if d.get("target_id") != target] or
            [d for d in original_stats.get("skipped", []) if d.get("target_id") != target] !=
            [d for d in replacement_stats.get("skipped", []) if d.get("target_id") != target]):
        raise ValueError("bed variants alter unrelated draws")
    old_geometry, old_casters, old_context = packet_signatures(original)
    new_geometry, new_casters, new_context = packet_signatures(replacement)
    if old_context != new_context:
        raise ValueError("bed variants alter camera, lighting or shadow context")
    if len(old_geometry) != len(old_draws) or len(new_geometry) != len(new_draws):
        raise ValueError("bed variant draw metadata does not match the packet")
    if ([sig for record, (sig, _) in zip(old_draws, old_geometry) if record.get("target_id") != target] !=
            [sig for record, (sig, _) in zip(new_draws, new_geometry) if record.get("target_id") != target]):
        raise ValueError("bed variants alter unrelated draw geometry or textures")
    def caster_records(shadow):
        return [] if shadow is None else shadow.get("offline_candidates", {}).get("sections", [])
    old_sections, new_sections = caster_records(old_shadow), caster_records(new_shadow)
    if len(old_sections) != len(old_casters) or len(new_sections) != len(new_casters):
        raise ValueError("bed variant caster metadata does not match the packet")
    if ([sig for record, sig in zip(old_sections, old_casters) if record.get("target_id") != target] !=
            [sig for record, sig in zip(new_sections, new_casters) if record.get("target_id") != target]):
        raise ValueError("bed variants alter unrelated caster geometry")
    body = struct.pack("<8sIII", b"T3VAR01\0", len(key), len(original), len(replacement)) + key + original + replacement
    return body, {"resource_key": resource_id, "actor_id": target,
                  "original_packet_sha256": hashlib.sha256(original).hexdigest(),
                  "replacement_packet_sha256": hashlib.sha256(replacement).hexdigest(),
                  "replacement_asset_sha256": report["source_sha256"],
                  "history_on_switch": "reset"}
