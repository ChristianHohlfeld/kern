// Soldier art after SpawnCity (see soldier_models.h). Numbers are SpawnCity's
// (sim/character_model.mjs, sim/weapon_models.mjs, player_walk_mode_v4.mjs,
// walk_world_experience_hotfix.mjs); colours are sRGB hex as there.
#include "soldier_models.h"
#include "kern/core/dmath.h"

#include <algorithm>
#include <cmath>

#include "kern/geo/earcut.h"

namespace kern::game::models {

namespace {

constexpr f32 kPi = 3.14159265f;

Vec3 rgb(u32 c) { return {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f}; }

Mat4 rx(f32 a) { Mat4 m; const f32 c = kern::dm::cos(a), s = kern::dm::sin(a); m.m[5] = c; m.m[6] = s; m.m[9] = -s; m.m[10] = c; return m; }
Mat4 ry(f32 a) { Mat4 m; const f32 c = kern::dm::cos(a), s = kern::dm::sin(a); m.m[0] = c; m.m[2] = -s; m.m[8] = s; m.m[10] = c; return m; }
Mat4 rz(f32 a) { Mat4 m; const f32 c = kern::dm::cos(a), s = kern::dm::sin(a); m.m[0] = c; m.m[1] = s; m.m[4] = -s; m.m[5] = c; return m; }


}  // namespace

Mat4 euler(Vec3 r) { return rx(r.x) * ry(r.y) * rz(r.z); }

// ---------------------------------------------------------------------------
// Builder

void Builder::material(u32 c, f32 metalness, bool glow) {
  color_ = rgb(c);
  surface_ = glow ? Surface::glow : metalness >= 0.5f ? Surface::metal : Surface::matte;
}

Mat4 Builder::part(Vec3 pos, Vec3 rot, Vec3 scale) const { return xf_ * Mat4::translate(pos) * euler(rot) * Mat4::scale(scale); }

void Builder::tri(Vec3 a, Vec3 b, Vec3 c, Vec3 na, Vec3 nb, Vec3 nc) {
  // Normals follow the cofactor matrix (inverse transpose up to scale): non-uniform part
  // scales stretch spheres and capsules correctly; mirrored transforms flip it back.
  const f32* m = cur_.m;
  const f32 a00 = m[0], a01 = m[4], a02 = m[8], a10 = m[1], a11 = m[5], a12 = m[9], a20 = m[2], a21 = m[6], a22 = m[10];
  const f32 c00 = a11 * a22 - a12 * a21, c01 = -(a10 * a22 - a12 * a20), c02 = a10 * a21 - a11 * a20;
  const f32 c10 = -(a01 * a22 - a02 * a21), c11 = a00 * a22 - a02 * a20, c12 = -(a00 * a21 - a01 * a20);
  const f32 c20 = a01 * a12 - a02 * a11, c21 = -(a00 * a12 - a02 * a10), c22 = a00 * a11 - a01 * a10;
  const f32 det = a00 * c00 + a01 * c01 + a02 * c02, sg = det < 0 ? -1.0f : 1.0f;
  auto nrm = [&](Vec3 n) {
    return normalize(Vec3{c00 * n.x + c01 * n.y + c02 * n.z, c10 * n.x + c11 * n.y + c12 * n.z, c20 * n.x + c21 * n.y + c22 * n.z} * sg, {0, 0, 1});
  };
  MeshData* d = &out_.matte;
  std::vector<f32>* bones = &out_.matte_bones;
  if (surface_ == Surface::metal) { d = &out_.metal; bones = &out_.metal_bones; }
  if (surface_ == Surface::glow) { d = &out_.glow; bones = &out_.glow_bones; }
  Vec3 pa = cur_.transform_point(a), pb = cur_.transform_point(b), pc = cur_.transform_point(c);
  Vec3 ma = nrm(na), mb = nrm(nb), mc = nrm(nc);
  const Vec3 g = cross(pb - pa, pc - pa);
  if (dot(g, g) < 1e-20f) return;  // degenerate (poles)
  // Counter-clockwise seen from outside: orient by the given normals.
  if (dot(g, ma + mb + mc) < 0) { std::swap(pb, pc); std::swap(mb, mc); }
  d->push(pa, ma, color_, {0, 0});
  d->push(pb, mb, color_, {0, 0});
  d->push(pc, mc, color_, {0, 0});
  bones->insert(bones->end(), {static_cast<f32>(bone_), static_cast<f32>(bone_), static_cast<f32>(bone_)});
}

void Builder::flat(Vec3 a, Vec3 b, Vec3 c, Vec3 outward) {
  Vec3 n = normalize(cross(b - a, c - a), outward);
  if (dot(n, outward) < 0) n = n * -1.0f;
  tri(a, b, c, n, n, n);
}

void Builder::box(Vec3 s, Vec3 pos, Vec3 rot, Vec3 scale) {
  cur_ = part(pos, rot, scale);
  const Vec3 h = s * 0.5f;
  const Vec3 p[8] = {{-h.x, -h.y, -h.z}, {h.x, -h.y, -h.z}, {h.x, h.y, -h.z}, {-h.x, h.y, -h.z},
                     {-h.x, -h.y, h.z},  {h.x, -h.y, h.z},  {h.x, h.y, h.z},  {-h.x, h.y, h.z}};
  const int f[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {3, 0, 4, 7}};
  const Vec3 out[6] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {1, 0, 0}, {-1, 0, 0}};
  for (int i = 0; i < 6; ++i) {
    const auto& q = f[i];
    flat(p[q[0]], p[q[1]], p[q[2]], out[i]);
    flat(p[q[0]], p[q[2]], p[q[3]], out[i]);
  }
}

void Builder::cylinder(f32 top, f32 bottom, f32 len, u32 seg, Vec3 pos, Vec3 rot, Vec3 scale) {
  cur_ = part(pos, rot, scale);
  seg = max_of(3u, seg);
  const f32 h = len * 0.5f;
  const f32 slope = (bottom - top) / max_of(1e-6f, len);
  for (u32 i = 0; i < seg; ++i) {
    // Three.js: theta from +Z toward +X (x = sin, z = cos).
    const f32 a0 = 2 * kPi * i / seg, a1 = 2 * kPi * (i + 1) / seg;
    const Vec3 d0{kern::dm::sin(a0), 0, kern::dm::cos(a0)}, d1{kern::dm::sin(a1), 0, kern::dm::cos(a1)};
    const Vec3 t0 = d0 * top + Vec3{0, h, 0}, t1 = d1 * top + Vec3{0, h, 0};
    const Vec3 b0 = d0 * bottom + Vec3{0, -h, 0}, b1 = d1 * bottom + Vec3{0, -h, 0};
    const Vec3 n0 = normalize(d0 + Vec3{0, slope, 0}), n1 = normalize(d1 + Vec3{0, slope, 0});
    tri(t0, b0, b1, n0, n0, n1);
    tri(t0, b1, t1, n0, n1, n1);
    if (top > 0) tri(Vec3{0, h, 0}, t0, t1, {0, 1, 0}, {0, 1, 0}, {0, 1, 0});
    if (bottom > 0) tri(Vec3{0, -h, 0}, b1, b0, {0, -1, 0}, {0, -1, 0}, {0, -1, 0});
  }
}

