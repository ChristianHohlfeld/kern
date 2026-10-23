#!/usr/bin/env python3
"""Mesh simplification for imported robot meshes (STL): weld, decimate, crease normals, quantise.

Used by tools/micro_duck_import.py. Needs numpy.

Decimation is vertex clustering with quadric representatives (Lindstrom, "Out-of-core
simplification of large polygonal models", SIGGRAPH 2000): the mesh is binned into a grid; each
occupied cell becomes one vertex at the point minimising the summed plane distances (quadric error)
of the triangles that touched it, clamped to the cell; triangles whose corners fall into fewer than
three cells disappear. The cell size is searched so the result lands near a triangle budget. Sharp
CAD edges survive because the quadric pulls the cell's vertex onto them.

Normals: area-weighted face normals averaged per corner over the faces within a crease angle, so
flat faces stay flat and fillets stay smooth.
"""
import struct

import numpy as np


def read_stl(path):
    b = open(path, "rb").read()
    if b[:5] == b"solid" and b"facet" in b[:512]:
        v = []
        for line in b.decode("ascii", "replace").splitlines():
            t = line.split()
            if t and t[0] == "vertex":
                v.append([float(t[1]), float(t[2]), float(t[3])])
        return np.array(v, dtype=np.float64).reshape(-1, 3, 3)
    n = struct.unpack("<I", b[80:84])[0]
    rec = np.frombuffer(b, dtype=np.dtype([("n", "<f4", 3), ("v", "<f4", (3, 3)), ("a", "<u2")]), count=n, offset=84)
    return rec["v"].astype(np.float64)


def weld(tris, eps=1e-7):
    flat = tris.reshape(-1, 3)
    key = np.round(flat / eps).astype(np.int64)
    uniq, inv = np.unique(key, axis=0, return_inverse=True)
    verts = np.zeros((len(uniq), 3))
    np.add.at(verts, inv, flat)
    verts /= np.bincount(inv)[:, None]
    faces = inv.reshape(-1, 3)
    ok = (faces[:, 0] != faces[:, 1]) & (faces[:, 1] != faces[:, 2]) & (faces[:, 0] != faces[:, 2])
    return verts, faces[ok]


def cluster(verts, faces, cell):
    lo = verts.min(axis=0)
    ijk = np.floor((verts - lo) / cell).astype(np.int64)
    keys, cid = np.unique(ijk, axis=0, return_inverse=True)
    cid = cid.reshape(-1)
    f = cid[faces]
    keep = (f[:, 0] != f[:, 1]) & (f[:, 1] != f[:, 2]) & (f[:, 0] != f[:, 2])
    # Quadrics of the original triangles, accumulated per cell of each corner.
    a, b, c = verts[faces[:, 0]], verts[faces[:, 1]], verts[faces[:, 2]]
    n = np.cross(b - a, c - a)
    area = np.linalg.norm(n, axis=1)
    nn = n / np.maximum(area, 1e-30)[:, None]
    d = -np.einsum("ij,ij->i", nn, a)
    w = area
    A = np.einsum("i,ij,ik->ijk", w, nn, nn)
    B = (w * d)[:, None] * nn
    QA = np.zeros((len(keys), 3, 3))
    QB = np.zeros((len(keys), 3))
    for k in range(3):
        np.add.at(QA, cid[faces[:, k]], A)
        np.add.at(QB, cid[faces[:, k]], B)
    mean = np.zeros((len(keys), 3))
    np.add.at(mean, cid, verts)
    mean /= np.bincount(cid, minlength=len(keys))[:, None]
    # Minimise x^T A x + 2 b^T x around the cell mean (regularised: falls back to the mean along
    # directions the planes do not constrain).
    reg = 1e-3 * np.trace(QA, axis1=1, axis2=2)[:, None, None] * np.eye(3) + 1e-30 * np.eye(3)
    rhs = -QB - np.einsum("ijk,ik->ij", QA, mean)
    delta = np.linalg.solve(QA + reg, rhs[..., None])[..., 0]
    pos = mean + delta
    cmin = lo + keys * cell
    pos = np.clip(pos, cmin - 0.5 * cell, cmin + 1.5 * cell)
    nf = f[keep]
    # Drop duplicate triangles (same corners, any order, same winding kept once).
    s = np.sort(nf, axis=1)
    _, first = np.unique(s, axis=0, return_index=True)
    nf = nf[np.sort(first)]
    used = np.unique(nf)
    remap = -np.ones(len(pos), dtype=np.int64)
    remap[used] = np.arange(len(used))
    return pos[used], remap[nf]


