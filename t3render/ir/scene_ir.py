"""Version 0 semantic scene interchange, independent of graphics APIs.

This module owns only plain values. Importers must explicitly state the origin
and conventions of their data; unknown engine facts are not inferred here.
"""
from __future__ import annotations

from dataclasses import dataclass, fields, is_dataclass
import json
import math
import re
from typing import Optional


def _object(value, keys, where):
    if not isinstance(value, dict) or set(value) != set(keys):
        raise ValueError(f"{where}: expected exactly {', '.join(keys)}")
    return value


def _string(value, where):
    if not isinstance(value, str) or not value:
        raise ValueError(f"{where}: expected a nonempty string")
    return value


def _id(value, where):
    value = _string(value, where)
    # Identifiers are names/keys, never a serialized process address.
    if re.fullmatch(r"0[xX][0-9a-fA-F]+", value):
        raise ValueError(f"{where}: process addresses are not scene identifiers")
    return value


def _number(value, where):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{where}: expected a number")
    try:
        if not math.isfinite(value):
            raise ValueError(f"{where}: expected a finite number")
    except OverflowError as exc:
        raise ValueError(f"{where}: number is too large") from exc
    return value


def _integer(value, where):
    if type(value) is not int or value < 0:
        raise ValueError(f"{where}: expected a nonnegative integer")
    return value


def _optional(value, parser, where):
    return None if value is None else parser(value, where)


def _vector(value, size, where):
    if not isinstance(value, (list, tuple)) or len(value) != size:
        raise ValueError(f"{where}: expected {size} values")
    return tuple(_number(n, where) for n in value)


def _list(value, parser, where):
    if not isinstance(value, (list, tuple)):
        raise ValueError(f"{where}: expected an array")
    return tuple(parser(item, f"{where}[{i}]") for i, item in enumerate(value))


def _enum(value, choices, where):
    if value not in choices:
        raise ValueError(f"{where}: invalid value")
    return value


def _plain(value):
    if is_dataclass(value):
        return {f.name: _plain(getattr(value, f.name)) for f in fields(value)}
    if isinstance(value, (tuple, list)):
        return [_plain(item) for item in value]
    if isinstance(value, dict):
        return {key: _plain(item) for key, item in value.items()}
    return value


@dataclass(frozen=True)
class Source:
    kind: str
    map: str
    frame_id: Optional[int]
    camera_synchronized: Optional[bool]
    fingerprints: dict
    limitations: tuple

    @classmethod
    def parse(cls, obj, where):
        _object(obj, ("kind", "map", "frame_id", "camera_synchronized", "fingerprints", "limitations"), where)
        kind = _enum(obj["kind"], ("offline", "runtime"), where + ".kind")
        frame = _optional(obj["frame_id"], _integer, where + ".frame_id")
        sync = obj["camera_synchronized"]
        if sync is not None and type(sync) is not bool:
            raise ValueError(f"{where}.camera_synchronized: expected boolean or null")
        if kind == "offline" and (frame is not None or sync is True):
            raise ValueError(f"{where}: offline scene is not a synchronized runtime frame")
        if kind == "runtime" and frame is None:
            raise ValueError(f"{where}: runtime scene requires frame_id")
        fingerprints = obj["fingerprints"]
        if not isinstance(fingerprints, dict):
            raise ValueError(f"{where}.fingerprints: expected an object")
        checked = {}
        for role, digest in fingerprints.items():
            if not isinstance(role, str) or not re.fullmatch(r"[a-z][a-z0-9_]*", role):
                raise ValueError(f"{where}.fingerprints: invalid source role")
            if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise ValueError(f"{where}.fingerprints: expected lowercase SHA-256")
            checked[role] = digest
        limitations = _list(obj["limitations"], _string, where + ".limitations")
        return cls(kind, _string(obj["map"], where + ".map"), frame, sync, checked, limitations)