void Builder::capsule(f32 r, f32 len, u32 seg, Vec3 pos, Vec3 rot, Vec3 scale) {
  cur_ = part(pos, rot, scale);
  seg = max_of(4u, seg);
  // Lathe profile: (radius, y, normal y) from the top pole to the bottom pole.
  std::vector<Vec3> prof;
  const u32 cap = 4;
  for (u32 k = 0; k <= cap; ++k) {
    const f32 a = 0.5f * kPi * k / cap;  // 0 at the pole
    prof.push_back({r * kern::dm::sin(a), len * 0.5f + r * kern::dm::cos(a), kern::dm::cos(a)});
  }
  for (u32 k = 0; k <= cap; ++k) {
    const f32 a = 0.5f * kPi + 0.5f * kPi * k / cap;
    prof.push_back({r * kern::dm::sin(a), -len * 0.5f + r * kern::dm::cos(a), kern::dm::cos(a)});
  }
  for (std::size_t k = 0; k + 1 < prof.size(); ++k) {
    const Vec3 u = prof[k], v = prof[k + 1];
    const f32 su = std::sqrt(max_of(0.0f, 1 - u.z * u.z)), sv = std::sqrt(max_of(0.0f, 1 - v.z * v.z));
    for (u32 i = 0; i < seg; ++i) {
      const f32 a0 = 2 * kPi * i / seg, a1 = 2 * kPi * (i + 1) / seg;
      const Vec3 d0{kern::dm::sin(a0), 0, kern::dm::cos(a0)}, d1{kern::dm::sin(a1), 0, kern::dm::cos(a1)};
      const Vec3 p00 = d0 * u.x + Vec3{0, u.y, 0}, p01 = d1 * u.x + Vec3{0, u.y, 0};
      const Vec3 p10 = d0 * v.x + Vec3{0, v.y, 0}, p11 = d1 * v.x + Vec3{0, v.y, 0};
      const Vec3 n00 = d0 * su + Vec3{0, u.z, 0}, n01 = d1 * su + Vec3{0, u.z, 0};
      const Vec3 n10 = d0 * sv + Vec3{0, v.z, 0}, n11 = d1 * sv + Vec3{0, v.z, 0};
      if (u.x > 1e-6f) tri(p00, p10, p11, n00, n10, n11);
      if (v.x > 1e-6f || u.x > 1e-6f) tri(p00, p11, p01, n00, n11, n01);
    }
  }
}

void Builder::sphere(f32 r, u32 seg, u32 rings, Vec3 pos, Vec3 rot, Vec3 scale, f32 theta_end) {
  cur_ = part(pos, rot, scale);
  seg = max_of(4u, seg);
  rings = max_of(2u, rings);
  auto pt = [&](u32 i, u32 k) {
    const f32 th = theta_end * k / rings, ph = 2 * kPi * i / seg;
    // Three.js SphereGeometry: x = -cos(phi) sin(theta), y = cos(theta), z = sin(phi) sin(theta)
    return Vec3{-kern::dm::cos(ph) * kern::dm::sin(th), kern::dm::cos(th), kern::dm::sin(ph) * kern::dm::sin(th)};
  };
  for (u32 k = 0; k < rings; ++k)
    for (u32 i = 0; i < seg; ++i) {
      const Vec3 a = pt(i, k), b = pt(i + 1, k), c = pt(i, k + 1), d = pt(i + 1, k + 1);
      if (k > 0) tri(a * r, b * r, d * r, a, b, d);
      tri(a * r, d * r, c * r, a, d, c);
    }
}

void Builder::circle(f32 r, u32 seg, Vec3 pos, Vec3 rot) {
  cur_ = part(pos, rot, {1, 1, 1});
  for (u32 i = 0; i < seg; ++i) {
    const f32 a0 = 2 * kPi * i / seg, a1 = 2 * kPi * (i + 1) / seg;
    flat({0, 0, 0}, {kern::dm::cos(a0) * r, kern::dm::sin(a0) * r, 0}, {kern::dm::cos(a1) * r, kern::dm::sin(a1) * r, 0}, {0, 0, 1});
  }
}

void Builder::profile(std::initializer_list<Vec2> points, f32 w, f32 bevel, Vec3 pos, Vec3 rot) {
  cur_ = part(pos, rot, {1, 1, 1});
  std::vector<Vec2> p(points);
  const std::size_t n = p.size();
  if (n < 3) return;
  f32 area = 0;
  for (std::size_t i = 0; i < n; ++i) area += p[i].x * p[(i + 1) % n].y - p[(i + 1) % n].x * p[i].y;
  const f32 orient = area >= 0 ? 1.0f : -1.0f;  // counter-clockwise: outward = right of the edge reversed
  // Outline grown by the bevel (miter of the two edge normals, clamped).
  std::vector<Vec2> grown(n);
  for (std::size_t i = 0; i < n; ++i) {
    const Vec2 a = p[(i + n - 1) % n], b = p[i], c = p[(i + 1) % n];
    auto out_normal = [&](Vec2 e0, Vec2 e1) {
      const Vec2 d{e1.x - e0.x, e1.y - e0.y};
      const f32 l = std::sqrt(d.x * d.x + d.y * d.y);
      return l > 1e-9f ? Vec2{d.y / l * orient, -d.x / l * orient} : Vec2{0, 0};
    };
    const Vec2 n0 = out_normal(a, b), n1 = out_normal(b, c);
    Vec2 m{n0.x + n1.x, n0.y + n1.y};
    const f32 ml = std::sqrt(m.x * m.x + m.y * m.y);
    if (ml > 1e-6f) { m.x /= ml; m.y /= ml; }
    const f32 cosh = max_of(0.35f, m.x * n0.x + m.y * n0.y);
    grown[i] = {b.x + m.x * bevel / cosh, b.y + m.y * bevel / cosh};
  }
  // shape (sx, sy) -> (x = extrusion, y = sy, z = -sx)
  auto at = [](Vec2 q, f32 x) { return Vec3{x, q.y, -q.x}; };
  const f32 hw = w * 0.5f, hi = max_of(0.0f, hw - bevel);
  const std::vector<Vec2>* rings[4] = {&p, &grown, &grown, &p};
  const f32 xs[4] = {-hw, -hi, hi, hw};
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t j = (i + 1) % n;
    // Outward direction of this edge in 3D (shape normal (nx, ny) -> (0, ny, -nx)).
    const Vec2 e{p[j].x - p[i].x, p[j].y - p[i].y};
    const f32 el = std::sqrt(e.x * e.x + e.y * e.y);
    if (el < 1e-9f) continue;
    const Vec2 on{e.y / el * orient, -e.x / el * orient};
    for (int r = 0; r < 3; ++r) {
      const Vec3 a = at((*rings[r])[i], xs[r]), b = at((*rings[r])[j], xs[r]), c = at((*rings[r + 1])[j], xs[r + 1]), d = at((*rings[r + 1])[i], xs[r + 1]);
      Vec3 out{0, on.y, -on.x};
      if (r == 0) out = normalize(out + Vec3{-1, 0, 0});
      if (r == 2) out = normalize(out + Vec3{1, 0, 0});
      flat(a, b, c, out);
      flat(a, c, d, out);
    }
  }
  // Caps (exact polygon).
  std::vector<f64> xy;
  for (const Vec2& q : p) xy.insert(xy.end(), {q.x, q.y});
  std::vector<u32> idx;
  Earcut ec;
  ec.triangulate(xy.data(), static_cast<u32>(n), nullptr, 0, idx);
  for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
    const Vec2 a = p[idx[t]], b = p[idx[t + 1]], c = p[idx[t + 2]];
    flat(at(a, hw), at(b, hw), at(c, hw), {1, 0, 0});
    flat(at(a, -hw), at(b, -hw), at(c, -hw), {-1, 0, 0});
  }
}

