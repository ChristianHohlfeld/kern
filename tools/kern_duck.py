#!/usr/bin/env python3
"""Kern's own look and colliders for the Micro Duck: no third-party geometry.

    python3 -I tools/kern_duck.py            (from the repository root; needs numpy)

Writes kern/src/game/duck_meshes.inc (what the duck looks like: one mesh per link, built from
rounded boxes, capsules and discs designed here) and kern/src/game/duck_colliders.inc (what
collides: oriented boxes for the servo housings, chamfered plates for the soles, plates for the
beak's pads, discs for the roller wheels).

What is taken from elsewhere is only measurement data: the link frames (kern/src/game/micro_duck_model.inc,
from Pollen Robotics' MJCF, Apache-2.0) and the collision envelopes in tools/kern_duck_envelopes.json:
coarse convex hulls (11-40 points each) of where the real robot's colliding parts reach – the servo
housings, the soles, the beak's pads, the trunk and head shells and the roller tyres. They decide how
the duck touches the world exactly as in training; no design can be seen in them. Everything the
duck looks like is drawn here.
"""
import math
import pathlib
import re

import numpy as np

root = pathlib.Path(__file__).resolve().parent.parent
game = root / "kern" / "src" / "game"
model = (game / "micro_duck_model.inc").read_text()

# ---------------------------------------------------------------- link frames (world, home pose)

link_rows = re.findall(r'\{"(\w+)", "(\w*)", (-?\d+), \{([^}]*)\}', model.split("kDuckLinks[]")[1].split("};")[0])
LINKS = [(name, int(parent), np.array([float(x.strip().rstrip("f")) for x in pos.split(",")])) for name, _, parent, pos in link_rows]
INDEX = {name: i for i, (name, _, _) in enumerate(LINKS)}
ORIGIN = [p for _, _, p in LINKS]
CHILDREN = {i: [k for k, (_, par, _) in enumerate(LINKS) if par == i] for i in range(len(LINKS))}

# ---------------------------------------------------------------- measurements (collision envelopes)

import json
ENVELOPES = json.loads((root / "tools" / "kern_duck_envelopes.json").read_text())
PARTS = [(p["link"], p["friction"], p["sole"], np.array(p["points"]).reshape(-1, 3)) for p in ENVELOPES["parts"]]

wheel_rows = re.findall(r'\{"(passive_\w+)", "passive_\w+", (\d+), 0, \{([^}]*)\}, \{([^}]*)\}', model)
WHEELS = [(name, int(ankle), np.array([float(x.strip().rstrip("f")) for x in pos.split(",")]),
           np.array([float(x.strip().rstrip("f")) for x in axis.split(",")])) for name, ankle, pos, axis in wheel_rows]
WHEEL_RADIUS, WHEEL_HALF_WIDTH = 14.8, 4.0  # mm, the look of the tyres


def part_box(p):
    """A box around an envelope (principal axes): where Kern draws the part."""
    pts = p[3]
    c = pts.mean(axis=0)
    _, _, R = np.linalg.svd(pts - c)
    if np.linalg.det(R) < 0:
        R[2] *= -1
    q = (pts - c) @ R.T
    lo, hi = q.min(axis=0), q.max(axis=0)
    return c + ((lo + hi) / 2) @ R, R, (hi - lo) / 2


# ---------------------------------------------------------------- shapes

WHITE, SHADE, DARK, ORANGE, BLUE, BLACK, METAL = (
    (.95, .95, .93), (.80, .82, .85), (.17, .18, .21), (1.0, .56, .13), (.24, .55, 1.0), (.04, .04, .05), (.62, .64, .68))


