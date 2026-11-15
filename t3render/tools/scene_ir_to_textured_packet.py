#!/usr/bin/env python3
"""Resolve a scoped SceneIR v1 and T3SDK textures for the SceneIR DX12 renderer.

The output is a bounded local rendering packet, not a D3D8 command stream.
Material-to-diffuse texture bindings come from the fingerprinted offline IBTs;
they are not claims that the original shader sampled those exact bytes.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "assets"))
sys.path.insert(0, str(ROOT / "t3render" / "ir"))

from ibt import IBT  # noqa: E402
from scene_ir import SceneSnapshot, transform_point  # noqa: E402
from t3common import game_dir, resolve_map  # noqa: E402
from t3texture import TYPE_MATERIAL, TYPE_TEXTURE, decode_rgba, mip_size, parse_material, parse_texture  # noqa: E402
import scene_ir_to_d3d12_packet as preview  # noqa: E402
import scene_legacy_states as legacy_states  # noqa: E402
import scene_ambient_candidates as ambient  # noqa: E402
import scene_direct_candidates as direct  # noqa: E402
import scene_fog_candidates as fog  # noqa: E402

MAGIC = b"T3SCN03\0"
AMBIENT_MAGIC = b"T3SCN04\0"
DIRECT_MAGIC = b"T3SCN05\0"
SPOT_MAGIC = b"T3SCN06\0"
PROJECTOR_MAGIC = b"T3SCN07\0"
FOG_MAGIC = b"T3SCN08\0"
MAX_BYTES = 64 * 1024 * 1024
MAX_TEXTURES = 64
MAX_DRAWS = 8192
MAX_TRIANGLES = 100000


def normal_matrix(matrix):
    """Inverse transpose of the row-vector affine transform's linear part."""
    a, b, c, d, e, f, g, h, i = (matrix[k] for k in (0, 1, 2, 4, 5, 6, 8, 9, 10))
    cofactors = (e*i-f*h, f*g-d*i, d*h-e*g,
                 c*h-b*i, a*i-c*g, b*g-a*h,
                 b*f-c*e, c*d-a*f, a*e-b*d)
    determinant = a * cofactors[0] + b * cofactors[1] + c * cofactors[2]
    if not math.isfinite(determinant) or abs(determinant) < 1e-12:
        raise ValueError("cannot transform normals with a singular matrix")
    result = tuple(x / determinant for x in cofactors)
    if any(not math.isfinite(x) for x in result):
        raise ValueError("normal transform is not finite")
    return result


def transform_normal(normal, matrix):
    value = tuple(sum(normal[i] * matrix[i * 3 + j] for i in range(3)) for j in range(3))
    length = math.sqrt(sum(x*x for x in value))
    if not math.isfinite(length) or length < 1e-12:
        raise ValueError("normal is zero or nonfinite")
    return tuple(x / length for x in value)