// ---------------------------------------------------------------------------
// weapons (weapon_models.mjs)

namespace {

/// Torus in the XY plane (radius, tube), facing Z.
void torus(Builder& b, f32 radius, f32 tube, Vec3 pos) {
  for (u32 i = 0; i < 20; ++i) {
    const f32 a = 2 * kPi * (i + 0.5f) / 20;
    b.box({tube * 2, 2 * kPi * radius / 20 * 1.05f, tube * 2}, pos + Vec3{kern::dm::cos(a) * radius, kern::dm::sin(a) * radius, 0}, {0, 0, a});
  }
}

void volt_smg(Builder& b, WeaponModel& w) {
  const u32 graphite = 0x2b2f36, panel = 0x7b4dff, gold = 0xf2b233, rubber = 0x15171b, cell = 0x4fe3ff, led = 0xb48bff;
  b.material(graphite, 0.35f);
  b.profile({{-.12f, .06f}, {.36f, .075f}, {.60f, .06f}, {.62f, -.02f}, {.38f, -.06f}, {.18f, -.06f}, {.12f, -.04f}, {-.02f, -.05f}, {-.12f, -.02f}}, .16f, .008f, {0, -.01f, 0});
  b.material(panel, 0.25f);
  b.profile({{.02f, .045f}, {.33f, .055f}, {.52f, .045f}, {.53f, -.01f}, {.34f, -.045f}, {.04f, -.04f}}, .172f, .004f, {0, -.01f, 0});
  b.material(rubber, 0);
  b.profile({{0, .075f}, {.42f, .085f}, {.44f, .105f}, {.40f, .11f}, {.06f, .10f}, {0, .09f}}, .09f, .004f);
  b.profile({{.10f, -.04f}, {.20f, -.04f}, {.17f, -.25f}, {.08f, -.26f}, {.065f, -.2f}}, .105f);
  b.material(graphite, 0.35f);
  b.profile({{.24f, -.05f}, {.33f, -.05f}, {.36f, -.24f}, {.27f, -.26f}}, .09f);
  b.material(gold, 0.85f);
  b.profile({{.27f, -.25f}, {.365f, -.235f}, {.37f, -.27f}, {.27f, -.285f}}, .10f, .004f);
  b.material(rubber, 0);
  b.profile({{-.12f, .04f}, {-.04f, .04f}, {-.04f, -.02f}, {-.16f, -.06f}, {-.24f, -.06f}, {-.24f, 0}}, .12f);
  b.material(graphite, 0.35f);
  b.cylinder(.026f, .026f, .24f, 16, {0, .02f, -.72f}, {kPi / 2, 0, 0});
  b.material(rubber, 0);
  b.cylinder(.036f, .032f, .07f, 8, {0, .02f, -.86f}, {kPi / 2, 0, 0});
  b.material(gold, 0.85f);
  b.cylinder(.039f, .039f, .014f, 16, {0, .02f, -.825f}, {kPi / 2, 0, 0});
  b.material(cell, 0, true);
  b.box({.176f, .012f, .22f}, {0, .018f, -.28f});
  b.material(led, 0, true);
  b.box({.04f, .018f, .05f}, {0, .112f, -.14f});
  b.material(rubber, 0);
  b.box({.06f, .03f, .03f}, {0, .13f, -.10f});
  b.box({.03f, .035f, .025f}, {0, .13f, -.40f});
  b.material(cell, 0, true);
  b.sphere(.007f, 8, 6, {0, .152f, -.405f});
  w.grip = {0, -.15f, -.15f};
  w.rear_sight = {0, .152f, -.10f};
  w.front_sight = {0, .152f, -.405f};
  w.muzzle = {0, .02f, -.90f};
  w.eject = {.08f, .05f, -.2f};
  w.flash_scale = 1;
  w.flash_ms = 34;
  w.mount = {.012f, -.008f, .015f};
}

void glock(Builder& b, Builder& s, WeaponModel& w) {
  const u32 slide = 0x2b2f33, slide_cut = 0x15181b, cut = 0x121416, frame = 0x14171a, barrel = 0x1c1f22, steel = 0x5a5f63, white = 0xe9ece9, tritium = 0x7dff6a;
  // Slide group (animated).
  s.material(slide, 0.62f);
  s.profile({{-.025f, .070f}, {.315f, .070f}, {.325f, .058f}, {.325f, .008f}, {-.025f, .008f}}, .074f);
  s.material(slide_cut, 0.3f);
  for (int i = 0; i < 5; ++i) s.box({.076f, .045f, .005f}, {0, .040f, -.012f - i * .016f});
  for (int i = 0; i < 3; ++i) s.box({.076f, .030f, .004f}, {0, .045f, -.262f - i * .014f});
  s.box({.052f, .020f, .020f}, {0, .087f, -.015f});
  s.material(white, 0.02f);
  for (f32 x : {-.017f, .017f}) s.box({.011f, .011f, .004f}, {x, .098f, -.0265f});
  s.material(tritium, 0, true);
  for (f32 x : {-.017f, .017f}) s.box({.006f, .006f, .005f}, {x, .098f, -.0272f});
  s.material(slide_cut, 0.3f);
  s.box({.018f, .024f, .018f}, {0, .090f, -.304f});
  s.material(white, 0.02f);
  s.box({.009f, .009f, .004f}, {0, .103f, -.3135f});
  s.material(tritium, 0, true);
  s.box({.005f, .005f, .005f}, {0, .103f, -.3142f});
  s.material(slide_cut, 0.3f);
  s.box({.004f, .024f, .056f}, {.038f, .054f, -.13f});
  s.material(steel, 0.7f);
  s.box({.005f, .006f, .024f}, {.0385f, .06f, -.095f});
  s.material(slide_cut, 0.3f);
  for (f32 x : {-.031f, .031f}) s.box({.012f, .006f, .33f}, {x, .07f, -.15f});
  // Frame and static parts.
  b.material(barrel, 0.8f);
  b.box({.076f, .010f, .072f}, {0, .067f, -.145f});
  b.cylinder(.019f, .019f, .275f, 16, {0, .029f, -.205f}, {kPi / 2, 0, 0});
  b.cylinder(.027f, .027f, .016f, 16, {0, .029f, -.345f}, {kPi / 2, 0, 0});
  b.material(frame, 0.02f);
  b.profile({{0, .008f}, {.30f, .008f}, {.30f, -.030f}, {.13f, -.040f}, {.105f, -.078f}, {.035f, -.078f}, {.020f, -.035f}, {-.01f, -.025f}}, .072f);
  b.box({.074f, .018f, .145f}, {0, -.055f, -.205f});
  b.profile({{.005f, -.035f}, {.078f, -.040f}, {.055f, -.245f}, {-.035f, -.245f}, {-.058f, -.190f}, {-.030f, -.050f}}, .078f);
  b.material(cut, 0.18f);
  for (int i = 0; i < 5; ++i) b.box({.080f, .005f, .050f}, {0, -.080f - i * .034f, .014f - i * .003f});
  b.box({.086f, .020f, .092f}, {0, -.250f, .005f});
  b.material(steel, 0.7f);
  b.box({.011f, .045f, .012f}, {0, -.083f, -.060f});
  b.box({.004f, .03f, .008f}, {0, -.07f, -.052f});
  b.box({.078f, .008f, .012f}, {0, -.008f, -.10f});
  b.cylinder(.007f, .007f, .05f, 10, {0, -.012f, -.33f}, {kPi / 2, 0, 0});
  b.material(cut, 0.18f);
  for (int i = 0; i < 3; ++i) b.box({.076f, .006f, .012f}, {0, -.064f, -.17f - i * .03f});
  b.material(steel, 0.7f);
  b.box({.09f, .012f, .1f}, {0, -.262f, .005f});
  // Builder transform: scale 1.35, offset (0, -.02, -.05).
  const auto place = [](Vec3 p) { return p * 1.35f + Vec3{0, -.02f, -.05f}; };
  w.grip = place({0, -.145f, .005f});
  w.rear_sight = place({0, .101f, -.020f});
  w.front_sight = place({0, .103f, -.304f});
  w.eject = place({.04f, .075f, -.13f});
  w.muzzle = place({0, .029f, -.362f});
  w.flash_scale = 1.7f * 1.35f;
  w.flash_ms = 52;
  w.slide_travel = .062f * 1.35f;
  w.mount = {.03f, -.016f, 0};
}

void sniper(Builder& b, WeaponModel& w) {
  const u32 dark = 0x23272b, tan = 0x8a7a5c, steel = 0x3c4044, black = 0x0c0d0f, glass = 0x1a3a52, bolt = 0x9aa0a6;
  b.material(dark, 0.35f);
  b.profile({{-.10f, .050f}, {.42f, .050f}, {.44f, .030f}, {.44f, -.040f}, {.30f, -.050f}, {-.10f, -.045f}}, .070f);
  b.material(tan, 0.05f);
  b.profile({{-.62f, .040f}, {-.10f, .040f}, {-.10f, -.045f}, {-.25f, -.060f}, {-.45f, -.150f}, {-.62f, -.160f}}, .060f);
  b.material(black, 0.4f);
  b.box({.064f, .12f, .02f}, {0, -.04f, .62f});
  b.material(tan, 0.05f);
  b.profile({{-.02f, -.040f}, {.06f, -.040f}, {.03f, -.200f}, {-.05f, -.200f}, {-.06f, -.150f}}, .058f);
  b.material(black, 0.4f);
  b.box({.06f, .09f, .11f}, {0, -.09f, -.20f});
  b.material(steel, 0.7f);
  b.cylinder(.026f, .026f, .95f, 14, {0, .015f, -.90f}, {kPi / 2, 0, 0});
  b.material(black, 0.4f);
  for (int i = 0; i < 6; ++i) {
    const f32 a = i * kPi / 3;
    b.box({.006f, .006f, .70f}, {kern::dm::cos(a) * .026f, .015f + kern::dm::sin(a) * .026f, -.82f});
  }
  b.cylinder(.040f, .040f, .13f, 10, {0, .015f, -1.42f}, {kPi / 2, 0, 0});
  b.material(steel, 0.7f);
  for (f32 x : {-.041f, .041f}) b.box({.004f, .03f, .09f}, {x, .015f, -1.42f});
  b.material(dark, 0.35f);
  b.box({.068f, .06f, .46f}, {0, .01f, -.62f});
  b.material(black, 0.4f);
  for (f32 x : {-.022f, .022f}) b.box({.012f, .012f, .30f}, {x, -.035f, -.70f});
  b.material(bolt, 0.8f);
  b.cylinder(.009f, .009f, .07f, 8, {.06f, .035f, -.04f}, {0, 0, kPi / 2});
  b.sphere(.016f, 10, 8, {.10f, .035f, -.04f});
  b.material(black, 0.4f);
  b.box({.05f, .055f, .03f}, {0, .075f, -.08f});
  b.box({.05f, .055f, .03f}, {0, .075f, -.36f});
  b.cylinder(.024f, .024f, .42f, 18, {0, .125f, -.22f}, {kPi / 2, 0, 0});
  b.cylinder(.036f, .024f, .11f, 18, {0, .125f, -.49f}, {-kPi / 2, 0, 0});
  b.cylinder(.032f, .024f, .07f, 18, {0, .125f, .03f}, {kPi / 2, 0, 0});
  b.material(dark, 0.35f);
  b.cylinder(.016f, .016f, .035f, 12, {0, .162f, -.22f}, {0, 0, 0});
  b.cylinder(.016f, .016f, .035f, 12, {.040f, .125f, -.22f}, {0, 0, kPi / 2});
  b.material(glass, 0.9f);
  b.circle(.030f, 20, {0, .125f, .067f});
  const auto place = [](Vec3 p) { return p * .62f + Vec3{0, -.01f, .10f}; };
  w.grip = place({0, -.12f, 0});
  w.rear_sight = place({0, .125f, .06f});
  w.front_sight = place({0, .125f, -.54f});
  w.muzzle = place({0, .015f, -1.49f});
  w.eject = place({.06f, .05f, -.05f});
  w.flash_scale = 2.4f * .62f;
  w.flash_ms = 70;
  w.mount = {.012f, -.01f, .02f};
}

void boomstick(Builder& b, WeaponModel& w) {
  const u32 olive = 0x4a5a3a, dark = 0x1d2024, orange = 0xff7a1a, yellow = 0xffd23f, black = 0x121316, wood = 0x8a5a3b, glow_o = 0xff8a2a;
  b.material(olive, 0.2f);
  b.profile({{-.06f, .07f}, {.22f, .075f}, {.26f, .03f}, {.24f, -.05f}, {-.06f, -.05f}}, .17f);
  b.material(wood, 0);
  b.profile({{.06f, -.05f}, {.15f, -.05f}, {.13f, -.25f}, {.04f, -.26f}}, .1f);
  b.material(dark, 0.15f);
  b.profile({{-.06f, .05f}, {-.06f, -.05f}, {-.28f, -.09f}, {-.30f, 0}, {-.2f, .05f}}, .13f);
  b.material(olive, 0.2f);
  b.cylinder(.11f, .11f, .2f, 10, {0, -.005f, -.36f}, {kPi / 2, 0, 0});
  for (int i = 0; i < 4; ++i) {
    b.material(i % 2 ? black : yellow, 0.1f);
    b.cylinder(.113f, .113f, .03f, 10, {0, -.005f, -.29f - i * .045f}, {kPi / 2, 0, 0});
  }
  b.material(dark, 0.15f);
  for (int i = 0; i < 6; ++i) {
    const f32 a = i / 6.0f * 2 * kPi;
    b.cylinder(.026f, .026f, .205f, 10, {kern::dm::cos(a) * .07f, -.005f + kern::dm::sin(a) * .07f, -.36f}, {kPi / 2, 0, 0});
  }
  b.cylinder(.058f, .058f, .36f, 14, {0, .03f, -.62f}, {kPi / 2, 0, 0});
  b.material(orange, 0.1f);
  b.cylinder(.068f, .068f, .06f, 14, {0, .03f, -.81f}, {kPi / 2, 0, 0});
  b.material(glow_o, 0, true);
  torus(b, .06f, .01f, {0, .03f, -.842f});
  b.material(black, 0.1f);
  b.box({.05f, .03f, .26f}, {0, .13f, -.2f});
  b.box({.04f, .06f, .03f}, {0, .1f, -.08f});
  b.box({.04f, .06f, .03f}, {0, .1f, -.32f});
  b.material(glow_o, 0, true);
  b.box({.02f, .04f, .02f}, {0, .16f, -.3f});
  w.grip = {0, -.15f, -.095f};
  w.rear_sight = {0, .175f, -.08f};
  w.front_sight = {0, .175f, -.32f};
  w.muzzle = {0, .03f, -.87f};
  w.eject = {.09f, .05f, -.2f};
  w.flash_scale = 1.45f;
  w.flash_ms = 60;
  w.mount = {.01f, -.015f, .018f};
}

}  // namespace

