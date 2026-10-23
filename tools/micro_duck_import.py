#!/usr/bin/env python3
"""Imports Pollen Robotics' Micro Duck into Kern: robot model and walking policy.

    python3 -I tools/micro_duck_import.py ROBOT_XML ASSETS_DIR POLICY_ONNX OUT_DIR

ROBOT_XML    robot_allcollisions_backlash.xml from pollen-robotics/microduck_rl
             (src/mjlab_microduck/robot/microduck/), the model the velstand policy was trained on.
ASSETS_DIR   the STL folder next to it. Only the extents of the colliding meshes are read:
             the XL330 servo housing and the two soles. No mesh is copied into Kern.
POLICY_ONNX  velstand.onnx from huggingface.co/pollen-robotics/microduck-policies.

Writes OUT_DIR/micro_duck_model.inc and OUT_DIR/micro_duck_velstand.inc (C++ initialisers,
included by src/game/walker.cpp). Needs only the Python standard library.

What is converted, and how (docs/walker.md explains why):
  * Bodies, joints, masses, centres of mass and full inertia tensors from the MJCF, posed in the
    STAND keyframe (the policy's home pose). Kern builds every link in that pose; the joints carry
    the home angle as their reference, so a joint reads exactly the MJCF angle.
  * Actor frames are world-aligned (no rotation): inertia tensors and collider boxes are rotated
    into the world frame of the home pose.
  * Colliders are what collides in training (mjlab CollisionCfg FULL_COLLISION): the two soles
    (friction 1) and every XL330 housing (frictionless, condim 1). Each becomes an oriented box
    fitted to its mesh.
  * The policy: obs normaliser (mean, divisor) and the four Gemm layers, as exact hex floats.
"""
import math
import os
import struct
import sys
import xml.etree.ElementTree as ET

# ---------------------------------------------------------------- small linear algebra

def qmul(a, b):
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (aw * bw - ax * bx - ay * by - az * bz,
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw)

def qnorm(q):
    n = math.sqrt(sum(x * x for x in q))
    return tuple(x / n for x in q)

def qmat(q):
    w, x, y, z = qnorm(q)
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
            [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
            [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]

def mmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]

def mvec(a, v):
    return [sum(a[i][k] * v[k] for k in range(3)) for i in range(3)]

def mT(a):
    return [[a[j][i] for j in range(3)] for i in range(3)]

def add(a, b):
    return [a[i] + b[i] for i in range(3)]

def rz(angle):
    c, s = math.cos(angle), math.sin(angle)
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]

def floats(s, n=None):
    v = [float(x) for x in s.split()]
    assert n is None or len(v) == n, s
    return v

# ---------------------------------------------------------------- MJCF

HOME = {  # STAND keyframe (scene.xml) = HOME_FRAME (microduck_constants.py)
    "left_hip_yaw": 0.0, "left_hip_roll": -0.08726646259971647, "left_hip_pitch": -0.457924,
    "left_knee": -0.004940, "left_ankle": 0.452984,
    "neck_pitch": 0.3490658503988659, "head_pitch": 0.3490658503988659, "head_yaw": 0.0, "head_roll": 0.0,
    "right_hip_yaw": 0.0, "right_hip_roll": 0.08726646259971647, "right_hip_pitch": 0.457924,
    "right_knee": 0.004940, "right_ankle": -0.452984,
}
POLICY_ORDER = ["left_hip_yaw", "left_hip_roll", "left_hip_pitch", "left_knee", "left_ankle",
                "neck_pitch", "head_pitch", "head_yaw", "head_roll",
                "right_hip_yaw", "right_hip_roll", "right_hip_pitch", "right_knee", "right_ankle"]
SERVO_MESH = "xl330"
TIRE_MESH = "tire"  # the roller skates' wheels (robot_groundcontact_rollers_backlash.xml)
SOLES = {"left_foot_collision", "right_foot_collision"}
SENSOR_SITES = {"head_camera", "tof", "imu", "mouth_tip"}  # the robot's camera, ToF, trunk IMU and beak tip


