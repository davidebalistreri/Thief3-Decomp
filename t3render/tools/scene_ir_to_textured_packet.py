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

MAGIC = b"T3SCN02\0"
MAX_BYTES = 64 * 1024 * 1024
MAX_TEXTURES = 64
MAX_DRAWS = 8192
MAX_TRIANGLES = 100000


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
        texture_key = (str(texture_bundle.path), texture_resource.name.lower())
        if texture_key in self.textures:
            self.cache[key] = self.textures[texture_key]
            return self.cache[key]
        texture = parse_texture(texture_resource, texture_bundle.split(texture_resource))
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
        self.cache[key] = value
        return value


def create_packet(scene, resolver, render_pass=None, stage=None, draw_states=None):
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
    render_pass, stage = next(iter(scopes))
    submissions = [s for s in scene.submissions if (s.render_pass, s.stage) == (render_pass, stage)]
    if not 1 <= len(submissions) <= MAX_DRAWS:
        raise ValueError("draw count exceeds renderer bounds")
    meshes = {x.id: x for x in scene.static_mesh_resources}
    instances = {x.id: x for x in scene.mesh_instances}
    blocks = {x.id: x for x in scene.bsp_blocks}
    materials = {x.id: x.name for x in scene.materials}
    textures, texture_indices, draws, source_draws = [], {}, [], []
    triangle_count = 0
    for submission in submissions:
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
            vertices.append((*position[:3], *uv))
        triangle_count += len(vertices) // 3
        if not vertices or triangle_count > MAX_TRIANGLES:
            raise ValueError("triangle count exceeds renderer bounds")
        if draw_states is not None and submission.draw_id not in draw_states:
            raise ValueError("captured state missing for submitted draw")
        state = legacy_states.validate(dict(legacy_states.DEFAULT if draw_states is None else draw_states[submission.draw_id]))
        draws.append((texture_indices[tex_key], vertices, state))
        source_draws.append({"draw": submission.draw_id, "target_kind": submission.target_kind,
                             "target_id": submission.target_id, "section": submission.section,
                             "material": material_name, "texture": tex["name"], "triangles": len(vertices) // 3,
                             "state": state})
    camera = scene.camera
    viewport = camera.viewport
    size = (struct.calcsize("<8sIIff32fII") + sum(12 + sum(len(mip) for mip in tex["mips"]) for tex in textures) +
            sum(52 + len(vertices) * 20 for _, vertices, _ in draws))
    if size > MAX_BYTES:
        raise ValueError("packet exceeds renderer bounds")
    body = bytearray(struct.pack("<8sIIff32fII", MAGIC, viewport.width, viewport.height,
                                 viewport.min_depth, viewport.max_depth, *camera.view, *camera.projection,
                                 len(textures), len(draws)))
    for tex in textures:
        body.extend(struct.pack("<III", tex["width"], tex["height"], len(tex["mips"])))
        for mip in tex["mips"]:
            body.extend(mip)
    for texture_index, vertices, state in draws:
        body.extend(struct.pack("<13I", texture_index, len(vertices), *(state[key] for key in legacy_states.FIELDS)))
        for vertex in vertices:
            body.extend(struct.pack("<5f", *vertex))
    if len(body) != size:
        raise ValueError("packet size mismatch")
    stats = {"render_pass": render_pass, "stage": stage, "triangles": triangle_count, "draws": source_draws,
             "textures": [{key: tex[key] for key in ("name", "bundle", "format", "width", "height")}
                          | {"mips": len(tex["mips"])} for tex in textures]}
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
    parser.add_argument("--state-trace", type=Path, help="optional synchronized legacy state observation")
    parser.add_argument("--offline-scene", type=Path, help="original fixture used to revalidate scoped state")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        raw = preview.read_limited(args.scene, preview.MAX_INPUT)
        scene = SceneSnapshot.from_json(raw)
        if (args.state_trace is None) != (args.offline_scene is None):
            raise ValueError("state trace and offline scene must be supplied together")
        states = None
        if args.state_trace is not None:
            scopes = {(s.render_pass, s.stage) for s in scene.submissions if
                      (args.render_pass is None or s.render_pass == args.render_pass) and
                      (args.stage is None or s.stage == args.stage)}
            if len(scopes) != 1:
                raise ValueError("select an unambiguous submission pass and stage")
            selected_pass, selected_stage = next(iter(scopes))
            offline_bytes = preview.read_limited(args.offline_scene, preview.MAX_INPUT)
            states = legacy_states.from_trace(scene, SceneSnapshot.from_json(offline_bytes), offline_bytes,
                                               args.state_trace, selected_pass, selected_stage)
        game = game_dir(args.game_dir)
        local_path = resolve_map(game, scene.source.map, ".ibt")
        kernel_path = local_path.parent / "Kernel_GFXALL.ibt"
        paths = [("bundle", local_path)]
        if "kernel" in scene.source.fingerprints:
            paths.append(("kernel", kernel_path))
        for role, path in paths:
            if scene.source.fingerprints.get(role) != digest_file(path):
                raise ValueError(f"{role} bundle fingerprint does not match SceneIR")
        bundles = []
        try:
            for _, path in paths:
                bundles.append(IBT(path))
            body, stats = create_packet(scene, TextureResolver(bundles), args.render_pass, args.stage, states)
        finally:
            for bundle in bundles:
                bundle.close()
        manifest = {"schema": "t3.scene.textured-packet", "version": 2, "source_frame": scene.source.frame_id,
                    "scene_sha256": hashlib.sha256(raw).hexdigest(), "packet_sha256": hashlib.sha256(body).hexdigest(),
                    "state_source": "revalidated_same_frame_trace" if states is not None else "opaque_preview_defaults",
                    "source_fingerprints": scene.source.fingerprints, "source_limitations": list(scene.source.limitations),
                    "limitations": ["Diffuse textures are resolved from offline MatLib and fingerprinted assets, not GPU readback.",
                                    "UV0 is used without a flip; clip depth 0..1 and half-pixel correction are assumptions.",
                                    "Linear wrap sampler with mips; shader output and culling convention remain approximations.",
                                    "No fog, runtime lights, legacy shaders, missing objects or HUD."], **stats}
        write_pair(args.output, body, manifest)
        print(json.dumps(manifest, indent=2, allow_nan=False))
    except (OSError, ValueError, TypeError, KeyError, IndexError, EOFError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