WeaponModel weapon_model(WeaponKind kind) {
  WeaponModel w;
  Builder b(w.parts), s(w.slide);
  switch (kind) {
    case WeaponKind::pistol: {
      const Mat4 xf = Mat4::translate({0, -.02f, -.05f}) * Mat4::scale({1.35f, 1.35f, 1.35f});
      b.set_transform(xf);
      s.set_transform(xf);
      glock(b, s, w);
      break;
    }
    case WeaponKind::sniper:
      b.set_transform(Mat4::translate({0, -.01f, .10f}) * Mat4::scale({.62f, .62f, .62f}));
      sniper(b, w);
      break;
    case WeaponKind::shotgun: boomstick(b, w); break;
    case WeaponKind::rifle: default: volt_smg(b, w); break;
  }
  return w;
}

void view_arms(Builder& b, Vec3 grip) {
  // player_walk_mode_v4.mjs:261-270 (posed for a grip near the legacy pistol grip) and the skin
  // hand of walk_world_experience_hotfix.mjs, moved so the right glove holds this weapon's grip.
  const Vec3 legacy_grip{.015f, -.185f, .040f};
  const Mat4 base = b.transform();
  b.set_transform(base * Mat4::translate(grip - legacy_grip));
  const u32 sleeve = 0x263544, cuff = 0x121a20, glove = 0x252a27, pad = 0x0e1211, skin = 0xc58a67;
  b.material(sleeve, 0);
  b.capsule(.066f, .31f, 8, {.075f, -.405f, .205f}, {-.54f, 0, -.13f});
  b.capsule(.064f, .28f, 8, {-.150f, -.365f, .155f}, {-.62f, 0, .27f});
  b.material(cuff, 0.04f);
  b.cylinder(.078f, .071f, .085f, 10, {.035f, -.270f, .115f}, {-.48f, 0, -.13f});
  b.cylinder(.075f, .068f, .078f, 10, {-.092f, -.242f, .055f}, {-.56f, 0, .25f});
  b.material(glove, 0);
  b.capsule(.047f, .125f, 9, {.015f, -.185f, .040f}, {-.15f, 0, -.04f}, {1.08f, 1, .88f});
  b.capsule(.045f, .115f, 9, {-.067f, -.128f, -.060f}, {-.28f, .18f, .34f}, {1.04f, 1, .82f});
  b.material(pad, 0.02f);
  b.box({.078f, .075f, .034f}, {.012f, -.142f, -.018f}, {-.15f, 0, -.03f});
  b.box({.070f, .060f, .030f}, {-.052f, -.096f, -.112f}, {-.22f, .16f, .30f});
  b.material(skin, 0);
  b.capsule(.043f, .075f, 8, {.035f, -.245f, .090f}, {.62f, 0, .08f});
  b.capsule(.048f, .19f, 8, {.055f, -.335f, .175f}, {.72f, 0, .10f});
  b.set_transform(base);
}

