"""Opt-in bedroom material selection and local Godot texture conversion.

The selection mirrors the Godot preview's bed-relative BSP triangle rules. It
does not mutate SceneIR or imply that a runtime material was observed.
"""
import hashlib
import math
from pathlib import Path
import subprocess


MAP = "SouthQuarter1_int1"
ACTOR = "StaticMeshActor__1520"
MESH = "innbed1a1"
SCALE = 0.01905
MATERIALS = {"stmwall512d_d": "stone_wall_v2", "hsefloorb_d": "wood_floor_v2"}
AREAS = {"stone_wall_v2": ((-.25, -.10, -2.10), (4.85, 4.10, 3.35)),
         "wood_floor_v2": ((-6.0, -.08, -1.6), (5.0, .08, 3.2))}
SOURCE_SIZES = {"stone_wall_v2": 2048, "wood_floor_v2": 1024, "bed_wool_v2": 512, "pillow_linen": 512,
                "candlestick_metal_v2": 1024}
MAX_PNG_BYTES = 16 * 1024 * 1024
TEXTURE_SIZE = 1024


def _inverse_linear(matrix):
    a, b, c, d, e, f, g, h, i = (matrix[k] for k in (0, 1, 2, 4, 5, 6, 8, 9, 10))
    cof = (e*i-f*h, f*g-d*i, d*h-e*g,
           c*h-b*i, a*i-c*g, b*g-a*h,
           b*f-c*e, c*d-a*f, a*e-b*d)
    det = a*cof[0] + b*cof[1] + c*cof[2]
    if not math.isfinite(det) or abs(det) < 1e-12:
        raise ValueError("bedroom anchor has a singular transform")
    return tuple(cof[j*3+i] / det for i in range(3) for j in range(3))


class BedroomSelection:
    def __init__(self, scene):
        c, source = scene.conventions, scene.source
        if (source.kind != "runtime" or source.map != MAP or source.camera_synchronized is not True or
                c.length_unit != "t3_unit" or c.axes != "t3_xyz" or c.up_axis != "z" or
                c.matrix_layout != "row_major" or c.matrix_vectors != "row" or
                c.meters_per_unit is None or not math.isclose(c.meters_per_unit, SCALE, rel_tol=0, abs_tol=1e-9)):
            raise ValueError("bedroom overrides require the synchronized T3 scene and SDK scale")
        actor_id = f"actor:{MAP}:{source.fingerprints.get('map')}:{ACTOR}"
        anchors = [i for i in scene.mesh_instances if i.id == actor_id]
        meshes = {m.id: m for m in scene.static_mesh_resources}
        if (len(anchors) != 1 or anchors[0].resource_id not in meshes or
                meshes[anchors[0].resource_id].name.lower() != MESH):
            raise ValueError("bedroom overrides require the unique semantic bed anchor")
        matrix = anchors[0].transform
        if any(abs(matrix[i]) > 1e-6 for i in (3, 7, 11)) or abs(matrix[15] - 1) > 1e-6:
            raise ValueError("bedroom anchor must have an affine row-vector transform")
        self.anchor_id = actor_id
        self.anchor_transform = matrix
        self.frame_id = source.frame_id
        self.origin = matrix[12:15]
        self.inverse = _inverse_linear(matrix)

    def validate_scene(self, scene):
        current = BedroomSelection(scene)
        if (current.anchor_id != self.anchor_id or current.anchor_transform != self.anchor_transform or
                current.frame_id != self.frame_id):
            raise ValueError("bedroom override anchor does not belong to this frame")

    def _local(self, point):
        p = [point[i] - self.origin[i] for i in range(3)]
        t3 = [sum(p[i] * self.inverse[i*3+j] for i in range(3)) for j in range(3)]
        return (t3[0] * SCALE, t3[2] * SCALE, t3[1] * SCALE)

    def partition(self, target_kind, material, vertices):
        """Return contiguous (first vertex, end vertex, replacement) runs."""
        replacement = MATERIALS.get(material.lower()) if target_kind == "bsp_block" else None
        if replacement is None:
            return [(0, len(vertices), None)]
        lo, hi = AREAS[replacement]
        result = []
        for start in range(0, len(vertices), 3):
            # The SDK swaps Y/Z; Godot's GLTF importer converts back to its
            # clockwise front-face convention after the export's reversal.
            a, b, c = (self._local(v[:3]) for v in vertices[start:start+3])
            ab = [b[i]-a[i] for i in range(3)]
            ac = [c[i]-a[i] for i in range(3)]
            face = (ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0])
            length = math.sqrt(sum(v*v for v in face))
            center = [(a[i]+b[i]+c[i])/3 for i in range(3)]
            inside = all(lo[i] <= center[i] < hi[i] for i in range(3))
            facing = (abs(face[1]) < length * .1 if replacement == "stone_wall_v2"
                      else face[1] < -length * .99)
            label = replacement if length > 1e-12 and inside and facing else None
            if result and result[-1][2] == label:
                result[-1] = (result[-1][0], start + 3, label)
            else:
                result.append((start, start + 3, label))
        return result


