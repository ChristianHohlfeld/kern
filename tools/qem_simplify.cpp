// Quadric edge-collapse mesh simplification (Garland & Heckbert, "Surface Simplification Using
// Quadric Error Metrics", SIGGRAPH 1997), the iterative-threshold variant: each pass collapses every
// edge whose error is below a rising threshold, so no priority queue is needed. A collapse that
// would flip or squash a neighbouring triangle is refused, and open borders only collapse along
// themselves, so thin walls (a printed shell) keep both faces.
//
// Used by tools/mesh_simplify.py (the Micro Duck import). Standalone; build with
//   c++ -O2 -std=c++17 tools/qem_simplify.cpp -o qem_simplify
// I/O (little endian): stdin  u32 vertex count, u32 triangle count, f64 xyz..., u32 abc..., u32 target
//                      stdout u32 vertex count, u32 triangle count, f64 xyz..., u32 abc...
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

double kFlipLimit = 0.0;  // a neighbour may turn at most 90 degrees in one collapse (current normals)

struct Q {  // symmetric 4x4 quadric
  double m[10] = {0};
  Q() = default;
  Q(double a, double b, double c, double d) {
    m[0] = a * a; m[1] = a * b; m[2] = a * c; m[3] = a * d; m[4] = b * b;
    m[5] = b * c; m[6] = b * d; m[7] = c * c; m[8] = c * d; m[9] = d * d;
  }
  Q operator+(const Q& o) const { Q r; for (int i = 0; i < 10; ++i) r.m[i] = m[i] + o.m[i]; return r; }
  double det(int a11, int a12, int a13, int a21, int a22, int a23, int a31, int a32, int a33) const {
    return m[a11] * m[a22] * m[a33] + m[a13] * m[a21] * m[a32] + m[a12] * m[a23] * m[a31] - m[a13] * m[a22] * m[a31] -
           m[a11] * m[a23] * m[a32] - m[a12] * m[a21] * m[a33];
  }
};

struct V3 { double x = 0, y = 0, z = 0; };
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 normalized(V3 a) { const double l = std::sqrt(dot(a, a)); return l > 0 ? a * (1 / l) : a; }

struct Tri { int v[3]; double err[4]; bool deleted = false, dirty = false; V3 n; };
struct Vert { V3 p; int tstart = 0, tcount = 0; Q q; bool border = false; };
struct Ref { int tid, tvertex; };

std::vector<Tri> tris;
std::vector<Vert> verts;
std::vector<Ref> refs;

double vertex_error(const Q& q, double x, double y, double z) {
  const double* m = q.m;
  return m[0] * x * x + 2 * m[1] * x * y + 2 * m[2] * x * z + 2 * m[3] * x + m[4] * y * y + 2 * m[5] * y * z + 2 * m[6] * y +
         m[7] * z * z + 2 * m[8] * z + m[9];
}

double calculate_error(int a, int b, V3& out) {
  const Q q = verts[a].q + verts[b].q;
  const bool border = verts[a].border && verts[b].border;
  const double det = q.det(0, 1, 2, 1, 4, 5, 2, 5, 7);
  if (det != 0 && !border) {
    out.x = -1 / det * q.det(1, 2, 3, 4, 5, 6, 5, 7, 8);
    out.y = 1 / det * q.det(0, 2, 3, 1, 5, 6, 2, 7, 8);
    out.z = -1 / det * q.det(0, 1, 3, 1, 4, 6, 2, 5, 8);
    return vertex_error(q, out.x, out.y, out.z);
  }
  const V3 p1 = verts[a].p, p2 = verts[b].p, p3 = (p1 + p2) * 0.5;
  const double e1 = vertex_error(q, p1.x, p1.y, p1.z), e2 = vertex_error(q, p2.x, p2.y, p2.z), e3 = vertex_error(q, p3.x, p3.y, p3.z);
  double e = e1;
  out = p1;
  if (e2 < e) { e = e2; out = p2; }
  if (e3 < e) { e = e3; out = p3; }
  return e;
}