def rbox(c, R, half, n, color, lat=14, lon=28):
    """A superellipsoid |u/a|^n + |v/b|^n + |w/c|^n = 1 (n = 2: ellipsoid; large n: rounded box)."""
    c, R, half = np.asarray(c, float), np.asarray(R, float), np.asarray(half, float)
    e = 2.0 / n
    P, N = [], []
    for i in range(lat + 1):
        th = -math.pi / 2 + math.pi * i / lat
        for j in range(lon + 1):
            ph = -math.pi + 2 * math.pi * j / lon
            cu, su, cv, sv = math.cos(th), math.sin(th), math.cos(ph), math.sin(ph)
            f = lambda x: math.copysign(abs(x) ** e, x)
            q = np.array([f(cu) * f(cv), f(cu) * f(sv), f(su)]) * half
            g = np.sign(q) * np.abs(q / half) ** (n - 1) / half
            g = g / (np.linalg.norm(g) or 1)
            P.append(c + q @ R)
            N.append(g @ R)
    T = []
    for i in range(lat):
        for j in range(lon):
            a, b = i * (lon + 1) + j, (i + 1) * (lon + 1) + j
            T += [(a, a + 1, b + 1), (a, b + 1, b)]
    return np.array(P), np.array(N), np.tile(color, (len(P), 1)), np.array(T)


def towards(d):
    """Rows u, v, w with w along d."""
    w = np.asarray(d, float); w = w / np.linalg.norm(w)
    t = np.array([1.0, 0, 0]) if abs(w[0]) < .9 else np.array([0, 1.0, 0])
    u = np.cross(t, w); u /= np.linalg.norm(u)
    return np.array([u, np.cross(w, u), w])


def capsule(a, b, r, color):
    a, b = np.asarray(a, float), np.asarray(b, float)
    L = np.linalg.norm(b - a)
    if L < 1e-6:
        return rbox(a, np.eye(3), (r, r, r), 2, color, 8, 16)
    return rbox((a + b) / 2, towards(b - a), (r, r, L / 2 + r), 2.6, color, 10, 16)


def disc(c, axis, r, half_width, color):
    return rbox(c, towards(axis), (r, r, half_width), 8, color, 12, 24)


def merge(parts):
    P, N, C, T, k = [], [], [], [], 0
    for p, n, c, t in parts:
        P.append(p); N.append(n); C.append(c); T.append(t + k); k += len(p)
    return np.concatenate(P), np.concatenate(N), np.concatenate(C), np.concatenate(T)


def support(points, k=32):
    """At most k points of the convex hull: the extreme point along k spread directions."""
    pts = np.asarray(points)
    out = []
    g = math.pi * (3 - math.sqrt(5))
    for i in range(k):
        z = 1 - 2 * (i + .5) / k
        r = math.sqrt(1 - z * z)
        d = np.array([math.cos(g * i) * r, math.sin(g * i) * r, z])
        j = int(np.argmax(pts @ d))
        if j not in out:
            out.append(j)
    return pts[out]


# ---------------------------------------------------------------- the design (link frames, metres)

def local(i, p):
    return np.asarray(p, float) - ORIGIN[i]


def housings(i, shrink=.97):
    out = []
    for p in PARTS:
        if p[0] != i or p[1] != 0:
            continue
        c, R, half = part_box(p)
        out.append(rbox(c, R, half * shrink, 6, DARK, 8, 16))
    return out


def limb(i, r=.0052, color=SHADE):
    """A strut from the link's joint to each child's joint."""
    return [capsule((0, 0, 0), local(i, ORIGIN[k]), r, color) for k in CHILDREN[i]]


def sole_plate(i, color=ORANGE):
    p = next(p for p in PARTS if p[0] == i and p[2])
    c, R, half = part_box(p)
    return rbox(c, R, half * np.array([1, 1, .9]), 5, color, 10, 24)


TRUNK = ((-.0065, 0, -.0005), np.eye(3), (.0405, .0312, .0425), 3.2)
HEAD_BACK = ((-.006, 0, .007), np.eye(3), (.031, .040, .023), 2.6)
HEAD_FACE = ((.0405, 0, .016), np.eye(3), (.0425, .0445, .0241), 3.4)


