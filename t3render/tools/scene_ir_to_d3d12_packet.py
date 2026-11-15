#!/usr/bin/env python3
"""Preview selected SceneIR geometry with the existing DX12 diagnostic.

An independent camera is a viewpoint only and never makes an offline scene a
runtime frame. A runtime source is a producer assertion, not validation by this
consumer; inspect its provenance and limitations separately.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ir"))
from scene_ir import Camera, SceneSnapshot, transform_point  # noqa: E402

MAX_INPUT = 256 * 1024 * 1024
MAX_TRIANGLES = 4096
IDENTITY = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
PALETTE = (0xff8c6b50, 0xffa29883, 0xff74685d, 0xff9e987e, 0xff887c69,
           0xffa58a6b, 0xff7f8178, 0xffb0a591)


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _invalid_number(value):
    raise ValueError(f"invalid JSON number: {value}")


def load_camera(raw):
    data = json.loads(raw, object_pairs_hook=_pairs, parse_constant=_invalid_number)
    if (not isinstance(data, dict) or set(data) != {"schema", "version", "source", "camera"} or
            data["schema"] != "t3.camera.observation" or type(data["version"]) is not int or data["version"] != 1):
        raise ValueError("invalid camera observation schema")
    source = data["source"]
    if not isinstance(source, dict):
        raise ValueError("camera source is missing")
    if source.get("kind") == "captured_draw":
        if (set(source) != {"kind", "frame_id", "operation_id", "trace_sha256"} or
                type(source["frame_id"]) is not int or source["frame_id"] < 0 or
                type(source["operation_id"]) is not int or source["operation_id"] < 1 or
                not isinstance(source["trace_sha256"], str) or
                not re.fullmatch(r"[0-9a-f]{64}", source["trace_sha256"])):
            raise ValueError("invalid captured camera provenance")
    elif source.get("kind") == "synthetic":
        if set(source) != {"kind"}:
            raise ValueError("invalid synthetic camera provenance")
    else:
        raise ValueError("unsupported camera provenance")
    camera = Camera.parse(data["camera"], "camera")
    if source["kind"] == "captured_draw" and (
            camera.source.kind != "runtime" or camera.source.frame_id != source["frame_id"] or
            camera.source.fingerprint != source["trace_sha256"]):
        raise ValueError("camera and captured provenance disagree")
    if source["kind"] == "synthetic" and camera.source.kind != "offline":
        raise ValueError("synthetic camera cannot claim runtime provenance")
    validate_camera(camera)
    return camera, source


def validate_camera(camera):
    if camera is None or camera.view is None or camera.projection is None or camera.viewport is None:
        raise ValueError("camera must provide view, projection and viewport")
    viewport = camera.viewport
    if (viewport.x or viewport.y or viewport.width > 2048 or viewport.height > 2048 or
            not 0 <= viewport.min_depth <= viewport.max_depth <= 1):
        raise ValueError("camera viewport is outside diagnostic backend bounds")
    if any(abs(v) > 10000 for v in camera.view + camera.projection):
        raise ValueError("camera matrix exceeds diagnostic backend bounds")


def create_packet(scene, camera, instance_ids, bsp_indices, excluded_materials=(), render_pass=None, stage=None):
    c = scene.conventions
    if (c.length_unit != "t3_unit" or c.axes != "t3_xyz" or c.up_axis != "z" or
            c.matrix_layout != "row_major" or c.matrix_vectors != "row" or
            c.clip_depth == "minus_one_to_one"):
        raise ValueError("unsupported SceneIR coordinate or clip convention")
    if (len(instance_ids) != len(set(instance_ids)) or len(bsp_indices) != len(set(bsp_indices)) or
            not instance_ids and not bsp_indices):
        raise ValueError("select distinct mesh instances and/or BSP indices")
    resources = {resource.id: resource for resource in scene.static_mesh_resources}
    materials = {material.id: material.name for material in scene.materials}
    instances = {instance.id: instance for instance in scene.mesh_instances}
    # Match the SDK's resource naming rules, which use lower(), not casefold().
    excluded = {name.lower() for name in excluded_materials}
    vertices, sources, skipped = [], [], []
    scoped_sections = None
    if scene.version == 1 and scene.source.kind == "runtime":
        scopes = {(s.render_pass, s.stage) for s in scene.submissions}
        if render_pass is None or stage is None:
            candidates = {(p, s) for p, s in scopes if (render_pass is None or p == render_pass) and
                          (stage is None or s == stage)}
            if len(candidates) != 1:
                raise ValueError("runtime SceneIR requires an unambiguous submission pass and stage")
            render_pass, stage = next(iter(candidates))
        if (render_pass, stage) not in scopes:
            raise ValueError("submission pass and stage not found")
        scoped_sections = {(s.target_kind, s.target_id, s.section) for s in scene.submissions
                           if (s.render_pass, s.stage) == (render_pass, stage)}
    elif render_pass is not None or stage is not None:
        raise ValueError("submission pass and stage require runtime SceneIR v1")

    def add_geometry(kind, identifier, geometry, matrix, material_ids=()):
        if (any(abs(matrix[i]) > 1e-6 for i in (3, 7, 11)) or abs(matrix[15] - 1) > 1e-6):
            raise ValueError("only affine row-vector transforms are supported")
        # A SceneIR mesh without explicit sections represents one complete
        # triangle list, not an empty mesh.
        for number, section in enumerate(geometry.sections or (None,)):
            if scoped_sections is not None and (kind, identifier, number) not in scoped_sections:
                skipped.append({"kind": kind, "id": identifier, "section": number, "reason": "not_submitted_in_selected_scope"})
                continue
            material = material_ids[number] if material_ids else section.material_id if section else None
            name = materials.get(material) if material is not None else None
            if name is not None and name.lower() in excluded:
                skipped.append({"kind": kind, "id": identifier, "section": number, "material": name})
                continue
            count = section.index_count if section else len(geometry.indices)
            first = section.first_index if section else 0
            if not count:
                continue
            if len(vertices) // 3 + count // 3 > MAX_TRIANGLES:
                raise ValueError("selected geometry exceeds diagnostic triangle limit")
            color = PALETTE[len(sources) % len(PALETTE)]
            for index in geometry.indices[first:first + count]:
                point = transform_point(geometry.positions[index], matrix)
                if len(point) != 4 or abs(point[3] - 1) > 1e-6 or any(
                        not math.isfinite(v) or abs(v) > 10000 for v in point[:3]):
                    raise ValueError("transformed vertex exceeds diagnostic backend bounds")
                vertices.append((point[:3], color))
            sources.append({"kind": kind, "id": identifier, "section": number if section else None,
                            "material": name, "triangles": count // 3})

    for identifier in instance_ids:
        if identifier not in instances:
            raise ValueError(f"mesh instance not found: {identifier}")
        instance = instances[identifier]
        if scoped_sections is not None and not any(k == "mesh_instance" and i == identifier for k, i, _ in scoped_sections):
            raise ValueError(f"mesh instance not submitted in selected scope: {identifier}")
        add_geometry("mesh_instance", identifier, resources[instance.resource_id].geometry,
                     instance.transform, instance.material_ids)
    for index in bsp_indices:
        matching = [block for block in scene.bsp_blocks if block.source_index == index]
        if len(matching) != 1:
            raise ValueError(f"BSP index is missing or ambiguous: {index}")
        if scoped_sections is not None and not any(k == "bsp_block" and i == matching[0].id for k, i, _ in scoped_sections):
            raise ValueError(f"BSP block not submitted in selected scope: {index}")
        add_geometry("bsp_block", matching[0].id, matching[0].geometry, IDENTITY)
    if not vertices:
        raise ValueError("selected geometry contains no triangles")
    viewport = camera.viewport
    lines = ["T3R_RECONSTRUCTION_XYZ_V1", f"{viewport.width} {viewport.height}",
             f"{viewport.min_depth:.9g} {viewport.max_depth:.9g}", str(0xff16181c),
             " ".join(map(str, IDENTITY)), " ".join(f"{x:.9g}" for x in camera.view),
             " ".join(f"{x:.9g}" for x in camera.projection), str(len(vertices) // 3)]
    lines.extend(" ".join(f"{x:.9g}" for x in point) + f" {color}" for point, color in vertices)
    packet = "\n".join(lines) + "\n"
    if len(packet.encode("ascii")) > 1024 * 1024:
        raise ValueError("packet exceeds diagnostic backend input limit")
    return packet, {"triangles": len(vertices) // 3, "sources": sources, "excluded_sections": skipped,
                    "submission_scope": {"render_pass": render_pass, "stage": stage} if scoped_sections is not None else None}


def read_limited(path, limit):
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"input exceeds {limit} bytes: {path}")
    return data


def write_pair(packet_path, packet, manifest):
    sidecar = Path(str(packet_path) + ".manifest.json")
    created = []
    try:
        for path, body in ((packet_path, packet),
                           (sidecar, json.dumps(manifest, indent=2, allow_nan=False) + "\n")):
            with path.open("x", encoding="utf-8", newline="\n") as stream:
                identity = os.fstat(stream.fileno())
                created.append((path, identity.st_dev, identity.st_ino))
                stream.write(body)
    except Exception:
        for path, device, inode in created:
            try:
                current = path.lstat()
                if (current.st_dev, current.st_ino) == (device, inode):
                    path.unlink()
            except OSError:
                pass
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene", type=Path)
    parser.add_argument("--camera", type=Path, help="separate camera observation; otherwise use the SceneIR camera")
    parser.add_argument("--instance", action="append", default=[])
    parser.add_argument("--bsp", action="append", default=[], type=int)
    parser.add_argument("--exclude-material", action="append", default=[])
    parser.add_argument("--submission-pass", type=int)
    parser.add_argument("--submission-stage", type=int)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if any(index < 0 for index in args.bsp):
            raise ValueError("BSP indices must be nonnegative")
        scene_bytes = read_limited(args.scene, MAX_INPUT)
        scene = SceneSnapshot.from_json(scene_bytes)
        if args.camera:
            camera_bytes = read_limited(args.camera, 1024 * 1024)
            camera, camera_source = load_camera(camera_bytes)
            camera_digest = hashlib.sha256(camera_bytes).hexdigest()
        else:
            camera = scene.camera
            validate_camera(camera)
            camera_source = {"kind": "scene_ir_camera", "origin": camera.source.kind,
                             "frame_id": camera.source.frame_id, "fingerprint": camera.source.fingerprint}
            camera_digest = None
        packet, stats = create_packet(scene, camera, args.instance, args.bsp, args.exclude_material,
                                     args.submission_pass, args.submission_stage)
        runtime = scene.source.kind == "runtime"
        manifest = {"kind": "runtime_scene_ir_preview_producer_assertion" if runtime else "offline_scene_ir_preview_not_runtime_frame",
                    "map": scene.source.map, "scene_source": scene.source.kind,
                    "scene_frame_id": scene.source.frame_id, "scene_limitations": list(scene.source.limitations),
                    "scene_sha256": hashlib.sha256(scene_bytes).hexdigest(),
                    "camera_observation_sha256": camera_digest,
                    "camera_source": camera_source,
                    "camera_synchronized_to_scene": bool(runtime and args.camera is None and scene.source.camera_synchronized is True),
                    "clip_depth_assumption": "zero_to_one",
                    "packet_sha256": hashlib.sha256(packet.encode("ascii")).hexdigest(), **stats,
                    "limitations": ["Scene provenance and synchronization are producer assertions, not verified by this consumer.",
                        "A separate camera does not establish synchronization with scene objects.",
                        "Clip depth 0..1 is assumed even when the scene convention is unknown.",
                        "Flat colors, approximate CCW culling and new D32 LESS depth are diagnostic only.",
                        "Sections are grouped by selection, not replayed in original submission order.",
                        "Textures, lighting, original depth history and unselected objects are absent."]}
        write_pair(args.output, packet, manifest)
        print(json.dumps(manifest, indent=2, allow_nan=False))
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
