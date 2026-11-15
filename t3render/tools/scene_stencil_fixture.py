#!/usr/bin/env python3
"""GPU fixture for a two-sided z-fail shadow volume and a lit receiver."""
import argparse
import os
from pathlib import Path
import struct
import sys

from scene_legacy_states import DEFAULT, FIELDS
from scene_texture_fixture import IDENTITY


def cube_volume():
    """Closed cube faces plus degenerate silhouette-edge quads."""
    p = [(-.35, -.35, 2), (.35, -.35, 2), (.35, .35, 2), (-.35, .35, 2),
         (-.35, -.35, 2.5), (.35, -.35, 2.5), (.35, .35, 2.5), (-.35, .35, 2.5)]
    faces = [((0, 3, 2, 1), (0, 0, -1)), ((4, 5, 6, 7), (0, 0, 1)),
             ((0, 4, 7, 3), (-1, 0, 0)), ((1, 2, 6, 5), (1, 0, 0)),
             ((0, 1, 5, 4), (0, -1, 0)), ((3, 7, 6, 2), (0, 1, 0))]
    vertices, indices, edges = [], [], {}
    for face, normal in faces:
        for tri in ((face[0], face[1], face[2]), (face[0], face[2], face[3])):
            ids = [len(vertices) + i for i in range(3)]
            vertices.extend((p[v], normal) for v in tri)
            indices.extend(ids)
            for i in range(3):
                u, v = tri[i], tri[(i+1) % 3]
                edges.setdefault(tuple(sorted((u, v))), []).append((u, v, ids[i], ids[(i+1) % 3]))
    for pair in edges.values():
        if len(pair) != 2 or pair[0][:2] != tuple(reversed(pair[1][:2])):
            raise ValueError("cube has inconsistent face winding")
        _, _, a, b = pair[0]
        _, _, b2, a2 = pair[1]
        indices.extend((b, a, a2, b, a2, b2))
    return [vertices[i] for i in indices]


def fixture(shadow=True, reset=False):
    if reset and not shadow:
        raise ValueError("stencil reset fixture requires a shadow batch")
    # D3D row-vector perspective: near .1, far 10.
    projection = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 10/9.9, 1, 0, 0, -1/9.9, 0]
    light = (-.5, 0, 1, .01, 1, 1, 1, .5)
    data = bytearray(struct.pack("<8sIIff32fII", b"T3SCN12\0", 64, 64, 0, 1,
                                 *IDENTITY, *projection, 1, (3 if shadow else 2) + (2 if reset else 0)))
    data.extend(struct.pack("<III4B", 1, 1, 1, 255, 255, 255, 255))

    def draw(vertices, state, shading, color, position, shadow_op):
        data.extend(struct.pack("<13IfI4f8f12fII", 0, len(vertices), *(state[k] for k in FIELDS),
                                0, shading, *color, *position, *((0,) * 12), 0, shadow_op))
        for point, normal in vertices:
            data.extend(struct.pack("<12f", *point, .5, .5, *normal, 0, 0, 0, 0))

    receiver = [((x, y, 4), (0, 0, -1)) for x, y in
                ((-4, -4), (-4, 4), (4, 4), (-4, -4), (4, 4), (4, -4))]
    draw(receiver, dict(DEFAULT, cull=0), 1, (.125, .125, .125, 1), (0,) * 8, 0)
    if shadow:
        volume_state = dict(DEFAULT, depth_write=0, depth_compare=4, cull=0, color_mask=0)
        draw(cube_volume(), volume_state, 13, (0,) * 4, light, 1)
    light_state = dict(DEFAULT, depth_write=0, depth_compare=4, cull=0, blend_enable=1,
                       blend_src=1, blend_dst=1, color_mask=7)
    draw(receiver, light_state, 2, (0,) * 4, light, 2 if shadow else 0)
    if reset:
        # A second light's empty-on-screen volume must clear the first stencil.
        shifted = [((p[0]+50, p[1], p[2]), n) for p, n in cube_volume()]
        draw(shifted, volume_state, 13, (0,) * 4, light, 1)
        draw(receiver, light_state, 2, (0,) * 4, light, 2)
    return bytes(data)


def verify(shadow, unshadowed):
    header = b"P6\n64 64\n255\n"
    if any(not image.startswith(header) or len(image) != len(header) + 64*64*3
           for image in (shadow, unshadowed)):
        raise ValueError("invalid stencil fixture image")
    def pixel(image, x, y):
        offset = len(header) + (y*64+x)*3
        return tuple(image[offset:offset+3])
    dark, lit, reference_dark, reference_lit = (pixel(shadow, 40, 32), pixel(shadow, 16, 32),
                                                 pixel(unshadowed, 40, 32), pixel(unshadowed, 16, 32))
    if (any(abs(v-32) > 3 for v in dark) or any(v < 70 for v in reference_dark) or
            any(abs(a-b) > 2 for a, b in zip(lit, reference_lit)) or any(v < 70 for v in lit)):
        raise ValueError(f"incorrect shadow pixels: {dark}, {lit}, {reference_dark}, {reference_lit}")
    return 4


def verify_reset(reset, original):
    header = b"P6\n64 64\n255\n"
    if any(not image.startswith(header) or len(image) != len(header) + 64*64*3 for image in (reset, original)):
        raise ValueError("invalid stencil fixture image")
    def pixel(image, x):
        offset = len(header) + (32*64+x)*3
        return image[offset:offset+3]
    if any(a < b+30 for a, b in zip(pixel(reset, 40), pixel(original, 40))):
        raise ValueError("stencil was not cleared between light batches")
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--unshadowed", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.action == "create":
            created = []
            try:
                for path, body in ((args.path, fixture(True)), (args.unshadowed, fixture(False))):
                    with path.open("xb") as dest:
                        info = os.fstat(dest.fileno())
                        created.append((path, info.st_dev, info.st_ino))
                        dest.write(body)
            except OSError:
                for path, dev, ino in created:
                    try:
                        info = path.lstat()
                        if (info.st_dev, info.st_ino) == (dev, ino):
                            path.unlink()
                    except OSError:
                        pass
                raise
        else:
            print(f"stencil_shadow=PASS pixels={verify(args.path.read_bytes(), args.unshadowed.read_bytes())}")
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