def stl_extent(path):
    data = open(path, "rb").read()
    lo, hi = [1e9] * 3, [-1e9] * 3
    if data[:5] == b"solid" and b"facet" in data[:400]:
        for line in data.decode(errors="ignore").splitlines():
            p = line.split()
            if p and p[0] == "vertex":
                v = [float(x) for x in p[1:4]]
                lo = [min(lo[i], v[i]) for i in range(3)]
                hi = [max(hi[i], v[i]) for i in range(3)]
    else:
        n = struct.unpack_from("<I", data, 80)[0]
        for t in range(n):
            off = 84 + t * 50 + 12
            for k in range(3):
                v = struct.unpack_from("<3f", data, off + 12 * k)
                lo = [min(lo[i], v[i]) for i in range(3)]
                hi = [max(hi[i], v[i]) for i in range(3)]
    return [(lo[i] + hi[i]) / 2 for i in range(3)], [hi[i] - lo[i] for i in range(3)]


def load_robot(xml_path, assets):
    root = ET.parse(xml_path).getroot()
    meshes = {}
    for m in root.iter("mesh"):
        f = m.get("file")
        meshes[m.get("name") or os.path.splitext(f)[0]] = os.path.join(assets, f)
    links = []  # (name, joint, parent index, origin, R, inertial, boxes)
    materials = {m.get("name"): floats(m.get("rgba"), 4) for m in root.iter("material")}

    def walk(body, parent, p_pos, p_R):
        pos = floats(body.get("pos", "0 0 0"), 3)
        quat = floats(body.get("quat", "1 0 0 0"), 4)
        origin = add(p_pos, mvec(p_R, pos))
        R = mmul(p_R, qmat(quat))
        joint = None
        for j in body.findall("joint"):
            name = j.get("name")
            if name.startswith("passive_") and name.endswith("_wheel"):
                joint = (name, None, None)  # a roller's free-spinning wheel (rollers model)
                continue
            if name.startswith("passive_"):
                continue  # gearbox backlash (+-1 degree): not modelled, see docs/walker.md
            assert j.get("type", "hinge") == "hinge" and floats(j.get("axis", "0 0 1")) == [0, 0, 1]
            assert floats(j.get("pos", "0 0 0")) == [0, 0, 0]
            lo, hi = floats(j.get("range"), 2)
            joint = (name, lo, hi)
            R = mmul(R, rz(HOME[name]))  # posed in the home keyframe
        inertial = body.find("inertial")
        ipos = floats(inertial.get("pos"), 3)
        mass = float(inertial.get("mass"))
        fi = floats(inertial.get("fullinertia"), 6)
        assert inertial.get("quat") is None
        I = [[fi[0], fi[3], fi[4]], [fi[3], fi[1], fi[5]], [fi[4], fi[5], fi[2]]]
        boxes = []
        for g in body.findall("geom"):
            if g.get("class") not in ("collision",):
                continue
            mesh = g.get("mesh")
            sole = g.get("name") in SOLES
            if not sole and mesh != SERVO_MESH and mesh != TIRE_MESH:
                continue  # not colliding in training (CollisionCfg disables unnamed geoms)
            c, size = stl_extent(meshes[mesh])
            gR = qmat(floats(g.get("quat", "1 0 0 0"), 4))
            gpos = floats(g.get("pos", "0 0 0"), 3)
            # body-local centre and orientation of the box, then the joint's home rotation
            local_c = add(gpos, mvec(gR, c))
            boxes.append({"c": local_c, "R": gR, "size": size, "sole": sole, "path": meshes[mesh], "gpos": gpos})
        visuals = []
        for g in body.findall("geom"):
            if g.get("class") == "visual":
                visuals.append({"mesh": g.get("mesh"), "path": meshes[g.get("mesh")], "pos": floats(g.get("pos", "0 0 0"), 3),
                                "R": qmat(floats(g.get("quat", "1 0 0 0"), 4)), "rgba": materials[g.get("material")]})
        sites = []
        for st in body.findall("site"):
            if st.get("name") in SENSOR_SITES:
                sites.append({"name": st.get("name"), "pos": floats(st.get("pos", "0 0 0"), 3), "R": qmat(floats(st.get("quat", "1 0 0 0"), 4))})
        index = len(links)
        links.append({"name": body.get("name"), "joint": joint, "parent": parent, "origin": origin, "R": R,
                      "mass": mass, "ipos": ipos, "I": I, "boxes": boxes, "sites": sites, "visuals": visuals})
        for child in body.findall("body"):
            walk(child, index, origin, R)

    trunk = root.find("worldbody").find("body")
    walk(trunk, -1, [0, 0, 0], [[1, 0, 0], [0, 1, 0], [0, 0, 1]])
    return links

