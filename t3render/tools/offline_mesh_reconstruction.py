#!/usr/bin/env python3
"""Build a diagnostic DX12 packet from offline T3 assets and trace candidates.

This is a reconstruction, not a runtime frame, a Scene IR or a replay of the
GPU inputs. Both matching and colors are explicitly provisional.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import sys

import correlate_trace_actor as correlation
import trace_summary

ASSETS = Path(__file__).resolve().parents[2] / "tools" / "assets"
sys.path.insert(0, str(ASSETS))
from ibt import open_ibt  # noqa: E402
from t3bsp import find_render_blocks  # noqa: E402
from t3common import game_dir, resolve_map  # noqa: E402
from t3mesh import TYPE_STATICMESH, parse_static_mesh  # noqa: E402
from upkg import Package  # noqa: E402

MAX_TRIANGLES = 4096
IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
PALETTE = [0xff8c6b50, 0xffa29883, 0xff74685d, 0xff9e987e, 0xff887c69,
           0xffa58a6b, 0xff7f8178, 0xffb0a591]


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def signature(draw):
    if draw.get("call") != "DrawIndexedPrimitive" or draw.get("primitive_type") != 4:
        raise ValueError("candidate is not an indexed triangle-list draw")
    values = tuple(draw.get(k) for k in ("primitive_count", "num_vertices", "start_index"))
    if any(type(v) is not int or v < 0 for v in values):
        raise ValueError("invalid candidate section signature")
    return values


def section_signature(section):
    if section.index_count != section.triangles * 3:
        raise ValueError("section is not a complete triangle list")
    return section.triangles, section.vertex_count, section.first_index


def vectors(draw):
    transform = draw.get("transforms", {})
    world = transform.get("world", [])
    matrix = correlation.finite_vector(world[0], 16) if isinstance(world, list) and world else None
    view = correlation.finite_vector(transform.get("view"), 16)
    projection = correlation.finite_vector(transform.get("projection"), 16)
    viewport = draw.get("viewport", {})
    if matrix is None or view is None or projection is None or not isinstance(viewport, dict):
        raise ValueError("missing world, view, projection or viewport")
    if viewport.get("x") != 0 or viewport.get("y") != 0:
        raise ValueError("only a viewport at origin is supported")
    width, height = viewport.get("width"), viewport.get("height")
    near, far = viewport.get("min_z"), viewport.get("max_z")
    if (type(width) is not int or type(height) is not int or not 0 < width <= 2048 or not 0 < height <= 2048 or
        any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) for v in (near, far)) or
        not 0 <= near <= far <= 1):
        raise ValueError("unsupported viewport or depth range")
    if any(abs(v) > 10000 for v in matrix + view + projection):
        raise ValueError("matrix exceeds diagnostic parser bounds")
    return matrix, view, projection, (width, height, near, far)


def transformed_position(position, matrix):
    if len(position) != 3 or any(not math.isfinite(v) for v in position):
        raise ValueError("non-finite asset vertex")
    p = [*position, 1]
    out = [sum(p[row] * matrix[row * 4 + col] for row in range(4)) for col in range(4)]
    if not math.isfinite(out[3]) or abs(out[3] - 1) > 1e-5 or any(
            not math.isfinite(v) or abs(v) > 10000 for v in out[:3]):
        raise ValueError("unsupported transformed asset vertex")
    return out[:3]


def triangle_vertices(mesh, section, matrix, color):
    start, count = section.first_index, section.index_count
    if start < 0 or count <= 0 or start + count > len(mesh.indices) or count != section.triangles * 3:
        raise ValueError("invalid section index range")
    indices = mesh.indices[start:start + count]
    if any(i < section.first_vertex or i >= section.first_vertex + section.vertex_count or
           i >= len(mesh.positions) for i in indices):
        raise ValueError("section vertex indices exceed its declared range")
    return [(transformed_position(mesh.positions[index], matrix), color) for index in indices]


def create_packet(records, frame, actors, actor_name, mesh, bed_ops, blocks, bsp_specs):
    if not bed_ops or len(bed_ops) != len(set(bed_ops)):
        raise ValueError("provide distinct actor draw operation IDs")
    rows = [r for r in records if r.get("frame") == frame]
    ends = [r for r in rows if r.get("event") == "frame_end"]
    if (len(ends) != 1 or ends[0].get("present_hr") != 0 or ends[0].get("had_reset") is not False or
            ends[0].get("concurrent_operations") is not False):
        raise ValueError("frame must have a successful Present, no reset and no recorded overlap")
    draws = {r["op_id"]: r for r in rows if r.get("event") == "draw"}
    results = {r["op_id"]: r for r in rows if r.get("event") == "draw_result"}
    selected_ops = bed_ops + [op for _, ops in bsp_specs for op in ops]
    if len(selected_ops) != len(set(selected_ops)) or any(op not in draws for op in selected_ops):
        raise ValueError("draw IDs must exist and be distinct")
    if any(results.get(op, {}).get("hr") != 0 or results[op].get("buffer_stable") is not True for op in selected_ops):
        raise ValueError("selected draws must have successful, stable observed windows")
    required_states = {"7": 1, "14": 1, "22": 3, "23": 2, "27": 0, "52": 0, "168": 15}
    if any(any(draws[op].get("render_states", {}).get(key) != value for key, value in required_states.items())
           for op in selected_ops):
        raise ValueError("selected draw states must match the reconstruction's depth and culling subset")
    actor_match = correlation.correlate(actors, actor_name, mesh, records, frame, 0.1)
    if actor_match["actors_within_tolerance"] != [actor_name]:
        raise ValueError("actor position is not unique within the correlation tolerance")
    selected_matches = [m for m in actor_match["matches"] if m["op_id"] in bed_ops]
    matched = {m["op_id"]: m for m in selected_matches}
    if len(matched) != len(selected_matches):
        raise ValueError("actor draw signature matches more than one mesh section")
    if len(matched) != len(bed_ops) or len({m["section"] for m in matched.values()}) != len(bed_ops):
        raise ValueError("actor draw IDs do not uniquely match mesh sections")

    first = draws[bed_ops[0]]
    _, view, projection, viewport = vectors(first)
    vertices, sources = [], []
    for op in bed_ops:
        world, v, p, vp = vectors(draws[op])
        if (v, p, vp) != (view, projection, viewport):
            raise ValueError("actor draws do not share a camera and viewport")
        match = matched[op]
        if str(match["material"]).casefold() == "bf":
            raise ValueError("hidden BF material is not rendered by this reconstruction")
        section = mesh.render.sections[match["section"]]
        vertices.extend(triangle_vertices(mesh.render, section, world, PALETTE[len(sources) % len(PALETTE)]))
        sources.append({"kind": "offline_actor_candidate", "actor": actor_name, "mesh": mesh.name,
                        "section": match["section"], "material": match["material"], "op_id": op,
                        "triangles": section.triangles})
    for index, ops in bsp_specs:
        if index < 0 or index >= len(blocks) or not ops:
            raise ValueError("BSP block or draw IDs are invalid")
        block = blocks[index]
        signatures = [signature(draws[op]) for op in ops]
        candidates = []
        for candidate_index, candidate in enumerate(blocks):
            counts = Counter(section_signature(s) for s in candidate.mesh.sections)
            if not (Counter(signatures) - counts):
                candidates.append(candidate_index)
        if candidates != [index]:
            raise ValueError("BSP section signatures do not uniquely identify the requested block")
        bindings = []
        for op in ops:
            draw = draws[op]
            world, v, p, vp = vectors(draw)
            if any(abs(a - b) > 1e-5 for a, b in zip(world, IDENTITY)) or (v, p, vp) != (view, projection, viewport):
                raise ValueError("BSP candidates must have identity world and share the actor camera")
            stream = next((s for s in draw.get("streams", []) if s.get("stream") == 0), {})
            index_binding = draw.get("indices", {})
            binding = (stream.get("buffer"), index_binding.get("buffer"))
            if not all(isinstance(x, str) and x != "0x0" for x in binding):
                raise ValueError("BSP candidates require observed VB and IB bindings")
            bindings.append(binding)
            hits = [i for i, s in enumerate(block.mesh.sections) if section_signature(s) == signature(draw)]
            if len(hits) != 1:
                raise ValueError("BSP draw does not uniquely match one block section")
            section = block.mesh.sections[hits[0]]
            vertices.extend(triangle_vertices(block.mesh, section, IDENTITY, PALETTE[len(sources) % len(PALETTE)]))
            sources.append({"kind": "offline_bsp_candidate", "block_index": index, "block_id": block.block_id,
                            "level_offset": block.offset, "section": hits[0], "material": section.name,
                            "op_id": op, "triangles": section.triangles})
        if len(set(bindings)) != 1:
            raise ValueError("BSP section candidates must share VB and IB bindings")
    if len(vertices) % 3 or not 0 < len(vertices) // 3 <= MAX_TRIANGLES:
        raise ValueError("triangle count exceeds diagnostic parser limits")
    width, height, near, far = viewport
    lines = ["T3R_RECONSTRUCTION_XYZ_V1", f"{width} {height}", f"{near:.9g} {far:.9g}",
             str(0xff16181c), " ".join(map(str, IDENTITY)),
             " ".join(f"{x:.9g}" for x in view), " ".join(f"{x:.9g}" for x in projection),
             str(len(vertices) // 3)]
    lines.extend(" ".join(f"{x:.9g}" for x in point) + f" {color}" for point, color in vertices)
    return "\n".join(lines) + "\n", {"frame": frame, "triangles": len(vertices) // 3,
        "viewport": {"width": width, "height": height, "min_z": near, "max_z": far}, "sources": sources,
        "limitations": ["Offline actor/BSP correlations are candidates, not runtime identities or GPU buffer data.",
                        "Vertex positions are transformed offline; view/projection are observed D3D8 state, not verified shader behavior.",
                        "Palette colors are diagnostic placeholders; textures, lighting, blending and depth history are not reproduced.",
                        "LESS depth and CCW culling are reconstructed from selected draw states; depth format and winding parity remain unverified.",
                        "Only the selected sections are present; this is not the original frame or a complete bedroom."]}


def parse_bsp(value):
    try:
        index, ops = value.split(":", 1)
        numbers = [int(op) for op in ops.split(",")]
        if int(index) < 0 or not numbers or any(op < 1 for op in numbers):
            raise ValueError
        return int(index), numbers
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected BLOCK:OP[,OP...] with nonnegative block and positive operations") from exc


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    parser.add_argument("actors")
    parser.add_argument("map")
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--actor", required=True)
    parser.add_argument("--frame", required=True, type=int)
    parser.add_argument("--actor-op", type=int, action="append", required=True)
    parser.add_argument("--bsp", type=parse_bsp, action="append", default=[])
    parser.add_argument("--output", required=True, help="new packet path; a .manifest.json sidecar is also created")
    args = parser.parse_args(argv)
    try:
        trace_summary.summarize(args.trace)
        records = [json.loads(line) for line in Path(args.trace).read_text(encoding="utf-8").splitlines()]
        exported = json.loads(Path(args.actors).read_text(encoding="utf-8"))
        if not isinstance(exported, dict) or not isinstance(exported.get("actors"), list) or \
                str(exported.get("level", "")).casefold() != Path(args.map).stem.casefold():
            raise ValueError("invalid actor export or level mismatch")
        actor = next((a for a in exported["actors"] if isinstance(a, dict) and a.get("name") == args.actor), None)
        if actor is None or not isinstance(actor.get("mesh"), str):
            raise ValueError("actor mesh not found")
        with open_ibt(args.map, args.game_dir) as ibt:
            resource = ibt.find(actor["mesh"], TYPE_STATICMESH)
            if resource is None:
                raise ValueError("actor mesh not found in map bundle")
            mesh = parse_static_mesh(resource, ibt.split(resource))
            resource_hash = hashlib.sha256(ibt.read(resource)).hexdigest()
        gmp = resolve_map(game_dir(args.game_dir), args.map, ".gmp")
        blocks = find_render_blocks(Package(gmp)) if args.bsp else []
        packet, manifest = create_packet(records, args.frame, exported["actors"], args.actor, mesh,
                                         args.actor_op, blocks, args.bsp)
        manifest.update({"kind": "offline_reconstruction_not_runtime_frame", "map": exported["level"],
                         "trace_sha256": sha256(args.trace), "actors_sha256": sha256(args.actors),
                         "mesh_resource_sha256": resource_hash, "map_sha256": sha256(gmp) if args.bsp else None,
                         "packet_sha256": hashlib.sha256(packet.encode("ascii")).hexdigest()})
        destination = Path(args.output)
        sidecar = Path(str(destination) + ".manifest.json")
        if destination.exists() or sidecar.exists():
            raise ValueError("packet and manifest must both be new files")
        with destination.open("x", encoding="ascii", newline="\n") as output:
            output.write(packet)
        with sidecar.open("x", encoding="utf-8") as output:
            json.dump(manifest, output, indent=2, allow_nan=False)
            output.write("\n")
        print(json.dumps(manifest, indent=2, allow_nan=False))
    except (OSError, ValueError, KeyError, TypeError, trace_summary.TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