// ---------------------------------------------------------------------------
// character (character_model.mjs)

const std::array<Vec3, kJointCount>& joint_offsets() {
  static const std::array<Vec3, kJointCount> o = {{
      {0, 0, 0},           // root
      {0, 0, .96f},        // pelvis
      {0, 0, .10f},        // spine
      {0, 0, .50f},        // neck
      {0, 0, .10f},        // head
      {-.225f, 0, .44f},   // shoulder L
      {0, 0, -.30f},       // elbow L
      {0, 0, -.27f},       // wrist L
      {.225f, 0, .44f},    // shoulder R
      {0, 0, -.30f},       // elbow R
      {0, 0, -.27f},       // wrist R
      {-.10f, 0, -.02f},   // hip L
      {0, 0, -.45f},       // knee L
      {0, 0, -.44f},       // ankle L
      {.10f, 0, -.02f},    // hip R
      {0, 0, -.45f},       // knee R
      {0, 0, -.44f},       // ankle R
  }};
  return o;
}

const std::array<i8, kJointCount>& joint_parents() {
  static const std::array<i8, kJointCount> p = {{-1, kRoot, kPelvis, kSpine, kNeck, kSpine, kShoulderL, kElbowL, kSpine, kShoulderR, kElbowR,
                                                  kPelvis, kHipL, kKneeL, kPelvis, kHipR, kKneeR}};
  return p;
}

namespace {

std::array<Vec3, kJointCount> rest_positions() {
  std::array<Vec3, kJointCount> out{};
  for (u32 j = 0; j < kJointCount; ++j) out[j] = joint_offsets()[j] + (joint_parents()[j] >= 0 ? out[static_cast<u32>(joint_parents()[j])] : Vec3{});
  return out;
}

}  // namespace

Outfit player_outfit(u32 shirt) { return {shirt, 0x2b3024, 0x3d3b33, 0x1a1a18, 0xc08a68, 0x1c1e1b, 0x3a4030}; }

u32 shirt_for(u32 id) {
  u32 h = 2166136261u;
  for (char c : std::to_string(id)) h = (h ^ static_cast<u8>(c)) * 16777619u;
  static const u32 p[] = {0x4b5238, 0x55524a, 0x3f4a52, 0x5a4a3a, 0x474c3e, 0x3c4436};
  return p[h % 6];
}