# ---------------------------------------------------------------- visuals and the mouth

# The beak's lower half turns on the mouth servo (robotd: MOUTH_CLOSED -5 deg .. MOUTH_OPEN 30 deg).
# The training MJCF models the head as one rigid body; the moving parts are the jaw (the U-shaped
# PLA frame whose ears ride the hinge) and the soft TPU pad it carries.
MOUTH_PARTS = {"jaw": 1240.0, "jaw_soft": 1210.0}  # kg/m^3: printed PLA, TPU
MOUTH_RANGE = (math.radians(0.0), math.radians(35.0))  # 0 = the CAD pose, pads together
GRIP_PADS = {"jaw_soft": "mouth", "soft_mouth_top": "head"}  # the beak's soft pads (TPU, friction 1)
SHELL_LINKS = ("trunk_base", "jaw_soft")  # bodies whose convex shell collides (housings inside)
SHELL_FRICTION = 0.5  # PLA shell on a floor
PAD_FRICTION = 1.0
MESH_BUDGET = 0.08  # triangles kept per part (vertex clustering, tools/mesh_simplify.py)
THIN_PARTS = ("shell", "face_part")  # printed shells: walls of about 1.5 mm
THIN_CELL = 0.001  # m: clustering cells on them stay below the wall thickness


def part_world(v, link, vis):
    """Part vertices (STL, metres) -> world, home pose (before the lift)."""
    import numpy as np
    R = np.array(link["R"])
    return np.array(link["origin"]) + (R @ (np.array(vis["pos"])[:, None] + np.array(vis["R"]) @ v.T)).T


def jaw_hinge(jaw_world):
    """The mouth hinge: the jaw's ears are discs on the hinge axis (world y in the home pose). The
    axis passes through their centres: the top of each ear and the chord 2.6 mm below it give the
    radius (r = (c^2/4 + h^2) / 2h) and so the centre."""
    import numpy as np
    centres = []
    for side in (1, -1):
        ear = jaw_world[jaw_world[:, 1] * side > 0.035]
        top = ear[:, 2].max()
        h = 0.0026
        band = ear[ear[:, 2] > top - h]
        chord = band[:, 0].max() - band[:, 0].min()
        r = (chord * chord / 4 + h * h) / (2 * h)
        centres.append([(band[:, 0].max() + band[:, 0].min()) / 2, side * np.abs(ear[:, 1]).mean(), top - r])
    c = np.mean(centres, axis=0)
    c[1] = 0.0
    return c, r


def build_visuals(links, mesh_cache):
    """Splits the mouth off the head, then gathers each link's parts in its own frame (origin at the
    joint, world-aligned axes, home pose). Returns (meshes, mouth link)."""
    import numpy as np
    import mesh_simplify as ms
    head = next(l for l in links if l["name"] == "jaw_soft")
    def load(vis):
        if vis["path"] not in mesh_cache:
            mesh_cache[vis["path"]] = ms.weld(ms.read_stl(vis["path"]))
        return mesh_cache[vis["path"]]
    # The mouth: pivot, mass properties (removed from the head's lumped inertial).
    jaw = next(v for v in head["visuals"] if v["mesh"] == "jaw")
    pivot, ear_r = jaw_hinge(part_world(load(jaw)[0], head, jaw))
    Rh = np.array(head["R"])
    m_tot, c_acc, I_parts = 0.0, np.zeros(3), []
    for vis in head["visuals"]:
        if vis["mesh"] in MOUTH_PARTS:
            v, f = load(vis)
            m, c, I = ms.mass_properties(part_world(v, head, vis), f, MOUTH_PARTS[vis["mesh"]])
            m_tot += m
            c_acc += m * c
            I_parts.append((m, c, I))
    com_w = c_acc / m_tot
    def about(m, c, I, p):  # parallel axis: inertia about point p
        d = c - p
        return I + m * (np.dot(d, d) * np.eye(3) - np.outer(d, d))
    I_mouth = sum(about(m, c, I, com_w) for m, c, I in I_parts)
    # head inertial in world terms, minus the mouth
    hm = head["mass"]
    hc = np.array(head["origin"]) + Rh @ np.array(head["ipos"])
    hI = Rh @ np.array(head["I"]) @ Rh.T
    rest_m = hm - m_tot
    rest_c = (hm * hc - m_tot * com_w) / rest_m
    rest_I = about(hm, hc, hI, rest_c) - about(m_tot, com_w, I_mouth, rest_c)
    head["mass"] = rest_m
    head["ipos"] = list(Rh.T @ (rest_c - np.array(head["origin"])))
    head["I"] = (Rh.T @ rest_I @ Rh).tolist()
    mouth = {"name": "mouth", "joint": ("mouth", MOUTH_RANGE[0], MOUTH_RANGE[1]), "parent": links.index(head),
             "origin": list(pivot), "R": head["R"], "axis_world": [0.0, 1.0, 0.0],
             "mass": m_tot, "ipos": list(Rh.T @ (com_w - pivot)), "I": (Rh.T @ I_mouth @ Rh).tolist(),
             "boxes": [], "sites": [], "visuals": [dict(v, frame=head) for v in head["visuals"] if v["mesh"] in MOUTH_PARTS], "ear_r": ear_r}
    head["visuals"] = [v for v in head["visuals"] if v["mesh"] not in MOUTH_PARTS]
    return mouth