class GodotTextures:
    """Load generated local PNGs, retaining hashes and generating bounded mips."""
    def __init__(self, materials_dir, texture_size=TEXTURE_SIZE, linear_albedo_mips=False):
        self.materials_dir = Path(materials_dir)
        if type(texture_size) is not int or not 1 <= texture_size <= TEXTURE_SIZE or texture_size & (texture_size - 1):
            raise ValueError("override texture size must be a power of two up to 1024")
        self.texture_size = texture_size
        if type(linear_albedo_mips) is not bool:
            raise ValueError("linear albedo mip mode must be boolean")
        self.linear_albedo_mips = linear_albedo_mips
        self.cache = {}

    def resolve(self, name, kind):
        if name not in SOURCE_SIZES or kind not in ("albedo", "normal", "roughness", "ao"):
            raise ValueError("unknown bedroom override texture")
        key = (name, kind)
        if key in self.cache:
            return self.cache[key]
        path = self.materials_dir / name / f"{name}_{kind}.png"
        with path.open("rb") as stream:
            raw = stream.read(MAX_PNG_BYTES + 1)
        if len(raw) > MAX_PNG_BYTES or not raw.startswith(b"\x89PNG\r\n\x1a\n"):
            raise ValueError(f"invalid or oversized override PNG: {path}")
        try:
            info = subprocess.run(["magick", "identify", "-format", "%m %w %h\\n", "png:-"], input=raw,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=30).stdout
            expected = SOURCE_SIZES[name]
            if info != f"PNG {expected} {expected}\n".encode():
                raise ValueError(f"unexpected override PNG dimensions or frames: {path}")
            linear = kind == "albedo" and self.linear_albedo_mips
            args = ["magick", "png:-", "-alpha", "on"]
            if linear:
                args.extend(["-colorspace", "RGB"])
            args.extend(["-filter", "Triangle", "-resize", f"{self.texture_size}x{self.texture_size}!"])
            for level in range(self.texture_size.bit_length()):
                if level:
                    size = self.texture_size >> level
                    args.extend(["-resize", f"{size}x{size}!"])
                if linear:
                    args.extend(["(", "+clone", "-colorspace", "sRGB", "-depth", "8", "-write", "rgba:-", "+delete", ")"])
                else:
                    args.extend(["-depth", "8", "-write", "rgba:-"])
            args.append("null:")
            pixels = subprocess.run(args, input=raw, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    check=True, timeout=60).stdout
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
            raise ValueError(f"cannot decode override PNG: {path}") from exc
        sizes = [(self.texture_size >> level)**2 * 4 for level in range(self.texture_size.bit_length())]
        if len(pixels) != sum(sizes):
            raise ValueError(f"invalid override mip data: {path}")
        mips, offset = [], 0
        for size in sizes:
            mips.append(pixels[offset:offset+size])
            offset += size
        value = {"name": f"{name}_{kind}", "bundle": "local_godot_remaster", "format": "RGBA8",
                 "width": self.texture_size, "height": self.texture_size, "mips": mips,
                 "usage": 1 if kind == "normal" else 0, "source_sha256": hashlib.sha256(raw).hexdigest()}
        self.cache[key] = value
        return value


class BedroomOverrides(BedroomSelection):
    def __init__(self, scene, materials_dir, texture_size=TEXTURE_SIZE, linear_albedo_mips=False):
        super().__init__(scene)
        self.textures = GodotTextures(materials_dir, texture_size, linear_albedo_mips)

    def resolve(self, name, kind):
        return self.textures.resolve(name, kind)