PartMeshes character_mesh(const Outfit& o, bool gun) {
  PartMeshes out;
  Builder b(out);
  const auto rest = rest_positions();
  auto at = [&](u32 j) { b.set_bone(j); b.set_transform(Mat4::translate(rest[j])); };
  // limb(r, len): a capsule hanging from the joint down to -len.
  auto limb = [&](f32 r, f32 len, Vec3 pos = {}) { b.capsule(r, max_of(.01f, len - 2 * r), 7, pos + Vec3{0, 0, -len / 2}, {kPi / 2, 0, 0}); };
  at(kPelvis);
  b.material(o.pants, 0);
  b.box({.34f, .2f, .18f}, {0, 0, .02f});
  at(kSpine);
  b.material(o.shirt, 0);
  // torso: capsule .17 / .26 rotated to Z, then scaled (1.12, .72, 1) in that frame
  b.set_transform(Mat4::translate(rest[kSpine] + Vec3{0, 0, .25f}) * Mat4::scale({1.12f, .72f, 1}));
  b.capsule(.17f, .26f, 8, {}, {kPi / 2, 0, 0});
  at(kSpine);
  b.material(o.vest, 0);
  b.box({.38f, .28f, .34f}, {0, .004f, .27f});
  b.box({.30f, .07f, .16f}, {0, .16f, .22f});
  at(kNeck);
  b.material(o.skin, 0);
  limb(.055f, .10f, {0, 0, .08f});
  at(kHead);
  b.sphere(.115f, 10, 8, {0, .01f, .10f}, {}, {.92f, 1, 1.12f});
  b.material(o.helmet, 0.1f);
  b.sphere(.13f, 10, 5, {0, 0, .13f}, {kPi / 2, 0, 0}, {1, 1, 1}, kPi * .55f);
  for (u32 side = 0; side < 2; ++side) {
    const u32 sh = side ? kShoulderR : kShoulderL, el = side ? kElbowR : kElbowL, wr = side ? kWristR : kWristL;
    at(sh);
    b.material(o.shirt, 0);
    limb(.058f, .30f);
    at(el);
    limb(.05f, .27f);
    at(wr);
    b.material(o.gloves, 0);
    b.box({.075f, .09f, .11f}, {0, .01f, -.05f});
  }
  for (u32 side = 0; side < 2; ++side) {
    const u32 hip = side ? kHipR : kHipL, kn = side ? kKneeR : kKneeL, an = side ? kAnkleR : kAnkleL;
    at(hip);
    b.material(o.pants, 0);
    limb(.075f, .45f);
    at(kn);
    limb(.062f, .44f);
    at(an);
    b.material(o.boots, 0);
    b.box({.10f, .25f, .08f}, {0, .06f, -.03f});
  }
  if (gun) {
    // CHARACTER_GUN on the right wrist: (0, .02, -.09), rot.x -pi/2, two dark boxes.
    b.set_bone(kWristR);
    b.set_transform(Mat4::translate(rest[kWristR]) * Mat4::translate({0, .02f, -.09f}) * euler({-kPi / 2, 0, 0}));
    b.material(0x16181a, 0.3f);
    b.box({.035f, .22f, .05f}, {0, .08f, .02f});
    b.box({.03f, .05f, .10f}, {0, 0, -.04f});
  }
  return out;
}

// ---------------------------------------------------------------------------
// animation (animateCharacter, runGait)

namespace {

constexpr f32 kTau = 2 * kPi, kThigh = .45f, kShin = .44f, kAnkleZ = .05f, kToe = .185f, kReach = .885f, kHipDrop = .02f;

f32 clampf(f32 v, f32 a, f32 b) { return std::max(a, std::min(b, v)); }
f32 ease(f32 t) { return (1 - kern::dm::cos(kPi * clampf(t, 0, 1))) / 2; }
f32 cadence(f32 speed) { return clampf(150 + 6.5f * speed, 160, 205); }
f32 fract(f32 v) { return v - std::floor(v); }

void leg_ik(f32 dy, f32 dz, f32& hip, f32& knee) {
  f32 d = clampf(kern::dm::hypot(dy, dz), .35f, kReach);
  const f32 flex = kPi - kern::dm::acos(clampf((kThigh * kThigh + kShin * kShin - d * d) / (2 * kThigh * kShin), -1, 1));
  const f32 beta = kern::dm::acos(clampf((kThigh * kThigh + d * d - kShin * kShin) / (2 * kThigh * d), -1, 1));
  hip = kern::dm::atan2(dy, -dz) + beta;
  knee = -flex;
}

struct RunOut { f32 hipL, knL, anL, hipR, knR, anR, pelvis_z, pelvis_yaw, spine, spine_yaw, head, shL, shR, elL, elR; };

RunOut run_gait(f32 p, f32 speed) {
  RunOut o{};
  const f32 w = kTau * cadence(speed) / 120, tc = kTau / w;
  f32 duty = clampf(.40f - .025f * speed, .2f, .38f);
  duty = std::min(duty, .8f / std::max(.1f, speed * tc));
  const f32 lc = speed * duty * tc, y_land = lc * .42f;
  const f32 amp = clampf(.012f + .006f * speed, .018f, .032f), z_low = .905f - clampf(.012f * (speed - 2.5f), 0, .05f);
  const f32 phi_l = fract(p / kTau);
  auto pelvis_at = [&](f32 phi) { const f32 st = fract((phi - duty / 2) * 2); return z_low + amp * (1 - kern::dm::cos(kTau * st)); };
  o.pelvis_z = pelvis_at(phi_l);
  auto stance = [&](f32 u, f32 pz, f32& h, f32& k, f32& an) {
    const f32 th = u < .55f ? 0 : -.72f * kern::dm::pow((u - .55f) / .45f, 1.6f);
    const f32 toe_y = y_land + kToe - lc * u, ay = toe_y - kToe * kern::dm::cos(th), az = kAnkleZ + kToe * kern::dm::sin(-th);
    leg_ik(ay, az - (pz - kHipDrop), h, k);
    an = th - (h + k);
  };
  f32 h_to, k_to, a_to, h_s, k_s, a_s;
  stance(1, pelvis_at(duty), h_to, k_to, a_to);
  stance(0, pelvis_at(0), h_s, k_s, a_s);
  const f32 ov = .1f, k_add = clampf(.7f + .18f * speed, .9f, 1.75f);
  auto leg = [&](f32 phi, f32& h, f32& k, f32& an) {
    if (phi < duty) { stance(phi / duty, pelvis_at(phi), h, k, an); return; }
    const f32 u = (phi - duty) / (1 - duty), e = ease(u / .82f);
    h = h_to + (h_s + ov - h_to) * e - ov * ease((u - .82f) / .18f);
    k = k_to + (k_s - k_to) * u - k_add * kern::dm::pow(std::max(0.0f, kern::dm::sin(kPi * std::min(1.0f, u / .86f))), 1.3f);
    an = a_to + (a_s - a_to) * ease(u) + .12f * kern::dm::sin(kPi * u);
  };
  leg(phi_l, o.hipL, o.knL, o.anL);
  leg(fract(phi_l + .5f), o.hipR, o.knR, o.anR);
  o.anL = clampf(o.anL, -.95f, .55f);
  o.anR = clampf(o.anR, -.95f, .55f);
  const f32 a = kern::dm::cos(kTau * (phi_l + .04f)), aa = clampf(.3f + .07f * speed, .42f, .95f);
  o.shL = .12f - aa * a;
  o.shR = .12f + aa * a;
  o.elL = 1.42f - .22f * a;
  o.elR = 1.42f + .22f * a;
  o.spine = -clampf(.05f + .025f * speed, .08f, .3f);
  o.head = -o.spine * .7f;
  o.spine_yaw = .09f * a;
  o.pelvis_yaw = -.07f * a;
  return o;
}

f32 lerpf(f32 a, f32 b, f32 k) { return a + (b - a) * k; }

}  // namespace