def digest_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class TextureResolver:
    def __init__(self, bundles):
        self.bundles = bundles
        self.cache = {}
        self.textures = {}
        self.decoded_bytes = 0

    def find(self, name, kind):
        for bundle in self.bundles:
            matches = [r for r in bundle.resources if r.type == kind and r.name.lower() == name.lower()]
            if len(matches) > 1:
                raise ValueError(f"ambiguous resource {name} in {bundle.path.name}")
            if matches:
                return bundle, matches[0]
        raise ValueError(f"missing resource {name}")

    def resolve(self, name):
        key = name.lower()
        if key in self.cache:
            return self.cache[key]
        bundle, resource = self.find(name, TYPE_MATERIAL)
        material = parse_material(resource, bundle.split(resource))
        texture_name = material.texture(0)
        if not texture_name:
            raise ValueError(f"material {name} has no diffuse texture")
        texture_bundle, texture_resource = self.find(texture_name, TYPE_TEXTURE)
        value = self.decode_texture(texture_bundle, texture_resource, texture_name)
        self.cache[key] = value
        return value

    def resolve_projector(self, name):
        """Allow duplicate named entries only when all bytes and part boundaries agree."""
        for bundle in self.bundles:
            matches = [r for r in bundle.resources if r.type == TYPE_TEXTURE and r.name.lower() == name.lower()]
            if matches:
                parts = bundle.split(matches[0])
                if any(bundle.split(resource) != parts for resource in matches[1:]):
                    raise ValueError(f"ambiguous projector texture {name} in {bundle.path.name}")
                return self.decode_texture(bundle, matches[0], name, parts)
        raise ValueError(f"missing projector texture {name}")

    def decode_texture(self, texture_bundle, texture_resource, texture_name, parts=None):
        texture_key = (str(texture_bundle.path), texture_resource.name.lower())
        if texture_key in self.textures:
            return self.textures[texture_key]
        texture = parse_texture(texture_resource, texture_bundle.split(texture_resource) if parts is None else parts)
        if (not 1 <= texture.width <= 2048 or not 1 <= texture.height <= 2048 or
                not 1 <= len(texture.mips) <= 12):
            raise ValueError(f"texture {texture_name} exceeds renderer bounds")
        decoded_size = sum(max(1, texture.width >> i) * max(1, texture.height >> i) * 4
                           for i in range(len(texture.mips)))
        if decoded_size > MAX_BYTES - self.decoded_bytes:
            raise ValueError("decoded textures exceed renderer bounds")
        mips = []
        for level, mip in enumerate(texture.mips):
            width, height = max(1, texture.width >> level), max(1, texture.height >> level)
            if (mip.level != level or mip.width != width or mip.height != height or
                    len(mip.data) != mip_size(texture.format, width, height) or
                    (level and texture.mips[level - 1].width == texture.mips[level - 1].height == 1)):
                raise ValueError(f"invalid mip chain for {texture_name}")
            decoded_width, decoded_height, rgba = decode_rgba(texture, level)
            if (decoded_width, decoded_height, len(rgba)) != (width, height, width * height * 4):
                raise ValueError(f"invalid decoded texture {texture_name}")
            mips.append(bytes(rgba))
        value = {"name": texture.name, "bundle": texture_bundle.path.name,
                 "format": texture.format, "width": texture.width, "height": texture.height, "mips": mips}
        self.decoded_bytes += decoded_size
        self.textures[texture_key] = value
        return value


def match_sampler(candidate, texture):
    """Use metadata-compatible sampler observations as an explicitly labeled hypothesis."""
    if not isinstance(candidate, dict) or type(candidate.get("stage")) is not int or not 0 <= candidate["stage"] < 8:
        raise ValueError("invalid sampler candidate")
    if candidate.get("status") in ("unbound", "unavailable"):
        return {"status": candidate["status"] + "_default", "stage": candidate["stage"], "mip_lod_bias": 0.0}
    code = {"A8R8G8B8": 21, "X8R8G8B8": 22}.get(texture["format"])
    if texture["format"] in ("DXT1", "DXT3", "DXT5"):
        code = struct.unpack("<I", texture["format"].encode("ascii"))[0]
    if (candidate.get("status") != "candidate" or code is None or
            (candidate.get("width"), candidate.get("height"), candidate.get("levels"), candidate.get("format")) !=
            (texture["width"], texture["height"], len(texture["mips"]), code)):
        raise ValueError("sampler candidate texture metadata does not match asset")
    bias = candidate.get("lod_bias")
    if isinstance(bias, bool) or not isinstance(bias, (int, float)) or not math.isfinite(bias) or not -16 <= bias < 16:
        raise ValueError("invalid sampler candidate LOD bias")
    bias = struct.unpack("<f", struct.pack("<f", bias))[0]
    if not -16 <= bias < 16:
        raise ValueError("sampler candidate LOD bias rounds out of range")
    return {"status": "metadata_match_not_identity", "stage": candidate["stage"], "mip_lod_bias": bias}


def select_scope(scene, render_pass=None, stage=None):
    if scene.version != 1 or scene.source.kind != "runtime" or scene.source.camera_synchronized is not True:
        raise ValueError("textured preview requires a synchronized scoped runtime SceneIR v1")
    preview.validate_camera(scene.camera)
    c = scene.conventions
    if (c.length_unit != "t3_unit" or c.axes != "t3_xyz" or c.up_axis != "z" or
            c.matrix_layout != "row_major" or c.matrix_vectors != "row" or c.clip_depth == "minus_one_to_one"):
        raise ValueError("unsupported coordinate or clip convention")
    scopes = {(s.render_pass, s.stage) for s in scene.submissions if
              (render_pass is None or s.render_pass == render_pass) and (stage is None or s.stage == stage)}
    if len(scopes) != 1:
        raise ValueError("select an unambiguous submission pass and stage")
    return next(iter(scopes))