def link_mesh(link, mesh_cache, lift):
    """One merged, simplified, crease-shaded mesh for a link, in its frame (vertex colours =
    the parts' MJCF materials, sRGB). Also the pads' boxes (grip) in the link frame."""
    import numpy as np
    import mesh_simplify as ms
    P, N, C, T, pads = [], [], [], [], []
    origin = np.array(link["origin"])
    for vis in link["visuals"]:
        if vis["path"] not in mesh_cache:
            mesh_cache[vis["path"]] = ms.weld(ms.read_stl(vis["path"]))
        v, f = mesh_cache[vis["path"]]
        w = part_world(v, vis.get("frame", link), vis) - origin
        thin = any(k in vis["mesh"] for k in THIN_PARTS)
        v2, f2 = ms.simplify(w, f, max(120, int(len(f) * MESH_BUDGET)), THIN_CELL if thin else None)
        p, n, t = ms.crease_normals(v2, f2)
        T.append(t + sum(len(x) for x in P))
        P.append(p)
        N.append(n)
        C.append(np.tile(vis["rgba"][:3], (len(p), 1)))  # sRGB (the renderer decodes vertex colours)
        if vis["mesh"] in GRIP_PADS:
            pads.append({"hull": ms.hull_points(w, 40)})
    P, N, C, T = np.concatenate(P), np.concatenate(N), np.concatenate(C), np.concatenate(T)
    return P, N, C, T, ms.hull_points(P), pads



def varint(b, i):
    r = s = 0
    while True:
        x = b[i]
        i += 1
        r |= (x & 0x7F) << s
        s += 7
        if x < 0x80:
            return r, i


def fields(b):
    i, out = 0, []
    while i < len(b):
        k, i = varint(b, i)
        f, t = k >> 3, k & 7
        if t == 0:
            v, i = varint(b, i)
        elif t == 1:
            v, i = b[i:i + 8], i + 8
        elif t == 5:
            v, i = b[i:i + 4], i + 4
        elif t == 2:
            n, i = varint(b, i)
            v, i = b[i:i + n], i + n
        else:
            raise ValueError("protobuf wire type %d" % t)
        out.append((f, t, v))
    return out