CharacterAnimator::CharacterAnimator(u32 seed) {
  phase_ = static_cast<f32>((seed * 2654435761u) % 6000u) / 1000.0f;  // random 0..6 like SpawnCity
}

void CharacterAnimator::set(u32 j, Vec3 r, f32 k) {
  rot_[j] = {lerpf(rot_[j].x, r.x, k), lerpf(rot_[j].y, r.y, k), lerpf(rot_[j].z, r.z, k)};
}

void CharacterAnimator::update(const Input& in) {
  const f32 dt = clampf(in.dt, 0, .1f);
  const bool air = in.air && !in.dead;
  const f32 speed = air || in.dead ? 0 : in.speed;
  const f32 k = 1 - kern::dm::exp(-dt * 12);
  const bool moving = speed > .25f, run = speed > 3.2f && in.crouch < .5f;
  phase_ += dt * (run ? kTau * cadence(speed) / 120 : moving ? std::min(12.0f, 4.2f + speed * 1.9f) : 1.2f);
  if (phase_ > 1000 * kTau) phase_ -= 1000 * kTau;
  run_w_ = run ? std::min(1.0f, run_w_ + dt * 4) : std::max(0.0f, run_w_ - dt * 4);
  RunOut g{};
  if (run) g = run_gait(phase_, speed);
  const f32 kr = run ? (run_w_ >= 1 ? 1 : std::max(k, run_w_ * run_w_)) : k;
  const f32 p = phase_;
  const f32 swing = moving ? kern::dm::sin(p) * std::min(.75f, .3f + speed * .09f) : 0;
  const f32 lift = moving ? std::max(0.0f, kern::dm::cos(p)) * .35f : 0, lift_o = moving ? std::max(0.0f, -kern::dm::cos(p)) * .35f : 0;
  // Pelvis height (Kern: crouching lowers it).
  const f32 pz = run && !air ? g.pelvis_z : .96f - (moving ? std::fabs(kern::dm::sin(p)) * .03f : 0);
  pelvis_z_ = lerpf(pelvis_z_, pz - in.crouch * .3f, run ? kr : k);
  rot_[kPelvis].z = run ? g.pelvis_yaw * run_w_ : 0;
  rot_[kAnkleL].x = run ? g.anL * run_w_ : 0;
  rot_[kAnkleR].x = run ? g.anR * run_w_ : 0;
  // Legs.
  if (in.dead) {
    set(kHipL, {0, 0, .1f}, k); set(kHipR, {0, 0, -.1f}, k); set(kKneeL, {}, k); set(kKneeR, {}, k);
  } else if (air) {
    const f32 sway = kern::dm::sin(p * .9f) * .05f;
    set(kHipL, {.32f + sway, 0, .08f}, k); set(kHipR, {.08f - sway, 0, -.08f}, k);
    set(kKneeL, {-.55f, 0, 0}, k); set(kKneeR, {-.3f, 0, 0}, k);
  } else if (run) {
    set(kHipL, {g.hipL, 0, 0}, kr); set(kHipR, {g.hipR, 0, 0}, kr);
    set(kKneeL, {g.knL, 0, 0}, kr); set(kKneeR, {g.knR, 0, 0}, kr);
  } else {
    // Crouch (Kern): thighs forward, knees folded so the feet stay under the lowered pelvis.
    const f32 c = in.crouch;
    set(kHipL, {swing + c * .95f, 0, 0}, k); set(kHipR, {-swing + c * .95f, 0, 0}, k);
    set(kKneeL, {-lift * 1.1f - c * 1.5f, 0, 0}, k); set(kKneeR, {-lift_o * 1.1f - c * 1.5f, 0, 0}, k);
    rot_[kAnkleL].x += c * .55f;
    rot_[kAnkleR].x += c * .55f;
  }
  // Torso lean and the aim pitch (Kern replicates where players look).
  const f32 pitch = in.dead ? 0 : clampf(in.pitch, -1.2f, 1.2f);
  set(kSpine, {(run ? g.spine : 0) - pitch * .25f + in.crouch * -.15f, 0, run ? g.spine_yaw : 0}, run ? kr : k);
  set(kHead, {(run ? g.head : 0) - pitch * .45f, 0, 0}, k);
  // Arms.
  const f32 aim_down = -pitch * .75f;
  if (in.dead) {
    set(kShoulderL, {.2f, 0, .5f}, k); set(kShoulderR, {.2f, 0, -.5f}, k); set(kElbowL, {}, k); set(kElbowR, {}, k);
  } else if (in.armed && in.pistol) {
    set(kShoulderR, {1.48f + aim_down + in.kick * .22f, 0, -.05f}, k); set(kElbowR, {.05f, 0, 0}, k);
    set(kShoulderL, {1.38f + aim_down, 0, .32f}, k); set(kElbowL, {.3f, 0, 0}, k);
  } else if (in.armed) {
    set(kShoulderR, {1.1f + aim_down + in.kick * .22f, 0, -.2f}, k); set(kElbowR, {.3f, 0, 0}, k);
    set(kShoulderL, {1.25f + aim_down, 0, .42f}, k); set(kElbowL, {.6f, 0, 0}, k);
  } else if (run) {
    set(kShoulderL, {g.shL, 0, -.2f}, kr); set(kShoulderR, {g.shR, 0, .2f}, kr);
    set(kElbowL, {g.elL, 0, 0}, kr); set(kElbowR, {g.elR, 0, 0}, kr);
  } else {
    set(kShoulderL, {-swing * .6f, 0, .06f}, k); set(kShoulderR, {swing * .6f, 0, -.06f}, k);
    set(kElbowL, {.25f + (moving ? .2f : 0), 0, 0}, k); set(kElbowR, {.25f + (moving ? .2f : 0), 0, 0}, k);
  }
  // Death: the body tips over backwards (SpawnCity swaps in a ragdoll).
  fall_ = in.dead ? std::min(1.0f, fall_ + dt * 2.6f) : std::max(0.0f, fall_ - dt * 4);
  rot_[kRoot] = {kPi / 2 * fall_ * fall_, 0, 0};
}