// Would moving vertex i0 (whose partner i1 merges into it) to p flip or squash a triangle?
bool flipped(V3 p, int i1, const Vert& v0, std::vector<int>& deleted) {
  for (int k = 0; k < v0.tcount; ++k) {
    const Tri& t = tris[refs[v0.tstart + k].tid];
    if (t.deleted) continue;
    const int s = refs[v0.tstart + k].tvertex;
    const int id1 = t.v[(s + 1) % 3], id2 = t.v[(s + 2) % 3];
    if (id1 == i1 || id2 == i1) { deleted[k] = 1; continue; }
    const V3 d1 = normalized(verts[id1].p - p), d2 = normalized(verts[id2].p - p);
    if (std::fabs(dot(d1, d2)) > 0.999) return true;
    const V3 n = normalized(cross(d1, d2));
    deleted[k] = 0;
    if (dot(n, t.n) < kFlipLimit) return true;
  }
  return false;
}

void update_triangles(int i0, const Vert& v, const std::vector<int>& deleted, int& deleted_triangles) {
  V3 p;
  for (int k = 0; k < v.tcount; ++k) {
    const Ref r = refs[v.tstart + k];
    Tri& t = tris[r.tid];
    if (t.deleted) continue;
    if (deleted[k]) { t.deleted = true; ++deleted_triangles; continue; }
    t.v[r.tvertex] = i0;
    t.dirty = true;
    t.n = normalized(cross(verts[t.v[1]].p - verts[t.v[0]].p, verts[t.v[2]].p - verts[t.v[0]].p));  // the flip test compares with now
    t.err[0] = calculate_error(t.v[0], t.v[1], p);
    t.err[1] = calculate_error(t.v[1], t.v[2], p);
    t.err[2] = calculate_error(t.v[2], t.v[0], p);
    t.err[3] = std::fmin(t.err[0], std::fmin(t.err[1], t.err[2]));
    refs.push_back(r);
  }
}

void update_mesh(int iteration) {
  if (iteration > 0) {  // compact triangles
    std::size_t dst = 0;
    for (const Tri& t : tris)
      if (!t.deleted) tris[dst++] = t;
    tris.resize(dst);
  }
  for (Vert& v : verts) { v.tstart = 0; v.tcount = 0; }
  for (const Tri& t : tris)
    for (int j = 0; j < 3; ++j) verts[t.v[j]].tcount++;
  int tstart = 0;
  for (Vert& v : verts) { v.tstart = tstart; tstart += v.tcount; v.tcount = 0; }
  refs.assign(tris.size() * 3, Ref{0, 0});
  for (std::size_t i = 0; i < tris.size(); ++i) {
    const Tri& t = tris[i];
    for (int j = 0; j < 3; ++j) {
      Vert& v = verts[t.v[j]];
      refs[v.tstart + v.tcount] = {static_cast<int>(i), j};
      v.tcount++;
    }
  }
  if (iteration == 0) {
    // Borders: an edge used by one triangle only.
    std::vector<int> vcount, vids;
    for (Vert& v : verts) v.border = false;
    for (std::size_t i = 0; i < verts.size(); ++i) {
      const Vert& v = verts[i];
      vcount.clear();
      vids.clear();
      for (int j = 0; j < v.tcount; ++j) {
        const Tri& t = tris[refs[v.tstart + j].tid];
        for (int k = 0; k < 3; ++k) {
          std::size_t ofs = 0;
          const int id = t.v[k];
          while (ofs < vcount.size() && vids[ofs] != id) ++ofs;
          if (ofs == vcount.size()) { vcount.push_back(1); vids.push_back(id); }
          else vcount[ofs]++;
        }
      }
      for (std::size_t j = 0; j < vcount.size(); ++j)
        if (vcount[j] == 1) verts[vids[j]].border = true;
    }
    // Quadrics from the original planes, then the edge errors.
    for (Tri& t : tris) {
      const V3 p0 = verts[t.v[0]].p, p1 = verts[t.v[1]].p, p2 = verts[t.v[2]].p;
      const V3 n = normalized(cross(p1 - p0, p2 - p0));
      t.n = n;
      const Q q(n.x, n.y, n.z, -dot(n, p0));
      for (int j = 0; j < 3; ++j) verts[t.v[j]].q = verts[t.v[j]].q + q;
    }
    V3 p;
    for (Tri& t : tris) {
      for (int j = 0; j < 3; ++j) t.err[j] = calculate_error(t.v[j], t.v[(j + 1) % 3], p);
      t.err[3] = std::fmin(t.err[0], std::fmin(t.err[1], t.err[2]));
    }
  }
}