@dataclass(frozen=True)
class Conventions:
    length_unit: str
    meters_per_unit: Optional[float]
    axes: str
    up_axis: str
    handedness: str
    winding: str
    uv_origin: str
    tangent_space: str
    matrix_layout: str
    matrix_vectors: str
    clip_depth: str

    @classmethod
    def parse(cls, obj, where):
        names = tuple(f.name for f in fields(cls))
        _object(obj, names, where)
        choices = {
            "length_unit": ("unknown", "t3_unit", "meter"),
            "axes": ("unknown", "t3_xyz"), "up_axis": ("unknown", "z"),
            "handedness": ("unknown", "left", "right"),
            "winding": ("unknown", "normal_aligned", "normal_opposed"),
            "uv_origin": ("unknown", "top_left", "bottom_left"),
            "tangent_space": ("unknown",), "matrix_layout": ("row_major",),
            "matrix_vectors": ("row",),
            "clip_depth": ("unknown", "zero_to_one", "minus_one_to_one"),
        }
        result = {name: _enum(obj[name], permitted, where + "." + name)
                  for name, permitted in choices.items()}
        scale = _optional(obj["meters_per_unit"], _number, where + ".meters_per_unit")
        if scale is not None and (scale <= 0 or obj["length_unit"] == "unknown"):
            raise ValueError(f"{where}.meters_per_unit: scale requires a known unit and must be positive")
        if obj["length_unit"] == "meter" and scale not in (None, 1):
            raise ValueError(f"{where}: one meter must equal one meter")
        result["meters_per_unit"] = scale
        return cls(**result)


@dataclass(frozen=True)
class Viewport:
    x: int
    y: int
    width: int
    height: int
    min_depth: float
    max_depth: float

    @classmethod
    def parse(cls, obj, where):
        _object(obj, ("x", "y", "width", "height", "min_depth", "max_depth"), where)
        xywh = [_integer(obj[k], where + "." + k) for k in ("x", "y", "width", "height")]
        if not xywh[2] or not xywh[3]:
            raise ValueError(f"{where}: viewport dimensions must be positive")
        depths = [_number(obj[k], where + "." + k) for k in ("min_depth", "max_depth")]
        if depths[0] > depths[1]:
            raise ValueError(f"{where}: reversed depth range")
        return cls(*xywh, *depths)


@dataclass(frozen=True)
class CameraSource:
    kind: str
    frame_id: Optional[int]
    fingerprint: Optional[str]

    @classmethod
    def parse(cls, obj, where):
        _object(obj, ("kind", "frame_id", "fingerprint"), where)
        kind = _enum(obj["kind"], ("offline", "runtime"), where + ".kind")
        frame = _optional(obj["frame_id"], _integer, where + ".frame_id")
        if (kind == "runtime") != (frame is not None):
            raise ValueError(f"{where}: runtime camera requires a frame, offline camera cannot have one")
        fingerprint = obj["fingerprint"]
        if fingerprint is not None and (not isinstance(fingerprint, str) or
                                        not re.fullmatch(r"[0-9a-f]{64}", fingerprint)):
            raise ValueError(f"{where}.fingerprint: expected lowercase SHA-256 or null")
        return cls(kind, frame, fingerprint)


@dataclass(frozen=True)
class Camera:
    source: CameraSource
    view: Optional[tuple]
    projection: Optional[tuple]
    viewport: Optional[Viewport]
    near: Optional[float]
    far: Optional[float]

    @classmethod
    def parse(cls, obj, where):
        _object(obj, ("source", "view", "projection", "viewport", "near", "far"), where)
        source = CameraSource.parse(obj["source"], where + ".source")
        view = _optional(obj["view"], lambda x, p: _vector(x, 16, p), where + ".view")
        proj = _optional(obj["projection"], lambda x, p: _vector(x, 16, p), where + ".projection")
        viewport = _optional(obj["viewport"], Viewport.parse, where + ".viewport")
        near = _optional(obj["near"], _number, where + ".near")
        far = _optional(obj["far"], _number, where + ".far")
        if near is not None and far is not None and near >= far:
            raise ValueError(f"{where}: near must be less than far")
        return cls(source, view, proj, viewport, near, far)


@dataclass(frozen=True)
class Section:
    name: str
    first_index: int
    index_count: int
    material_id: Optional[str]

    @classmethod
    def parse(cls, obj, where):
        _object(obj, ("name", "first_index", "index_count", "material_id"), where)
        return cls(_string(obj["name"], where + ".name"),
                   _integer(obj["first_index"], where + ".first_index"),
                   _integer(obj["index_count"], where + ".index_count"),
                   _optional(obj["material_id"], _id, where + ".material_id"))


