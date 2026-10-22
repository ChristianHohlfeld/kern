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
SOLES = {"left_foot_collision", "right_foot_collision"}


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

    def walk(body, parent, p_pos, p_R):
        pos = floats(body.get("pos", "0 0 0"), 3)
        quat = floats(body.get("quat", "1 0 0 0"), 4)
        origin = add(p_pos, mvec(p_R, pos))
        R = mmul(p_R, qmat(quat))
        joint = None
        for j in body.findall("joint"):
            name = j.get("name")
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
            if not sole and mesh != SERVO_MESH:
                continue  # not colliding in training (CollisionCfg disables unnamed geoms)
            c, size = stl_extent(meshes[mesh])
            gR = qmat(floats(g.get("quat", "1 0 0 0"), 4))
            gpos = floats(g.get("pos", "0 0 0"), 3)
            # body-local centre and orientation of the box, then the joint's home rotation
            local_c = add(gpos, mvec(gR, c))
            boxes.append({"c": local_c, "R": gR, "size": size, "sole": sole})
        index = len(links)
        links.append({"name": body.get("name"), "joint": joint, "parent": parent, "origin": origin, "R": R,
                      "mass": mass, "ipos": ipos, "I": I, "boxes": boxes})
        for child in body.findall("body"):
            walk(child, index, origin, R)

    trunk = root.find("worldbody").find("body")
    walk(trunk, -1, [0, 0, 0], [[1, 0, 0], [0, 1, 0], [0, 0, 1]])
    return links

# ---------------------------------------------------------------- ONNX (protobuf wire format)

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


def main():
    xml_path, assets, onnx_path, out = sys.argv[1:5]
    links = load_robot(xml_path, assets)
    by_joint = {l["joint"][0]: i for i, l in enumerate(links) if l["joint"]}
    order = [0] + [by_joint[n] for n in POLICY_ORDER]
    remap = {old: new for new, old in enumerate(order)}
    # Lowest collider corner in the home pose -> trunk height that puts the soles on the ground.
    lowest = 1e9
    for l in links:
        for b in l["boxes"]:
            Rw = mmul(l["R"], b["R"])
            cw = add(l["origin"], mvec(l["R"], b["c"]))
            for sx in (-0.5, 0.5):
                for sy in (-0.5, 0.5):
                    for sz in (-0.5, 0.5):
                        p = add(cw, mvec(Rw, [sx * b["size"][0], sy * b["size"][1], sz * b["size"][2]]))
                        lowest = min(lowest, p[2])
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
        axis = [l["R"][0][2], l["R"][1][2], l["R"][2][2]]
        com = mvec(l["R"], l["ipos"])  # world-aligned, relative to the origin
        Iw = mmul(mmul(l["R"], l["I"]), mT(l["R"]))
        inertia = [Iw[0][0], Iw[1][1], Iw[2][2], Iw[0][1], Iw[0][2], Iw[1][2]]
        if l["joint"]:
            jn, lo, hi = l["joint"]
            home = HOME[jn]
        else:
            jn, lo, hi, home = "", 0, 0, 0
        parent = remap[l["parent"]] if l["parent"] >= 0 else -1
        w('    {"%s", "%s", %d, %s, %s, %s, %s, %s, %s, %s, %s},' % (
            l["name"], jn, parent, vec(origin), vec(axis), cf(lo), cf(hi), cf(home), cf(l["mass"]), vec(com), vec(inertia)))
    w("};")
    w("const DuckBoxDef kDuckBoxes[] = {")
    for i in order:
        l = links[i]
        for b in l["boxes"]:
            Rw = mmul(l["R"], b["R"])
            cw = mvec(l["R"], b["c"])
            e, s = box_rotation(Rw, b["size"])
            w("    {%d, %s, %s, %s, %s}," % (remap[i], vec(cw), vec(e), vec(s), "true" if b["sole"] else "false"))
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