def create_packet(scene, resolver, render_pass=None, stage=None, draw_states=None, sampler_candidates=None,
                  additional_scenes=(), ambient_candidates=None, direct_candidates=None, fog_candidates=None):
    render_pass, stage = select_scope(scene, render_pass, stage)
    selections = [(scene, render_pass, stage)]
    if additional_scenes and draw_states is None:
        raise ValueError("composed previews require revalidated per-draw states")
    seen_scopes = {(render_pass, stage)}
    identities = {}
    for index, current in enumerate((scene, *additional_scenes)):
        scope = (render_pass, stage) if index == 0 else select_scope(current)
        if index:
            if (current.source.map != scene.source.map or current.source.frame_id != scene.source.frame_id or
                    current.source.fingerprints != scene.source.fingerprints or current.camera != scene.camera or
                    current.conventions != scene.conventions):
                raise ValueError("composed scenes must share frame, fingerprints, camera and conventions")
            if scope[0] != render_pass or scope in seen_scopes:
                raise ValueError("composed scenes must have distinct stages in the same pass")
            seen_scopes.add(scope)
            selections.append((current, *scope))
        for collection in ("static_mesh_resources", "mesh_instances", "bsp_blocks", "materials"):
            for item in getattr(current, collection):
                key = (collection, item.id)
                if key in identities and identities[key] != item:
                    raise ValueError("composed scenes disagree on a shared resource or instance")
                identities[key] = item
    submissions = []
    for current, selected_pass, selected_stage in selections:
        lookup = ({x.id: x for x in current.static_mesh_resources}, {x.id: x for x in current.mesh_instances},
                  {x.id: x for x in current.bsp_blocks}, {x.id: x.name for x in current.materials})
        submissions.extend((s, lookup) for s in current.submissions
                           if (s.render_pass, s.stage) == (selected_pass, selected_stage))
    if not 1 <= len(submissions) <= MAX_DRAWS:
        raise ValueError("draw count exceeds renderer bounds")
    if len({s.draw_id for s, _ in submissions}) != len(submissions):
        raise ValueError("composed scenes contain duplicate draw IDs")
    if ambient_candidates is not None:
        if draw_states is None or not isinstance(ambient_candidates, dict) or not ambient_candidates or not (
                set(ambient_candidates) <= {s.draw_id for s, _ in submissions}):
            raise ValueError("ambient candidates require revalidated submitted draws")
    if direct_candidates is not None:
        if draw_states is None or not isinstance(direct_candidates, dict) or not direct_candidates or not (
                set(direct_candidates) <= {s.draw_id for s, _ in submissions}):
            raise ValueError("direct candidates require revalidated submitted draws")
        if ambient_candidates is not None and set(ambient_candidates) & set(direct_candidates):
            raise ValueError("a draw cannot have ambient and direct candidates")
    if fog_candidates is not None:
        if draw_states is None or not isinstance(fog_candidates, dict) or not fog_candidates or not (
                set(fog_candidates) <= {s.draw_id for s, _ in submissions}):
            raise ValueError("fog candidates require revalidated submitted draws")
        if ((ambient_candidates is not None and set(fog_candidates) & set(ambient_candidates)) or
                (direct_candidates is not None and set(fog_candidates) & set(direct_candidates))):
            raise ValueError("a draw cannot have fog and light candidates")
    extended_packet = direct_candidates is not None or fog_candidates is not None
    spot_packet = direct_candidates is not None and any(
        isinstance(value, dict) and value.get("mode") == "spot_approximation" for value in direct_candidates.values())
    projector_packet = direct_candidates is not None and any(
        isinstance(value, dict) and value.get("projector_candidate") is not None for value in direct_candidates.values())
    if projector_packet and any(isinstance(value, dict) and value.get("projector_candidate") is not None and
                                value.get("mode") != "spot_approximation" for value in direct_candidates.values()):
        raise ValueError("projector texture candidates require spotlight draws")
    if additional_scenes:
        # Trace operation IDs increase within a frame; restore their original
        # order when combining individually verified stage snapshots.
        submissions.sort(key=lambda pair: pair[0].draw_id)
    textures, texture_indices, draws, source_draws = [], {}, [], []
    triangle_count = 0
    for submission, lookup in submissions:
        meshes, instances, blocks, materials = lookup
        if submission.target_kind == "mesh_instance":
            instance = instances[submission.target_id]
            geometry, matrix = meshes[instance.resource_id].geometry, instance.transform
            material_id = (instance.material_ids[submission.section] if instance.material_ids else
                           geometry.sections[submission.section].material_id)
        else:
            geometry, matrix = blocks[submission.target_id].geometry, preview.IDENTITY
            material_id = geometry.sections[submission.section].material_id
        section = geometry.sections[submission.section]
        material_name = materials.get(material_id)
        if not material_name or geometry.uv0 is None:
            raise ValueError("submitted section lacks a material or UV0")
        if any(abs(matrix[i]) > 1e-6 for i in (3, 7, 11)) or abs(matrix[15] - 1) > 1e-6:
            raise ValueError("only affine row-vector transforms are supported")
        light = direct_candidates.get(submission.draw_id) if direct_candidates is not None else None
        if direct_candidates is not None and submission.draw_id in direct_candidates:
            if not isinstance(light, dict) or light.get("mode") not in (
                    "point_approximation", "skipped_spot", "spot_approximation"):
                raise ValueError("invalid direct candidate")
        is_lit = light is not None and light["mode"] in ("point_approximation", "spot_approximation")
        fog_value = fog_candidates.get(submission.draw_id) if fog_candidates is not None else None
        if fog_candidates is not None and submission.draw_id in fog_candidates:
            if not isinstance(fog_value, dict):
                raise ValueError("invalid fog candidate")
            camera_pos, fog_start, fog_inverse, fog_color = (fog_value.get("camera_position"), fog_value.get("start"),
                                                            fog_value.get("inverse_range"), fog_value.get("offline_color"))
            numbers = (list(camera_pos) if isinstance(camera_pos, (list, tuple)) else []) + [fog_start, fog_inverse] + (
                      list(fog_color) if isinstance(fog_color, (list, tuple)) else [])
            if (not isinstance(camera_pos, (list, tuple)) or len(camera_pos) != 3 or
                    not isinstance(fog_color, (list, tuple)) or len(fog_color) != 3 or any(
                        isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x) for x in numbers) or
                    any(abs(x) > 1000000 for x in camera_pos) or not 0 <= fog_start <= 1000000 or
                    not 0 < fog_inverse <= 1 or any(not 0 <= x <= 1 for x in fog_color)):
                raise ValueError("invalid fog candidate parameters")
        if is_lit and geometry.normals is None:
            raise ValueError("direct-light candidate requires source normals")
        normal_xform = normal_matrix(matrix) if is_lit else None
        tex = resolver.resolve(material_name)
        tex_key = (tex["bundle"], tex["name"].lower())
        if tex_key not in texture_indices:
            if len(textures) == MAX_TEXTURES:
                raise ValueError("texture count exceeds renderer bounds")
            texture_indices[tex_key] = len(textures)
            textures.append(tex)
        vertices = []
        for index in geometry.indices[section.first_index:section.first_index + section.index_count]:
            position = transform_point(geometry.positions[index], matrix)
            uv = geometry.uv0[index]
            if (abs(position[3] - 1) > 1e-6 or any(not math.isfinite(v) or abs(v) > 1000000 for v in position[:3] + uv)):
                raise ValueError("vertex exceeds renderer bounds")
            if fog_value is not None:
                amount = (math.dist(position[:3], camera_pos) - fog_start) * fog_inverse
                if not math.isfinite(amount) or abs(amount) > 1000000:
                    raise ValueError("fog factor exceeds renderer bounds")
                normal = (amount, 0, 0)  # Interpolate the legacy vertex fog factor.
            else:
                normal = transform_normal(geometry.normals[index], normal_xform) if is_lit else (0, 0, 0)
            vertices.append((*position[:3], *uv, *normal) if extended_packet else (*position[:3], *uv))
        triangle_count += len(vertices) // 3
        if not vertices or triangle_count > MAX_TRIANGLES:
            raise ValueError("triangle count exceeds renderer bounds")
        if draw_states is not None and submission.draw_id not in draw_states:
            raise ValueError("captured state missing for submitted draw")
        state = legacy_states.validate(dict(legacy_states.DEFAULT if draw_states is None else draw_states[submission.draw_id]))
        if light is not None and (state["depth_enable"] != 1 or state["depth_write"] != 0 or
                state["blend_enable"] != 1 or state["blend_src"] != 1 or state["blend_dst"] != 1 or
                state["blend_op"] != 0 or state["color_mask"] != 7):
            raise ValueError("direct candidates require additive RGB with depth testing and no depth writes")
        if fog_value is not None and (state["depth_enable"] != 1 or state["depth_write"] != 0 or
                state["blend_enable"] != 1 or state["blend_src"] != 4 or state["blend_dst"] != 5 or
                state["blend_op"] != 0 or state["color_mask"] != 7 or state["alpha_compare"] != 0):
            raise ValueError("fog candidates require source-alpha blending with depth testing and no depth writes")
        if sampler_candidates is not None and submission.draw_id not in sampler_candidates:
            raise ValueError("sampler candidate missing for submitted draw")
        sampler = (match_sampler(sampler_candidates[submission.draw_id], tex) if sampler_candidates is not None else
                   {"status": "preview_default", "stage": None, "mip_lod_bias": 0.0})
        color = ambient_candidates.get(submission.draw_id) if ambient_candidates is not None else None
        if ambient_candidates is not None and submission.draw_id in ambient_candidates and (
                not isinstance(color, (tuple, list)) or len(color) != 4 or any(
                isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x) or not 0 <= x <= 1
                for x in color)):
            raise ValueError("invalid ambient candidate color")
        if is_lit:
            position, inverse, rgb = light.get("position"), light.get("inverse_radius_candidate"), light.get("offline_color")
            energy, gain = light.get("offline_energy"), light.get("gain")
            values = (position if isinstance(position, (tuple, list)) else [])
            values = list(values) + [inverse] + (list(rgb) if isinstance(rgb, (tuple, list)) else []) + [energy, gain]
            if (not isinstance(position, (tuple, list)) or len(position) != 3 or
                    not isinstance(rgb, (tuple, list)) or len(rgb) != 3 or any(
                        isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x) for x in values) or
                    any(abs(x) > 1000000 for x in position) or not 0 < inverse <= 1 or
                    any(not 0 <= x <= 1 for x in rgb) or not 0 <= energy <= 1 or not 0 <= gain <= 4):
                raise ValueError("invalid point-light candidate parameters")
            if struct.unpack("<f", struct.pack("<f", inverse))[0] <= 0:
                raise ValueError("point-light inverse radius rounds to zero")
            light_values = (*position, inverse, *rgb, energy * gain)
        else:
            light_values = (0,) * 8
        if fog_value is not None:
            light_values = (0, 0, 0, 0, *fog_color, 0)
        projector_index = 0
        if light is not None and light["mode"] == "spot_approximation":
            rows = light.get("projector_rows")
            if (not isinstance(rows, (list, tuple)) or len(rows) != 3 or any(
                    not isinstance(row, (list, tuple)) or len(row) != 4 or any(
                        isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x) or abs(x) > 1000000
                        for x in row) for row in rows) or sum(x*x for x in rows[2][:3]) < 1e-12):
                raise ValueError("invalid spot candidate projector")
            projector = tuple(x for row in rows for x in row)
            mode = 4
            candidate = light.get("projector_candidate")
            if candidate is not None:
                if (not isinstance(candidate, dict) or not isinstance(candidate.get("name"), str) or not candidate["name"] or
                        any(type(candidate.get(key)) is not int or candidate[key] <= 0
                            for key in ("width", "height", "levels", "format"))):
                    raise ValueError("invalid projector texture candidate")
                projector_texture = resolver.resolve_projector(candidate["name"])
                if projector_texture["name"].lower() != candidate["name"].lower():
                    raise ValueError("projector texture candidate name does not match asset")
                match_sampler({**candidate, "status": "candidate", "stage": 3, "lod_bias": 0}, projector_texture)
                projector_key = (projector_texture["bundle"], projector_texture["name"].lower())
                if projector_key not in texture_indices:
                    if len(textures) == MAX_TEXTURES:
                        raise ValueError("texture count exceeds renderer bounds")
                    texture_indices[projector_key] = len(textures)
                    textures.append(projector_texture)
                projector_index = texture_indices[projector_key]
                mode = 5
        else:
            projector = (0,) * 12
            mode = 6 if fog_value is not None else 2 if is_lit else 3 if light is not None else 1 if color is not None else 0
        draws.append((texture_indices[tex_key], vertices, state, sampler, color, mode, light_values, projector, projector_index))
        source_draws.append({"draw": submission.draw_id, "target_kind": submission.target_kind,
                             "target_id": submission.target_id, "section": submission.section,
                             "render_pass": submission.render_pass, "stage": submission.stage,
                             "material": material_name, "texture": tex["name"], "triangles": len(vertices) // 3,
                             "state": state, "sampler": sampler})
        if color is not None:
            source_draws[-1]["ambient_candidate"] = list(color)
        if light is not None:
            source_draws[-1]["direct_candidate"] = light
        if fog_value is not None:
            source_draws[-1]["fog_candidate"] = fog_value
    camera = scene.camera
    viewport = camera.viewport
    size = (struct.calcsize("<8sIIff32fII") + sum(12 + sum(len(mip) for mip in tex["mips"]) for tex in textures) +
            sum(56 + (20 if ambient_candidates is not None or extended_packet else 0) +
                (32 if extended_packet else 0) + len(vertices) * (32 if extended_packet else 20)
                + (48 if spot_packet or fog_candidates is not None else 0) + (4 if projector_packet or fog_candidates is not None else 0)
                for _, vertices, _, _, _, _, _, _, _ in draws))
    if size > MAX_BYTES:
        raise ValueError("packet exceeds renderer bounds")
    magic = (FOG_MAGIC if fog_candidates is not None else PROJECTOR_MAGIC if projector_packet else SPOT_MAGIC if spot_packet else
             DIRECT_MAGIC if direct_candidates is not None else AMBIENT_MAGIC if ambient_candidates is not None else MAGIC)
    body = bytearray(struct.pack("<8sIIff32fII", magic,
                                 viewport.width, viewport.height,
                                 viewport.min_depth, viewport.max_depth, *camera.view, *camera.projection,
                                 len(textures), len(draws)))
    for tex in textures:
        body.extend(struct.pack("<III", tex["width"], tex["height"], len(tex["mips"])))
        for mip in tex["mips"]:
            body.extend(mip)
    for texture_index, vertices, state, sampler, color, mode, light_values, projector, projector_index in draws:
        body.extend(struct.pack("<13If", texture_index, len(vertices), *(state[key] for key in legacy_states.FIELDS),
                                sampler["mip_lod_bias"]))
        if ambient_candidates is not None or extended_packet:
            body.extend(struct.pack("<I4f", mode, *(color if color is not None else (0, 0, 0, 0))))
        if extended_packet:
            body.extend(struct.pack("<8f", *light_values))
        if spot_packet or fog_candidates is not None:
            body.extend(struct.pack("<12f", *projector))
        if projector_packet or fog_candidates is not None:
            body.extend(struct.pack("<I", projector_index))
        for vertex in vertices:
            body.extend(struct.pack("<8f" if extended_packet else "<5f", *vertex))
    if len(body) != size:
        raise ValueError("packet size mismatch")
    stats = {"render_pass": render_pass, "stage": stage, "triangles": triangle_count, "draws": source_draws,
             "textures": [{key: tex[key] for key in ("name", "bundle", "format", "width", "height")}
                          | {"mips": len(tex["mips"])} for tex in textures]}
    if additional_scenes:
        stats["composed_scopes"] = [{"render_pass": p, "stage": s} for _, p, s in selections]
    return bytes(body), stats