@dataclass(frozen=True)
class Geometry:
    positions: tuple
    normals: Optional[tuple]
    uv0: Optional[tuple]
    tangents: Optional[tuple]
    indices: tuple
    sections: tuple

    @classmethod
    def parse(cls, obj, where):
        _object(obj, ("positions", "normals", "uv0", "tangents", "indices", "sections"), where)
        positions = _list(obj["positions"], lambda x, p: _vector(x, 3, p), where + ".positions")
        attributes = []
        for name, width in (("normals", 3), ("uv0", 2), ("tangents", 3)):
            values = _optional(obj[name], lambda x, p: _list(x, lambda v, q: _vector(v, width, q), p), where + "." + name)
            if values is not None and len(values) != len(positions):
                raise ValueError(f"{where}.{name}: count does not match positions")
            attributes.append(values)
        indices = _list(obj["indices"], _integer, where + ".indices")
        sections = _list(obj["sections"], Section.parse, where + ".sections")
        for section in sections:
            if section.index_count % 3 or section.first_index + section.index_count > len(indices):
                raise ValueError(f"{where}.sections: expected in-range triangle sections")
            if any(index >= len(positions) for index in indices[section.first_index:section.first_index + section.index_count]):
                raise ValueError(f"{where}.sections: vertex index out of range")
        if not sections and (len(indices) % 3 or any(index >= len(positions) for index in indices)):
            raise ValueError(f"{where}.indices: expected in-range triangles")
        return cls(positions, *attributes, indices, sections)


@dataclass(frozen=True)
class StaticMeshResource:
    id: str
    name: str
    geometry: Geometry

    @classmethod
    def parse(cls, obj, where):
        _object(obj, ("id", "name", "geometry"), where)
        return cls(_id(obj["id"], where + ".id"), _string(obj["name"], where + ".name"),
                   Geometry.parse(obj["geometry"], where + ".geometry"))


@dataclass(frozen=True)
class MeshInstance:
    id: str
    resource_id: str
    transform: tuple
    skin: Optional[str]
    material_ids: tuple

    @classmethod
    def parse(cls, obj, where):
        _object(obj, ("id", "resource_id", "transform", "skin", "material_ids"), where)
        return cls(_id(obj["id"], where + ".id"), _id(obj["resource_id"], where + ".resource_id"),
                   _vector(obj["transform"], 16, where + ".transform"),
                   _optional(obj["skin"], _string, where + ".skin"),
                   _list(obj["material_ids"], lambda x, p: _optional(x, _id, p), where + ".material_ids"))


@dataclass(frozen=True)
class BspBlock:
    id: str
    block_id: int
    source_index: int
    source_offset: Optional[int]
    geometry: Geometry

    @classmethod
    def parse(cls, obj, where):
        _object(obj, ("id", "block_id", "source_index", "source_offset", "geometry"), where)
        return cls(_id(obj["id"], where + ".id"), _integer(obj["block_id"], where + ".block_id"),
                   _integer(obj["source_index"], where + ".source_index"),
                   _optional(obj["source_offset"], _integer, where + ".source_offset"),
                   Geometry.parse(obj["geometry"], where + ".geometry"))


@dataclass(frozen=True)
class MaterialRef:
    id: str
    name: str

    @classmethod
    def parse(cls, obj, where):
        _object(obj, ("id", "name"), where)
        return cls(_id(obj["id"], where + ".id"), _string(obj["name"], where + ".name"))


@dataclass(frozen=True)
class VisualLight:
    id: str
    kind: Optional[str]
    position: Optional[tuple]
    color: Optional[tuple]
    radius: Optional[float]
    intensity: Optional[float]
    enabled: Optional[bool]

    @classmethod
    def parse(cls, obj, where):
        _object(obj, ("id", "kind", "position", "color", "radius", "intensity", "enabled"), where)
        if obj["enabled"] is not None and type(obj["enabled"]) is not bool:
            raise ValueError(f"{where}.enabled: expected boolean or null")
        return cls(_id(obj["id"], where + ".id"), _optional(obj["kind"], _string, where + ".kind"),
                   _optional(obj["position"], lambda x, p: _vector(x, 3, p), where + ".position"),
                   _optional(obj["color"], lambda x, p: _vector(x, 3, p), where + ".color"),
                   _optional(obj["radius"], _number, where + ".radius"),
                   _optional(obj["intensity"], _number, where + ".intensity"), obj["enabled"])