def load_policy(path):
    model = fields(open(path, "rb").read())
    graph = fields([v for f, t, v in model if f == 7][0])
    meta = {}
    for f, t, v in model:
        if f == 14:
            kv = {ff: vv.decode() for ff, tt, vv in fields(v)}
            meta[kv[1]] = kv.get(2, "")
    inits = {}
    for f, t, v in graph:
        if f != 5:
            continue
        dims, name, raw, flt = [], "", None, None
        for ff, tt, vv in fields(v):
            if ff == 1:
                if tt == 2:
                    j = 0
                    while j < len(vv):
                        x, j = varint(vv, j)
                        dims.append(x)
                else:
                    dims.append(vv)
            elif ff == 8:
                name = vv.decode()
            elif ff == 9:
                raw = vv
            elif ff == 4 and tt == 2:
                flt = list(struct.unpack("<%df" % (len(vv) // 4), vv))
        if raw is not None:
            flt = list(struct.unpack("<%df" % (len(raw) // 4), raw))
        inits[name] = (dims, flt)
    ops = []
    for f, t, v in graph:
        if f == 1:
            node = {"in": [], "op": ""}
            for ff, tt, vv in fields(v):
                if ff == 1:
                    node["in"].append(vv.decode())
                elif ff == 4:
                    node["op"] = vv.decode()
            ops.append(node)
    # The exported rsl_rl actor: Sub(mean) Div(std+eps) then Gemm/Elu x3 and a final Gemm.
    assert [n["op"] for n in ops] == ["Sub", "Div", "Gemm", "Elu", "Gemm", "Elu", "Gemm", "Elu", "Gemm"], [n["op"] for n in ops]
    mean = inits[ops[0]["in"][1]][1]
    div = inits[ops[1]["in"][1]][1]
    layers = []
    for n in ops:
        if n["op"] == "Gemm":
            (wd, w), (bd, b) = inits[n["in"][1]], inits[n["in"][2]]
            layers.append((wd, w, b))
    return meta, mean, div, layers

# ---------------------------------------------------------------- output

def hexf(x):
    """Exact float32 as a C hex literal, trailing zero digits dropped (0x1.645a1cp-4f)."""
    h = float.hex(struct.unpack("<f", struct.pack("<f", x))[0])
    mant, exp = h.split("p")
    if "." in mant:
        mant = mant.rstrip("0").rstrip(".")
    return mant + "p" + exp + "f" if mant not in ("0x0", "-0x0") else mant + ".0p+0f"

def cf(x):
    return ("%.9g" % x).replace("e-0", "e-") + "f" if "." in "%.9g" % x or "e" in "%.9g" % x else "%.9g.0f" % x

def vec(v):
    return "{%s}" % ", ".join(cf(x) for x in v)


def euler_of(R):
    """Kern's Euler degrees (R = Rz(yaw) Ry(pitch) Rx(roll)); None near gimbal lock."""
    if abs(R[2][0]) > 0.999:
        return None
    pitch = math.asin(-R[2][0])
    roll = math.atan2(R[2][1], R[2][2])
    yaw = math.atan2(R[1][0], R[0][0])
    return [math.degrees(roll), math.degrees(pitch), math.degrees(yaw)]


def signed_permutations():
    import itertools
    for perm in itertools.permutations(range(3)):
        for signs in itertools.product((1, -1), repeat=3):
            P = [[0] * 3 for _ in range(3)]
            for i in range(3):
                P[perm[i]][i] = signs[i]
            det = (P[0][0] * (P[1][1] * P[2][2] - P[1][2] * P[2][1]) - P[0][1] * (P[1][0] * P[2][2] - P[1][2] * P[2][0])
                   + P[0][2] * (P[1][0] * P[2][1] - P[1][1] * P[2][0]))
            if det == 1:
                yield P, perm


def box_rotation(R, size):
    """A box is symmetric under signed axis permutations: pick one whose Euler angles are regular."""
    best = None
    for P, perm in signed_permutations():
        R2 = mmul(R, P)
        e = euler_of(R2)
        if e is None:
            continue
        s2 = [size[perm[i]] for i in range(3)]
        score = sum(abs(a) for a in e)
        if best is None or score < best[0]:
            best = (score, e, s2)
    return best[1], best[2]


def export_rollers(xml_path, assets, links, order, remap, lift, w, mesh_cache, meshes):
    """The roller skates (pollen-robotics/microduck_rl robot_groundcontact_rollers_backlash.xml, what
    roller.onnx and roller_crouch.onnx train on): the ankles carry a blade instead of the foot, and
    two free wheels hang off each. Everything above the ankles is the walking model."""
    import numpy as np
    import mesh_simplify as ms
    rl = load_robot(xml_path, assets)
    by = {l["name"]: l for l in rl}
    ankle_of = {"ankle_l_v1": "left_ankle", "ankle_r_v1": "right_ankle"}
    joint_link = {links[i]["joint"][0]: remap[i] for i in order if links[i]["joint"]}
    # Lowest wheel point vs the shoes' sole: how much higher the rollers stand.
    lowest = 1e9
    for l in rl:
        for b in l["boxes"]:
            if b["path"].endswith(TIRE_MESH + ".stl"):
                if b["path"] not in mesh_cache:
                    mesh_cache[b["path"]] = ms.weld(ms.read_stl(b["path"]))
                lowest = min(lowest, float(part_world(mesh_cache[b["path"]][0], l, {"pos": b["gpos"], "R": b["R"]})[:, 2].min()))
    raise_by = -lowest - lift
    w("// Roller skates: the ankles' blades and the four free wheels (rollers model). The whole robot")
    w("// stands this much higher on them (metres, real):")
    w("constexpr float kDuckRollerRaise = %s;" % cf(raise_by))
    w("const DuckRollerDef kDuckRollers[] = {")
    hull_lines = []
    flat = []
    for name in ("ankle_l_v1", "ankle_r_v1"):
        a = by[name]
        li = joint_link[ankle_of[name]]
        Iw = mmul(mmul(a["R"], a["I"]), mT(a["R"]))
        P, N, C, T, H, _ = link_mesh(a, mesh_cache, lift)
        meshes.append(("roller/" + name, (P, N, C, T, H)))
        w('    {"%s", "", %d, -1, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, %s, %s, %s, 0, 0},' % (
            name, li, cf(a["mass"]), vec(mvec(a["R"], a["ipos"])), vec([Iw[0][0], Iw[1][1], Iw[2][2], Iw[0][1], Iw[0][2], Iw[1][2]])))
        for t in rl:
            if t["parent"] != rl.index(a) or not t["joint"]:
                continue
            Iw = mmul(mmul(t["R"], t["I"]), mT(t["R"]))
            P, N, C, T, H, _ = link_mesh(t, mesh_cache, lift)
            meshes.append(("roller/" + t["joint"][0], (P, N, C, T, H)))
            rel = [t["origin"][k] - a["origin"][k] for k in range(3)]
            axis = [t["R"][0][2], t["R"][1][2], t["R"][2][2]]
            box = t["boxes"][0]
            if box["path"] not in mesh_cache:
                mesh_cache[box["path"]] = ms.weld(ms.read_stl(box["path"]))
            hp = ms.hull_points(part_world(mesh_cache[box["path"]][0], t, {"pos": box["gpos"], "R": box["R"]}) - np.array(t["origin"]), 40)
            w('    {"%s", "%s", %d, %d, %s, %s, %s, %s, %s, %d, %d},' % (
                t["joint"][0], t["joint"][0], li, 0, vec(rel), vec(axis), cf(t["mass"]), vec(mvec(t["R"], t["ipos"])),
                vec([Iw[0][0], Iw[1][1], Iw[2][2], Iw[0][1], Iw[0][2], Iw[1][2]]), len(flat) // 3, len(hp)))
            flat += [x for q in hp for x in q]
    w("};")
    w("const float kDuckRollerHullPoints[] = {")
    for k in range(0, len(flat), 12):
        w("    " + ", ".join(cf(x) for x in flat[k:k + 12]) + ",")
    w("};")


def write_meshes(path, meshes):
    """The links' meshes as C++ arrays: positions quantised to 16 bits over the mesh bounds, normals
    and sRGB colours to 8 bits, 16-bit indices, hull points as floats (metres, real size)."""
    import numpy as np
    L = ["// Generated by tools/micro_duck_import.py from Pollen Robotics' Micro Duck meshes",
         "// (github.com/pollen-robotics/microduck_rl, src/mjlab_microduck/robot/microduck/assets/*.stl).",
         "// Copyright Pollen Robotics. Licensed under CC BY-NC-SA 4.0",
         "// (https://creativecommons.org/licenses/by-nc-sa/4.0/). Modified: simplified (vertex clustering",
         "// with quadric representatives), merged per link, posed in the STAND keyframe, normals recomputed",
         "// with a crease angle, quantised. Not for commercial use; adaptations under the same licence.",
         "// Do not edit by hand."]
    table = []
    for name, (P, N, C, T, H) in meshes:
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
        table.append('    {"duck/%s", %s, %s, %d, %d, %s_pos, %s_nrm, %s_col, %s_idx, %d, %s_hull},' % (
            name, vec(lo), vec(hi), len(P), T.size, ident, ident, ident, ident, len(H), ident))
    L.append("const DuckMeshDef kDuckMeshes[] = {")
    L += table
    L.append("};")
    open(path, "w").write("\n".join(L) + "\n")


def main():
    xml_path, assets, onnx_path, out = sys.argv[1:5]
    meshes_path = sys.argv[5] if len(sys.argv) > 5 else None
    links = load_robot(xml_path, assets)
    mesh_cache = {}
    links.append(build_visuals(links, mesh_cache))
    by_joint = {l["joint"][0]: i for i, l in enumerate(links) if l["joint"]}
    order = [0] + [by_joint[n] for n in POLICY_ORDER] + [len(links) - 1]  # the mouth last
    remap = {old: new for new, old in enumerate(order)}
    # Lowest collider point in the home pose (the meshes, as MuJoCo collides them) -> the trunk
    # height that puts the soles on the ground.
    import mesh_simplify as ms
    lowest = 1e9
    for l in links:
        for b in l["boxes"]:
            if b["path"] not in mesh_cache:
                mesh_cache[b["path"]] = ms.weld(ms.read_stl(b["path"]))
            pw = part_world(mesh_cache[b["path"]][0], l, {"pos": b["gpos"], "R": b["R"]})
            lowest = min(lowest, float(pw[:, 2].min()))
    lift = -lowest
    lines = []
    w = lines.append
    w("// Generated by tools/micro_duck_import.py from Pollen Robotics' microduck_rl")
    w("// (robot_allcollisions_backlash.xml, Apache-2.0). Do not edit by hand.")
    w("// Real size (metres, kilograms), posed in the STAND keyframe with the soles on the ground.")
    w("// Frames are world-aligned; link order = trunk, then the policy's 14 joints.")
    w("const DuckLinkDef kDuckLinks[] = {")
    for i in order:
        l = links[i]
        origin = [l["origin"][0], l["origin"][1], l["origin"][2] + lift]
        axis = l.get("axis_world") or [l["R"][0][2], l["R"][1][2], l["R"][2][2]]
        com = mvec(l["R"], l["ipos"])  # world-aligned, relative to the origin
        Iw = mmul(mmul(l["R"], l["I"]), mT(l["R"]))
        inertia = [Iw[0][0], Iw[1][1], Iw[2][2], Iw[0][1], Iw[0][2], Iw[1][2]]
        if l["joint"]:
            jn, lo, hi = l["joint"]
            home = HOME.get(jn, 0.0)  # the mouth: its CAD pose
        else:
            jn, lo, hi, home = "", 0, 0, 0
        parent = remap[l["parent"]] if l["parent"] >= 0 else -1
        w('    {"%s", "%s", %d, %s, %s, %s, %s, %s, %s, %s, %s},' % (
            l["name"], jn, parent, vec(origin), vec(axis), cf(lo), cf(hi), cf(home), cf(l["mass"]), vec(com), vec(inertia)))
    w("};")
    meshes, pads = [], []
    for i in order:
        P, N, C, T, H, link_pads = link_mesh(links[i], mesh_cache, lift)
        meshes.append((links[i]["name"], (P, N, C, T, H)))
        pads += [(remap[i], p) for p in link_pads]
    w("// Colliders, as MuJoCo collides mesh geoms: the convex hull of each mesh. The soles (friction 1)")
    w("// and the XL330 housings (frictionless) as in training, then the beak's soft pads (TPU, friction")
    w("// %g): what grips. Points relative to the link's origin, world-aligned, home pose." % PAD_FRICTION)
    import numpy as np
    import mesh_simplify as ms
    hulls = []
    for i in order:
        l = links[i]
        for b in l["boxes"]:
            if b["path"] not in mesh_cache:
                mesh_cache[b["path"]] = ms.weld(ms.read_stl(b["path"]))
            v = mesh_cache[b["path"]][0]
            pw = part_world(v, l, {"pos": b["gpos"], "R": b["R"]}) - np.array(l["origin"])
            hulls.append((remap[i], 1.0 if b["sole"] else 0.0, b["sole"], ms.hull_points(pw, 40)))
    for li, p in pads:
        hulls.append((li, PAD_FRICTION, False, p["hull"]))
    flat = []
    w("const DuckHullDef kDuckHulls[] = {")
    for li, fr, sole, H in hulls:
        w("    {%d, %s, %s, %d, %d}," % (li, cf(fr), "true" if sole else "false", len(flat) // 3, len(H)))
        flat += [x for p in H for x in p]
    w("};")
    w("const float kDuckHullPoints[] = {")
    for k in range(0, len(flat), 12):
        w("    " + ", ".join(cf(x) for x in flat[k:k + 12]) + ",")
    w("};")
    rollers_xml = os.path.join(os.path.dirname(xml_path), "robot_groundcontact_rollers_backlash.xml")
    if os.path.exists(rollers_xml):
        export_rollers(rollers_xml, assets, links, order, remap, lift, w, mesh_cache, meshes)
    w("// Links whose convex shell collides (the shell encloses the servos; PLA on a floor).")
    w("constexpr const char* kDuckShellBodies[] = {%s};" % ", ".join('"%s"' % n for n in SHELL_LINKS))
    w("constexpr float kDuckShellFriction = %s;" % cf(SHELL_FRICTION))
    if meshes_path:
        write_meshes(meshes_path, meshes)
    w("// Sensor sites: position relative to the link's origin and the site frame's axes, world-aligned in")
    w("// the home pose (MJCF site frames; a MuJoCo camera looks along -Z with +Y up).")
    w("const DuckSiteDef kDuckSites[] = {")
    for i in order:
        l = links[i]
        for st in l["sites"]:
            Rw = mmul(l["R"], st["R"])
            pw = mvec(l["R"], st["pos"])
            col = lambda k: [Rw[0][k], Rw[1][k], Rw[2][k]]
            w('    {"%s", %d, %s, %s, %s, %s},' % (st["name"], remap[i], vec(pw), vec(col(0)), vec(col(1)), vec(col(2))))
    w("};")
    open(os.path.join(out, "micro_duck_model.inc"), "w").write("\n".join(lines) + "\n")

    meta, mean, div, layers = load_policy(onnx_path)
    names = meta.get("joint_names", "").split(",")
    assert names == POLICY_ORDER, names
    default = [float(x) for x in meta["default_joint_pos"].split(",")]
    for n, d in zip(POLICY_ORDER, default):
        assert abs(HOME[n] - d) < 1e-3, (n, d)
    assert meta.get("observation_names", "").split(",") == [
        "base_ang_vel", "projected_gravity", "joint_pos", "joint_vel", "actions", "command", "head_command", "body_command"]
    assert float(meta.get("action_scale", "1")) == 1.0
    p = []
    w = p.append
    w("// Generated by tools/micro_duck_import.py from velstand.onnx (Pollen Robotics,")
    w("// huggingface.co/pollen-robotics/microduck-policies, Apache-2.0). Do not edit by hand.")
    for k in sorted(meta):
        w("// %s: %s" % (k, meta[k][:200].replace("\n", " ")))
    dims = [len(mean)] + [l[0][0] for l in layers]
    w("constexpr int kVelstandDims[] = {%s};" % ", ".join(str(d) for d in dims))
    w("constexpr float kVelstandDefault[] = {%s};" % ", ".join(hexf(x) for x in default))

    def arr(name, values):
        w("alignas(16) constexpr float %s[] = {" % name)
        for k in range(0, len(values), 8):
            w("    " + ", ".join(hexf(x) for x in values[k:k + 8]) + ",")
        w("};")
    arr("kVelstandMean", mean)
    arr("kVelstandDiv", div)
    for li, (wd, wv, bv) in enumerate(layers):
        arr("kVelstandW%d" % li, wv)  # [out][in], Gemm transB=1
        arr("kVelstandB%d" % li, bv)
    open(os.path.join(out, "micro_duck_velstand.inc"), "w").write("\n".join(p) + "\n")
    print("lift %.5f m, %d links, %d boxes, policy %s" % (lift, len(order), sum(len(l["boxes"]) for l in links), dims))


if __name__ == "__main__":
    main()