Mat4 CharacterAnimator::joint_matrix(u32 j) const {
  Mat4 m;
  // Walk the chain root -> j.
  u32 chain[kJointCount];
  u32 n = 0;
  for (i32 c = static_cast<i32>(j); c >= 0; c = joint_parents()[static_cast<u32>(c)]) chain[n++] = static_cast<u32>(c);
  for (u32 i = n; i-- > 0;) {
    const u32 c = chain[i];
    Vec3 off = joint_offsets()[c];
    if (c == kPelvis) off.z = pelvis_z_;
    m = m * Mat4::translate(off) * euler(rot_[c]);
  }
  return m;
}

void CharacterAnimator::bones(f32* out) const {
  const auto rest = rest_positions();
  std::array<Mat4, kJointCount> world{};
  for (u32 j = 0; j < kJointCount; ++j) {
    Vec3 off = joint_offsets()[j];
    if (j == kPelvis) off.z = pelvis_z_;
    const Mat4 local = Mat4::translate(off) * euler(rot_[j]);
    world[j] = joint_parents()[j] >= 0 ? world[static_cast<u32>(joint_parents()[j])] * local : local;
    const Mat4 bone = world[j] * Mat4::translate(rest[j] * -1.0f);
    std::copy(bone.m, bone.m + 16, out + j * 16);
  }
}

// ---------------------------------------------------------------------------
// effect meshes

namespace {

void push_tri(MeshData& m, Vec3 a, Vec3 b, Vec3 c, Vec3 ca, Vec3 cb, Vec3 cc, Vec3 n) {
  m.push(a, n, ca, {0, 0});
  m.push(b, n, cb, {0, 0});
  m.push(c, n, cc, {0, 0});
}

}  // namespace

MeshData flash_mesh() {
  // FLASH_STAR_FS as geometry: a seven-armed star (sp(a) spikes) bright at the core, orange at the
  // tips, in the plane facing -Z; FLASH_SIDE_FS as two crossed tongues reaching forward along -Z.
  MeshData m;
  const Vec3 core{1, .96f, .82f}, mid{1, .7f, .3f}, tip{1, .42f, .08f};
  const u32 n = 28;
  auto spikes = [](f32 a) {
    const f32 sp = kern::dm::pow(std::fabs(kern::dm::cos(2.5f * a + 1.3f)), 14.0f) + .75f * kern::dm::pow(std::fabs(kern::dm::cos(4 * a + 3)), 26.0f) +
                   .5f * kern::dm::pow(std::fabs(kern::dm::cos(7 * a + 6.6f)), 40.0f);
    return .18f + .82f * std::min(1.0f, sp);
  };
  for (u32 i = 0; i < n; ++i) {
    const f32 a0 = 2 * kPi * i / n, a1 = 2 * kPi * (i + 1) / n;
    const f32 r0 = spikes(a0) * .1f, r1 = spikes(a1) * .1f;
    const Vec3 p0{kern::dm::cos(a0) * r0, kern::dm::sin(a0) * r0, -.012f}, p1{kern::dm::cos(a1) * r1, kern::dm::sin(a1) * r1, -.012f};
    const Vec3 q0 = p0 * .35f, q1 = p1 * .35f;
    push_tri(m, {0, 0, -.012f}, q0, q1, core, core, core, {0, 0, 1});
    push_tri(m, q0, p0, p1, mid, tip, tip, {0, 0, 1});
    push_tri(m, q0, p1, q1, mid, tip, mid, {0, 0, 1});
  }
  // Side tongues: width w(x) = (1-x)(.42 + .18 sin(17x)) + .05 (of .11), length 1 along -Z.
  for (int plane = 0; plane < 2; ++plane) {
    const u32 steps = 10;
    for (u32 s = 0; s < steps; ++s) {
      const f32 x0 = static_cast<f32>(s) / steps, x1 = static_cast<f32>(s + 1) / steps;
      auto w = [](f32 x) { return ((1 - x) * (.42f + .18f * kern::dm::sin(17 * x + 2)) + .05f) * .11f; };
      const Vec3 c0 = tip + (core - tip) * kern::dm::pow(1 - x0, 2.0f), c1 = tip + (core - tip) * kern::dm::pow(1 - x1, 2.0f);
      const Vec3 side = plane ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
      const Vec3 n = plane ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
      const Vec3 a{0, 0, -x0 * .26f}, b{0, 0, -x1 * .26f};
      push_tri(m, a - side * w(x0), a + side * w(x0), b + side * w(x1), c0, c0, c1, n);
      push_tri(m, a - side * w(x0), b + side * w(x1), b - side * w(x1), c0, c1, c1, n);
    }
  }
  return m;
}

MeshData tracer_mesh(u32 seg) {
  MeshData m;
  for (u32 i = 0; i < seg; ++i) {
    const f32 a0 = 2 * kPi * i / seg, a1 = 2 * kPi * (i + 1) / seg;
    const Vec3 d0{kern::dm::cos(a0), kern::dm::sin(a0), 0}, d1{kern::dm::cos(a1), kern::dm::sin(a1), 0};
    push_tri(m, d0, d1, d1 + Vec3{0, 0, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, normalize(d0 + d1));
    push_tri(m, d0, d1 + Vec3{0, 0, 1}, d0 + Vec3{0, 0, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, normalize(d0 + d1));
  }
  return m;
}

MeshData decal_mesh() {
  // A scorched bullet hole: dark centre, softer rim (vertex colours multiply the instance tint).
  MeshData m;
  const u32 seg = 14;
  for (u32 i = 0; i < seg; ++i) {
    const f32 a0 = 2 * kPi * i / seg, a1 = 2 * kPi * (i + 1) / seg;
    const Vec3 p0{kern::dm::cos(a0), kern::dm::sin(a0), 0}, p1{kern::dm::cos(a1), kern::dm::sin(a1), 0};
    push_tri(m, {0, 0, 0}, p0 * .45f, p1 * .45f, {.35f, .35f, .35f}, {.6f, .6f, .6f}, {.6f, .6f, .6f}, {0, 0, 1});
    push_tri(m, p0 * .45f, p0, p1, {.6f, .6f, .6f}, {1, 1, 1}, {1, 1, 1}, {0, 0, 1});
    push_tri(m, p0 * .45f, p1, p1 * .45f, {.6f, .6f, .6f}, {1, 1, 1}, {.6f, .6f, .6f}, {0, 0, 1});
  }
  return m;
}

MeshData chip_mesh() {
  PartMeshes p;
  Builder b(p);
  b.material(0xffffff);
  b.box({1, 1, 1}, {});
  return p.matte;
}

MeshData ball_mesh() {
  PartMeshes p;
  Builder b(p);
  b.material(0xffffff);
  b.sphere(1, 10, 8, {});
  return p.matte;
}

MeshData brass_mesh() {
  PartMeshes p;
  Builder b(p);
  b.material(0xffffff);
  b.cylinder(.0048f, .0048f, .019f, 8, {}, {});
  return p.matte;
}

}  // namespace kern::game::models