def design():
    meshes, hulls = {}, {}
    t = INDEX["trunk_base"]
    meshes["trunk_base"] = [rbox(*TRUNK, WHITE),
                            rbox((.0335, 0, .004), np.eye(3), (.0022, .013, .013), 4, BLUE, 8, 16),   # the Kern badge
                            rbox((-.042, 0, .030), towards((-.5, 0, 1)), (.006, .011, .009), 2.4, WHITE, 8, 16)]  # tail
    hulls["trunk_base"] = np.array(ENVELOPES["shells"]["trunk_base"]).reshape(-1, 3)  # what collides
    for side in ("yaw2roll", "bearing_roll", "hip_l", "hip_l_2", "upper_leg_left", "upper_leg_right", "leg", "leg_2",
                 "neck", "neck_pitch", "yaw_roll_motion"):
        i = INDEX[side]
        meshes[side] = housings(i) + limb(i)
        if not meshes[side]:
            meshes[side] = [rbox((0, 0, 0), np.eye(3), (.006, .006, .006), 2, SHADE, 8, 16)]
    for ankle in ("ankle_left", "ankle_right"):
        i = INDEX[ankle]
        sc, sR, sh = part_box(next(p for p in PARTS if p[0] == i and p[2]))
        top = sc + np.array([0, 0, sh[2]])
        meshes[ankle] = [sole_plate(i), capsule((0, 0, 0), top, .0055, SHADE)]
    h = INDEX["jaw_soft"]
    eye = lambda y: [rbox((.058, y, .041), np.eye(3), (.0105, .0105, .0105), 2, WHITE, 12, 24),         # eyeball
                     rbox((.0655, y * 1.02, .0445), np.eye(3), (.0042, .0072, .0074), 2, BLACK, 10, 20),  # pupil
                     rbox((.0697, y * .90, .0478), np.eye(3), (.0012, .0021, .0021), 2, WHITE, 6, 10)]   # glint
    meshes["jaw_soft"] = [rbox(*HEAD_BACK, WHITE), rbox(*HEAD_FACE, WHITE),
                          rbox((.059, 0, -.0066), np.eye(3), (.0252, .0446, .0031), 4, ORANGE, 10, 24),  # upper beak
                          disc((.0833, 0, .0155), (1, 0, 0), .0042, .0012, BLACK),                        # camera
                          rbox((.0833, .0224, .0135), np.eye(3), (.0010, .0032, .0022), 4, DARK, 6, 10)]  # range sensor
    meshes["jaw_soft"] += eye(.024) + eye(-.024)
    hulls["jaw_soft"] = np.array(ENVELOPES["shells"]["jaw_soft"]).reshape(-1, 3)  # what collides
    m = INDEX["mouth"]
    meshes["mouth"] = [rbox((.0415, 0, -.0151), np.eye(3), (.0215, .0446, .0047), 4, ORANGE, 10, 24)]
    for y in (.041, -.041):
        meshes["mouth"].append(capsule((-.006, y, 0), (.022, y, -.013), .0028, SHADE))
    # Roller skates: the shoe on a frame that carries two wheels; the wheels as discs.
    for blade, ankle in (("roller/ankle_l_v1", "ankle_left"), ("roller/ankle_r_v1", "ankle_right")):
        i = INDEX[ankle]
        ws = [w for w in WHEELS if w[1] == i]
        y = float(np.mean([w[2][1] for w in ws]))
        z = float(np.mean([w[2][2] for w in ws]))
        x0, x1 = min(w[2][0] for w in ws), max(w[2][0] for w in ws)
        parts = [sole_plate(i)]
        for dy in (-.0062, .0062):
            parts.append(rbox(((x0 + x1) / 2, y + dy, z * .62), np.eye(3), ((x1 - x0) / 2 + .006, .0011, -z * .42), 5, METAL, 8, 16))
        meshes[blade] = parts
    for name, ankle, pos, axis in WHEELS:
        meshes["roller/" + name] = [disc((0, 0, 0), axis, WHEEL_RADIUS / 1000, WHEEL_HALF_WIDTH / 1000, DARK),
                                    disc((0, 0, 0), axis, .0062, WHEEL_HALF_WIDTH / 1000 + .0006, METAL)]
    return meshes, hulls