@dataclass(frozen=True)
class SceneSnapshot:
    schema: str
    version: int
    source: Source
    conventions: Conventions
    camera: Optional[Camera]
    static_mesh_resources: tuple
    mesh_instances: tuple
    bsp_blocks: tuple
    materials: tuple
    lights: tuple

    @classmethod
    def from_dict(cls, obj):
        names = tuple(f.name for f in fields(cls))
        _object(obj, names, "scene")
        if obj["schema"] != "t3.scene" or type(obj["version"]) is not int or obj["version"] != 0:
            raise ValueError("scene: unsupported schema or version")
        source = Source.parse(obj["source"], "source")
        camera = _optional(obj["camera"], Camera.parse, "camera")
        if source.camera_synchronized is True and (camera is None or camera.source.kind != "runtime" or
                                                    camera.source.frame_id != source.frame_id):
            raise ValueError("scene: synchronized camera must belong to the same runtime frame")
        if source.camera_synchronized is True:
            trace = source.fingerprints.get("trace")
            if trace is None or camera.source.fingerprint is None or trace != camera.source.fingerprint:
                raise ValueError("scene: synchronized camera requires matching trace fingerprints")
        meshes = _list(obj["static_mesh_resources"], StaticMeshResource.parse, "static_mesh_resources")
        instances = _list(obj["mesh_instances"], MeshInstance.parse, "mesh_instances")
        blocks = _list(obj["bsp_blocks"], BspBlock.parse, "bsp_blocks")
        materials = _list(obj["materials"], MaterialRef.parse, "materials")
        lights = _list(obj["lights"], VisualLight.parse, "lights")
        for group in (meshes, instances, blocks, materials, lights):
            if len({item.id for item in group}) != len(group):
                raise ValueError("scene: duplicate identifiers")
        mesh_by_id = {mesh.id: mesh for mesh in meshes}
        material_ids = {material.id for material in materials}
        for geometry in [mesh.geometry for mesh in meshes] + [block.geometry for block in blocks]:
            if any(section.material_id is not None and section.material_id not in material_ids for section in geometry.sections):
                raise ValueError("scene: section references missing material")
        for instance in instances:
            mesh = mesh_by_id.get(instance.resource_id)
            if mesh is None:
                raise ValueError("scene: instance references missing mesh")
            if instance.material_ids and len(instance.material_ids) != len(mesh.geometry.sections):
                raise ValueError("scene: instance material count must match sections")
            if any(mat is not None and mat not in material_ids for mat in instance.material_ids):
                raise ValueError("scene: instance references missing material")
        return cls(obj["schema"], obj["version"], source, Conventions.parse(obj["conventions"], "conventions"),
                   camera, meshes, instances, blocks, materials, lights)

    @classmethod
    def from_json(cls, value):
        def no_duplicates(pairs):
            out = {}
            for key, item in pairs:
                if key in out:
                    raise ValueError(f"duplicate JSON key: {key}")
                out[key] = item
            return out
        def no_constant(value):
            raise ValueError(f"invalid JSON number: {value}")
        return cls.from_dict(json.loads(value, object_pairs_hook=no_duplicates, parse_constant=no_constant))

    def to_dict(self):
        data = _plain(self)
        SceneSnapshot.from_dict(data)
        return data

    def to_json(self, **kwargs):
        data = self.to_dict()
        return json.dumps(data, allow_nan=False, **kwargs)


def transform_point(point, matrix):
    """Transform a point with a row vector and a row-major 4x4 matrix."""
    x, y, z = _vector(point, 3, "point")
    m = _vector(matrix, 16, "matrix")
    return tuple(sum(v * m[row * 4 + col] for row, v in enumerate((x, y, z, 1))) for col in range(4))


def distance_in_meters(value, conventions):
    """Convert only when a source explicitly supplies a unit scale."""
    value = _number(value, "distance")
    if not isinstance(conventions, Conventions) or conventions.meters_per_unit is None:
        raise ValueError("unit scale is unknown")
    return value * conventions.meters_per_unit


def t3_to_y_up(point):
    """Swap Y/Z without scaling; callers must explicitly reverse triangle order."""
    x, y, z = _vector(point, 3, "point")
    return (x, z, y)


def reverse_triangles(indices):
    values = _list(indices, _integer, "indices")
    if len(values) % 3:
        raise ValueError("indices: expected triangles")
    return tuple(index for i in range(0, len(values), 3) for index in (values[i], values[i + 2], values[i + 1]))