def simplify(verts, faces, budget):
    if len(faces) <= budget:
        return verts, faces
    ext = np.max(verts.max(axis=0) - verts.min(axis=0))
    lo_c, hi_c = ext / 4000, ext / 4
    best = None
    for _ in range(22):
        mid = np.sqrt(lo_c * hi_c)
        v, f = cluster(verts, faces, mid)
        if len(f) > budget:
            lo_c = mid
        else:
            hi_c = mid
            best = (v, f)
    if best is None:
        best = cluster(verts, faces, hi_c)
    return best


def crease_normals(verts, faces, crease_deg=32):
    """Split vertices along creases; returns (positions, normals, triangles)."""
    a, b, c = verts[faces[:, 0]], verts[faces[:, 1]], verts[faces[:, 2]]
    fn = np.cross(b - a, c - a)
    fu = fn / np.maximum(np.linalg.norm(fn, axis=1), 1e-30)[:, None]
    cos_t = np.cos(np.radians(crease_deg))
    # corners grouped by vertex
    corner_v = faces.reshape(-1)
    corner_f = np.repeat(np.arange(len(faces)), 3)
    order = np.argsort(corner_v, kind="stable")
    out_p, out_n, new_index = [], [], np.zeros(len(corner_v), dtype=np.int64)
    starts = np.flatnonzero(np.r_[True, corner_v[order][1:] != corner_v[order][:-1]])
    ends = np.r_[starts[1:], len(order)]
    for s0, e0 in zip(starts, ends):
        idx = order[s0:e0]
        vf = corner_f[idx]
        normals = fu[vf]
        groups = []  # list of (seed normal, [corner indices])
        for k, ci in enumerate(idx):
            nk = normals[k]
            for g in groups:
                if np.dot(g[0], nk) >= cos_t:
                    g[1].append(ci)
                    g[2].append(vf[k])
                    break
            else:
                groups.append([nk, [ci], [vf[k]]])
        v = corner_v[idx[0]]
        for seed, cis, fs in groups:
            nsum = fn[fs].sum(axis=0)  # area weighted
            nsum /= max(np.linalg.norm(nsum), 1e-30)
            new = len(out_p)
            out_p.append(verts[v])
            out_n.append(nsum)
            for ci in cis:
                new_index[ci] = new
    return np.array(out_p), np.array(out_n), new_index.reshape(-1, 3)


def mass_properties(verts, faces, density):
    """Mass, centre of mass and inertia tensor about it of a closed triangle mesh (divergence
    theorem over signed tetrahedra from the origin)."""
    a, b, c = verts[faces[:, 0]], verts[faces[:, 1]], verts[faces[:, 2]]
    d = np.einsum("ij,ij->i", a, np.cross(b, c))  # 6 x signed volume
    if d.sum() < 0:
        d = -d  # inward winding
    vol = d.sum() / 6
    com = ((a + b + c) * d[:, None]).sum(axis=0) / (24 * vol)
    # second moments: integral of x_i x_j over each tetra (origin, a, b, c)
    def sec(i, j):
        return (d * (2 * (a[:, i] * a[:, j] + b[:, i] * b[:, j] + c[:, i] * c[:, j])
                     + a[:, i] * b[:, j] + a[:, j] * b[:, i] + a[:, i] * c[:, j] + a[:, j] * c[:, i]
                     + b[:, i] * c[:, j] + b[:, j] * c[:, i])).sum() / 120
    S = np.array([[sec(i, j) for j in range(3)] for i in range(3)]) * density
    m = vol * density
    S -= m * np.outer(com, com)  # about the centre of mass
    inertia = np.trace(S) * np.eye(3) - S
    return m, com, inertia


def hull_points(verts, count=30):
    """Points of the convex hull's silhouette: the extreme vertex along `count` spread directions
    (a Fibonacci sphere), duplicates removed. Few enough for Box3D's hull limits."""
    k = np.arange(count) + 0.5
    phi = np.arccos(1 - 2 * k / count)
    theta = np.pi * (1 + 5 ** 0.5) * k
    dirs = np.c_[np.cos(theta) * np.sin(phi), np.sin(theta) * np.sin(phi), np.cos(phi)]
    idx = np.unique(np.argmax(verts @ dirs.T, axis=0))
    return verts[idx]