void simplify(int target, double aggressiveness = 7) {
  for (Tri& t : tris) t.deleted = false;
  int deleted_triangles = 0;
  std::vector<int> deleted0, deleted1;
  const int start = static_cast<int>(tris.size());
  for (int iteration = 0; iteration < 200; ++iteration) {
    if (start - deleted_triangles <= target) break;
    if (iteration % 5 == 0) update_mesh(iteration);
    for (Tri& t : tris) t.dirty = false;
    const double threshold = 1e-9 * std::pow(iteration + 3.0, aggressiveness);
    for (std::size_t i = 0; i < tris.size(); ++i) {
      Tri& t = tris[i];
      if (t.err[3] > threshold || t.deleted || t.dirty) continue;
      for (int j = 0; j < 3; ++j) {
        if (t.err[j] >= threshold) continue;
        const int i0 = t.v[j], i1 = t.v[(j + 1) % 3];
        Vert& v0 = verts[i0];
        const Vert& v1 = verts[i1];
        if (v0.border != v1.border) continue;  // borders collapse along themselves only
        V3 p;
        calculate_error(i0, i1, p);
        deleted0.assign(v0.tcount, 0);
        deleted1.assign(v1.tcount, 0);
        if (flipped(p, i1, v0, deleted0) || flipped(p, i0, v1, deleted1)) continue;
        v0.p = p;
        v0.q = v1.q + v0.q;
        const int tstart = static_cast<int>(refs.size());
        update_triangles(i0, v0, deleted0, deleted_triangles);
        update_triangles(i0, v1, deleted1, deleted_triangles);
        const int tcount = static_cast<int>(refs.size()) - tstart;
        if (tcount <= v0.tcount) {
          for (int k = 0; k < tcount; ++k) refs[v0.tstart + k] = refs[tstart + k];
        } else {
          v0.tstart = tstart;
        }
        v0.tcount = tcount;
        break;
      }
      if (start - deleted_triangles <= target) break;
    }
  }
  // Compact.
  update_mesh(1);
  std::vector<int> remap(verts.size(), -1);
  std::vector<Vert> kept;
  for (Tri& t : tris)
    for (int j = 0; j < 3; ++j) {
      int& r = remap[t.v[j]];
      if (r < 0) { r = static_cast<int>(kept.size()); kept.push_back(verts[t.v[j]]); }
      t.v[j] = r;
    }
  verts = std::move(kept);
}

template <class T> bool get(T& v) { return std::fread(&v, sizeof v, 1, stdin) == 1; }
template <class T> void put(const T& v) { std::fwrite(&v, sizeof v, 1, stdout); }

}  // namespace

int main() {
  std::uint32_t nv = 0, nt = 0, target = 0;
  if (!get(nv) || !get(nt)) return 1;
  verts.resize(nv);
  for (Vert& v : verts) if (!get(v.p.x) || !get(v.p.y) || !get(v.p.z)) return 1;
  tris.resize(nt);
  for (Tri& t : tris)
    for (int j = 0; j < 3; ++j) {
      std::uint32_t i = 0;
      if (!get(i) || i >= nv) return 1;
      t.v[j] = static_cast<int>(i);
    }
  if (!get(target)) return 1;
  if (const char* e = std::getenv("QEM_FLIP")) kFlipLimit = std::atof(e);
  simplify(static_cast<int>(target));
  put(static_cast<std::uint32_t>(verts.size()));
  put(static_cast<std::uint32_t>(tris.size()));
  for (const Vert& v : verts) { put(v.p.x); put(v.p.y); put(v.p.z); }
  for (const Tri& t : tris)
    for (int j = 0; j < 3; ++j) put(static_cast<std::uint32_t>(t.v[j]));
  return 0;
}