def write_pair(path, body, manifest):
    paths = ((path, body), (Path(str(path) + ".manifest.json"),
                           (json.dumps(manifest, indent=2, allow_nan=False) + "\n").encode()))
    created = []
    try:
        for destination, content in paths:
            with destination.open("xb") as stream:
                identity = os.fstat(stream.fileno())
                created.append((destination, identity.st_dev, identity.st_ino))
                stream.write(content)
    except Exception:
        for destination, device, inode in created:
            try:
                current = destination.lstat()
                if (current.st_dev, current.st_ino) == (device, inode):
                    destination.unlink()
            except OSError:
                pass
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene", type=Path)
    parser.add_argument("--game-dir")
    parser.add_argument("--pass", dest="render_pass", type=int)
    parser.add_argument("--stage", type=int)
    parser.add_argument("--additional-scene", type=Path, action="append", default=[],
                        help="compose another same-frame scoped stage; requires trace revalidation")
    parser.add_argument("--state-trace", type=Path, help="optional synchronized legacy state observation")
    parser.add_argument("--offline-scene", type=Path, help="original fixture used to revalidate scoped state")
    parser.add_argument("--sampler-candidate-stage", type=int,
                        help="hypothesize a metadata-compatible texture stage; requires --state-trace")
    parser.add_argument("--ambient-candidate-stage", type=int,
                        help="use a pre-Reset ambient shader hypothesis for this submission stage")
    parser.add_argument("--direct-candidate-stage", type=int,
                        help="approximate scoped direct-light candidates for this submission stage")
    parser.add_argument("--fog-candidate-stage", type=int,
                        help="preview a scoped distance-fog hypothesis for this submission stage")
    parser.add_argument("--spot-candidates", action="store_true",
                        help="also preview validated spotlight projector candidates with an approximate circular mask")
    parser.add_argument("--spot-projector-candidate", help="explicitly hypothesize a metadata-compatible offline projector texture")
    parser.add_argument("--offline-actors", type=Path, help="fingerprinted t3map actor export for candidate light or fog colors")
    parser.add_argument("--direct-preview-gain", type=float, default=1.0,
                        help="uncalibrated visual gain for candidate direct lights (default: 1)")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        raw = preview.read_limited(args.scene, preview.MAX_INPUT)
        scene = SceneSnapshot.from_json(raw)
        extra_raw = [preview.read_limited(path, preview.MAX_INPUT) for path in args.additional_scene]
        extra_scenes = [SceneSnapshot.from_json(data) for data in extra_raw]
        if (args.state_trace is None) != (args.offline_scene is None):
            raise ValueError("state trace and offline scene must be supplied together")
        if extra_scenes and args.state_trace is None:
            raise ValueError("composed previews require a state trace and offline scene")
        if args.sampler_candidate_stage is not None and (args.state_trace is None or not 0 <= args.sampler_candidate_stage < 8):
            raise ValueError("sampler candidate requires a state trace and stage 0..7")
        if args.ambient_candidate_stage is not None and (args.state_trace is None or args.ambient_candidate_stage < 0):
            raise ValueError("ambient candidate requires a state trace and a nonnegative stage")
        needs_actors = args.direct_candidate_stage is not None or args.fog_candidate_stage is not None
        if needs_actors != (args.offline_actors is not None):
            raise ValueError("direct or fog candidates require offline actors")
        for candidate_stage in (args.direct_candidate_stage, args.fog_candidate_stage):
            if candidate_stage is not None and (args.state_trace is None or candidate_stage < 0):
                raise ValueError("direct or fog candidates require a state trace and a nonnegative stage")
        if args.spot_candidates and args.direct_candidate_stage is None:
            raise ValueError("spot candidates require a direct candidate stage")
        if args.spot_projector_candidate is not None and not args.spot_candidates:
            raise ValueError("projector texture candidate requires spotlight candidates")
        states = None
        samplers = {} if args.sampler_candidate_stage is not None else None
        if args.state_trace is not None:
            selected_pass, selected_stage = select_scope(scene, args.render_pass, args.stage)
            offline_bytes = preview.read_limited(args.offline_scene, preview.MAX_INPUT)
            offline = SceneSnapshot.from_json(offline_bytes)
            states = {}
            for current, scope in [(scene, (selected_pass, selected_stage))] + [
                    (other, select_scope(other)) for other in extra_scenes]:
                current_states = legacy_states.from_trace(current, offline, offline_bytes, args.state_trace, *scope,
                                                          sampler_stage=args.sampler_candidate_stage, samplers=samplers)
                if states.keys() & current_states.keys():
                    raise ValueError("composed scenes contain duplicate draw IDs")
                states.update(current_states)
            if extra_scenes:
                legacy_states.validate_composition_interval(scene, args.state_trace, states)
        game = game_dir(args.game_dir)
        local_path = resolve_map(game, scene.source.map, ".ibt")
        kernel_path = local_path.parent / "Kernel_GFXALL.ibt"
        paths = [("bundle", local_path)]
        if "kernel" in scene.source.fingerprints:
            paths.append(("kernel", kernel_path))
        for role, path in paths:
            if scene.source.fingerprints.get(role) != digest_file(path):
                raise ValueError(f"{role} bundle fingerprint does not match SceneIR")
        ambient_values = None
        if args.ambient_candidate_stage is not None:
            matching = [(current, select_scope(current, args.render_pass, args.stage) if current is scene else select_scope(current))
                        for current in (scene, *extra_scenes)]
            matching = [(current, scope) for current, scope in matching if scope[1] == args.ambient_candidate_stage]
            if len(matching) != 1:
                raise ValueError("ambient candidate stage must identify exactly one scene")
            current, scope = matching[0]
            ambient_values = ambient.from_trace(current, args.state_trace, kernel_path, *scope)
        direct_values = None
        if args.direct_candidate_stage is not None:
            matching = [(current, select_scope(current, args.render_pass, args.stage) if current is scene else select_scope(current))
                        for current in (scene, *extra_scenes)]
            matching = [(current, scope) for current, scope in matching if scope[1] == args.direct_candidate_stage]
            if len(matching) != 1:
                raise ValueError("direct candidate stage must identify exactly one scene")
            current, scope = matching[0]
            direct_values = direct.from_trace(current, offline, offline_bytes, args.state_trace, kernel_path, args.offline_actors,
                                              *scope, args.direct_preview_gain, preview_spots=args.spot_candidates,
                                              projector_candidate=args.spot_projector_candidate)
        fog_values = None
        if args.fog_candidate_stage is not None:
            matching = [(current, select_scope(current, args.render_pass, args.stage) if current is scene else select_scope(current))
                        for current in (scene, *extra_scenes)]
            matching = [(current, scope) for current, scope in matching if scope[1] == args.fog_candidate_stage]
            if len(matching) != 1:
                raise ValueError("fog candidate stage must identify exactly one scene")
            current, scope = matching[0]
            fog_values = fog.from_trace(current, offline, offline_bytes, args.state_trace, kernel_path,
                                        args.offline_actors, *scope)
        bundles = []
        try:
            for _, path in paths:
                bundles.append(IBT(path))
            body, stats = create_packet(scene, TextureResolver(bundles), args.render_pass, args.stage, states, samplers,
                                        additional_scenes=extra_scenes, ambient_candidates=ambient_values,
                                        direct_candidates=direct_values, fog_candidates=fog_values)
        finally:
            for bundle in bundles:
                bundle.close()
        manifest = {"schema": "t3.scene.textured-packet", "version": 8 if body[:8] == FOG_MAGIC else 7 if body[:8] == PROJECTOR_MAGIC else 6 if body[:8] == SPOT_MAGIC else 5 if direct_values is not None else 4 if ambient_values is not None else 3,
                    "source_frame": scene.source.frame_id,
                    "scene_sha256": hashlib.sha256(raw).hexdigest(), "packet_sha256": hashlib.sha256(body).hexdigest(),
                    "state_source": "revalidated_same_frame_trace" if states is not None else "opaque_preview_defaults",
                    "source_fingerprints": scene.source.fingerprints, "source_limitations": list(scene.source.limitations),
                    "limitations": ["Diffuse textures are resolved from offline MatLib and fingerprinted assets, not GPU readback.",
                                    "UV0 is used without a flip; clip depth 0..1 and half-pixel correction are assumptions.",
                                    "Sampler candidates match metadata only, not texture identity or shader UV routing.",
                                    "Linear wrap sampler with mips; shader output and culling convention remain approximations.",
                                    "No stencil, verified runtime lights, legacy shaders, missing objects or HUD."], **stats}
        if extra_scenes:
            manifest["additional_scene_sha256"] = [hashlib.sha256(data).hexdigest() for data in extra_raw]
            manifest["additional_source_limitations"] = [list(other.source.limitations) for other in extra_scenes]
            manifest["limitations"].append("Partial selected draws share a depth buffer; omitted draws and stencil operations are not reconstructed.")
        if ambient_values is not None:
            manifest["limitations"].append("Ambient RGB uses sampled vertex constants with a pre-Reset shader hypothesis; the active GPU and pixel shaders are unproven.")
        if direct_values is not None:
            manifest["limitations"].append("Point-light Lambert shading and squared linear falloff are approximations; the coefficient's radius meaning is unproven.")
            manifest["limitations"].append("Light colors and energies come from offline actor data; runtime light identity, colors, intensity and shadows are unproven.")
            if any("projector_candidate" in light for light in direct_values.values()):
                manifest["limitations"].append("The projected texture is an offline metadata-compatible hypothesis, not an observed GPU texture identity.")
            elif any(light.get("mode") == "spot_approximation" for light in direct_values.values()):
                manifest["limitations"].append("Spotlight projection uses sampled matrix candidates with an approximate circular mask, not the original projected texture or a verified cone.")
            else:
                manifest["limitations"].append("Spotlight contributions are omitted.")
        if fog_values is not None:
            manifest["limitations"].append("Fog is a vertex-distance hypothesis from a pre-Reset program and sampled constants; its color is from offline LevelInfo, not a verified runtime pixel shader.")
        else:
            manifest["limitations"].append("Fog is omitted.")
        write_pair(args.output, body, manifest)
        print(json.dumps(manifest, indent=2, allow_nan=False))
    except (OSError, ValueError, TypeError, KeyError, IndexError, EOFError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