ORDER = ["trunk_base", "yaw2roll", "hip_l", "upper_leg_left", "leg", "ankle_left", "neck", "neck_pitch",
         "yaw_roll_motion", "jaw_soft", "bearing_roll", "hip_l_2", "upper_leg_right", "leg_2", "ankle_right", "mouth",
         "roller/ankle_l_v1", "roller/passive_LF_wheel", "roller/passive_LR_wheel",
         "roller/ankle_r_v1", "roller/passive_RF_wheel", "roller/passive_RR_wheel"]


# ---------------------------------------------------------------- colliders

def cf(x):
    s = repr(float(np.float32(x)))
    return (s if ("." in s or "e" in s) else s + ".0") + "f"


def write_colliders(path):
    L = ["// Generated by tools/kern_duck.py from tools/kern_duck_envelopes.json. Do not edit by hand.",
         "// What collides, as in training: the soles (friction 1), the XL330 housings (frictionless) and",
         "// the beak's TPU pads (friction 1), each a coarse convex envelope of the real part. Points",
         "// relative to the link's origin, world-aligned, home pose."]
    flat, rows = [], []
    for link, fr, sole, pts in PARTS:
        rows.append("    {%d, %s, %s, %d, %d}," % (link, cf(fr), "true" if sole else "false", len(flat) // 3, len(pts)))
        flat += list(pts.reshape(-1))
    L.append("const DuckHullDef kDuckHulls[] = {")
    L += rows
    L.append("};")
    L.append("const float kDuckHullPoints[] = {")
    L += ["    " + ", ".join(cf(x) for x in flat[k:k + 12]) + "," for k in range(0, len(flat), 12)]
    L.append("};")
    L.append("// The roller wheels' tyres: 40 points each, in kDuckRollers' order.")
    flat = [x for w in ENVELOPES["wheels"] for x in w]
    L.append("const float kDuckRollerHullPoints[] = {")
    L += ["    " + ", ".join(cf(x) for x in flat[k:k + 12]) + "," for k in range(0, len(flat), 12)]
    L.append("};")
    path.write_text("\n".join(L) + "\n")


def write_meshes(path, meshes, hulls):
    L = ["// Generated by tools/kern_duck.py: Kern's own look for the Micro Duck (rounded boxes, struts, discs).",
         "// Do not edit by hand."]
    table = []
    for name in ORDER:
        P, N, C, T = merge(meshes[name])
        H = hulls.get(name)
        if H is None:
            H = support(P)
        assert len(P) < 65536, name
        lo, hi = P.min(axis=0), P.max(axis=0)
        q = np.round((P - lo) / np.maximum(hi - lo, 1e-9) * 65535 - 32768).astype(int)
        n = np.round(N * 127).astype(int)
        c = np.round(np.clip(C, 0, 1) * 255).astype(int)
        ident = "kDuckMesh_" + name.replace("/", "_")

        def arr(ctype, suffix, values):
            values = list(values)
            L.append("static const %s %s%s[] = {" % (ctype, ident, suffix))
            for k in range(0, len(values), 24):
                L.append("    " + ",".join(str(x) for x in values[k:k + 24]) + ",")
            L.append("};")
        arr("short", "_pos", q.reshape(-1))
        arr("signed char", "_nrm", n.reshape(-1))
        arr("unsigned char", "_col", c.reshape(-1))
        arr("unsigned short", "_idx", T.reshape(-1))
        L.append("static const float %s_hull[] = {%s};" % (ident, ", ".join(cf(x) for x in H.reshape(-1))))
        vec = lambda v: "{%s}" % ", ".join(cf(x) for x in v)
        table.append('    {"duck/%s", %s, %s, %d, %d, %s_pos, %s_nrm, %s_col, %s_idx, %d, %s_hull},' % (
            name, vec(lo), vec(hi), len(P), T.size, ident, ident, ident, ident, len(H), ident))
    L.append("const DuckMeshDef kDuckMeshes[] = {")
    L += table
    L.append("};")
    path.write_text("\n".join(L) + "\n")


if __name__ == "__main__":
    meshes, hulls = design()
    write_meshes(game / "duck_meshes.inc", meshes, hulls)
    write_colliders(game / "duck_colliders.inc")
    print("wrote", game / "duck_meshes.inc", "and", game / "duck_colliders.inc")
