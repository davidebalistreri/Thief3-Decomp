"""Prepare bounded world-space SDK volume candidates for direct-light draws."""
import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ir"))
from scene_ir import transform_point

MAX_LIGHTS = 64
MAX_EXPANDED_VERTICES = 300000


def _normal_transform(matrix):
    a, b, c, d, e, f, g, h, i = (matrix[k] for k in (0, 1, 2, 4, 5, 6, 8, 9, 10))
    cof = (e*i-f*h, f*g-d*i, d*h-e*g, c*h-b*i, a*i-c*g, b*g-a*h, b*f-c*e, c*d-a*f, a*e-b*d)
    det = a*cof[0] + b*cof[1] + c*cof[2]
    if not math.isfinite(det) or abs(det) < 1e-12:
        raise ValueError("shadow caster transform is singular")
    return tuple(value/det for value in cof)


def _unit(value):
    length = math.sqrt(sum(v*v for v in value))
    if not math.isfinite(length) or length < 1e-10:
        raise ValueError("shadow volume contains an invalid face normal")
    return tuple(v/length for v in value)


def _sub(a, b):
    return tuple(a[i]-b[i] for i in range(3))


def _dot(a, b):
    return sum(a[i]*b[i] for i in range(3))


def _cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def _encloses(positions, normals, indices, light):
    """Use the SDK's outward-face ray test to avoid enclosing-shell casters."""
    if any(light[k] < min(p[k] for p in positions) or light[k] > max(p[k] for p in positions) for k in range(3)):
        return False
    direction = _unit((.5773, .5779, .5767))
    crossings = 0
    for i in range(0, len(indices), 3):
        ia, ib, ic = indices[i:i+3]
        a, b, c = positions[ia], positions[ib], positions[ic]
        e1, e2 = _sub(b, a), _sub(c, a)
        h = _cross(direction, e2)
        det = _dot(e1, h)
        if abs(det) < 1e-10:
            continue
        q = _sub(light, a)
        u = _dot(q, h) / det
        if not 0 <= u <= 1:
            continue
        r = _cross(q, e1)
        v = _dot(direction, r) / det
        if v < 0 or u + v > 1 or _dot(e2, r) / det <= 1e-6:
            continue
        crossings += 1 if _dot(normals[ia], direction) > 0 else -1
    return crossings > 0


def prepare(document, candidates, selected, actors):
    """Build one expanded caster batch per explicitly selected semantic light."""
    if (not isinstance(selected, (list, tuple)) or not 1 <= len(selected) <= MAX_LIGHTS or
            any(not isinstance(value, str) or not value for value in selected) or len(set(selected)) != len(selected)):
        raise ValueError("select unique semantic lights for shadow candidates")
    resources = {r["id"]: r for r in document["resources"]}
    transformed = []
    for caster in document["casters"]:
        resource = resources[caster["resource_id"]]
        matrix = caster["transform"]
        normal_matrix = _normal_transform(matrix)
        positions = []
        for point in resource["positions"]:
            value = transform_point(point, matrix)
            if abs(value[3]-1) > 1e-6 or any(not math.isfinite(v) or abs(v) > 1000000 for v in value[:3]):
                raise ValueError("shadow vertex exceeds renderer bounds")
            positions.append(tuple(value[:3]))
        normals = [_unit(tuple(sum(n[i]*normal_matrix[i*3+j] for i in range(3)) for j in range(3)))
                   for n in resource["face_normals"]]
        indices = list(resource["indices"])
        if caster["reflected"]:
            for n in range(0, len(indices), 3):
                indices[n+1], indices[n+2] = indices[n+2], indices[n+1]
        transformed.append((caster, positions, normals, indices))
    batches, total_vertices = {}, 0
    for identity in selected:
        samples = [item for item in candidates.values() if item.get("offline_light_id") == identity and
                   item.get("mode") in ("point_approximation", "spot_approximation")]
        if not samples:
            raise ValueError("shadow light does not match an enabled direct candidate")
        positions = {tuple(item["position"]) for item in samples}
        if len(positions) != 1:
            raise ValueError("shadow light positions differ between draws")
        light = next(iter(positions))
        light_name = identity.rsplit(":", 1)[-1]
        actor = actors.get(light_name)
        if not isinstance(actor, dict):
            raise ValueError("shadow light lacks a fingerprinted offline actor")
        ignored = {light_name, actor.get("attached_to")}
        vertices, casters, skipped = [], [], []
        for caster, positions, normals, indices in transformed:
            target = caster["target_id"]
            if caster["target_kind"] == "mesh_instance" and target.rsplit(":", 1)[-1] in ignored:
                skipped.append({"target_id": target, "reason": "offline_light_owner_or_parent"})
                continue
            if _encloses(positions, normals, indices, light):
                skipped.append({"target_id": target, "reason": "encloses_light_candidate"})
                continue
            total_vertices += len(indices)
            if total_vertices > MAX_EXPANDED_VERTICES:
                raise ValueError("shadow candidate batches exceed renderer bounds")
            for index in indices:
                vertices.append((*positions[index], 0, 0, *normals[index], 0, 0, 0, 0))
            casters.append(target)
        if not vertices:
            raise ValueError("shadow light has no candidate caster geometry")
        batches[identity] = {"position": light, "vertices": vertices, "casters": casters, "skipped": skipped}
    return batches
