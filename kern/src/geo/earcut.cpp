// C++ port of Mapbox Earcut v3.0.2 (ISC). Structure and arithmetic follow
// the upstream JavaScript line by line so both produce identical triangles.
#include "kern/geo/earcut.h"

#include <algorithm>
#include <cmath>

namespace kern {

namespace {

inline f64 tri_area(f64 px, f64 py, f64 qx, f64 qy, f64 rx, f64 ry) { return (qy - py) * (rx - qx) - (qx - px) * (ry - qy); }

inline bool point_in_triangle(f64 ax, f64 ay, f64 bx, f64 by, f64 cx, f64 cy, f64 px, f64 py) {
  return (cx - px) * (ay - py) >= (ax - px) * (cy - py) && (ax - px) * (by - py) >= (bx - px) * (ay - py) &&
         (bx - px) * (cy - py) >= (cx - px) * (by - py);
}

inline bool point_in_triangle_except_first(f64 ax, f64 ay, f64 bx, f64 by, f64 cx, f64 cy, f64 px, f64 py) {
  return !(ax == px && ay == py) && point_in_triangle(ax, ay, bx, by, cx, cy, px, py);
}

inline int sign(f64 v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

f64 signed_area(const f64* xy, u32 start, u32 end) {
  f64 sum = 0;
  for (u32 i = start, j = end - 1; i < end; ++i) {
    sum += (xy[j * 2] - xy[i * 2]) * (xy[i * 2 + 1] + xy[j * 2 + 1]);
    j = i;
  }
  return sum;
}

// JS `value | 0` for the z-order transform (truncation, int32 wrap).
inline i32 to_int32(f64 v) {
  if (!std::isfinite(v)) return 0;
  const f64 t = std::trunc(v);
  return static_cast<i32>(static_cast<u32>(static_cast<i64>(std::fmod(t, 4294967296.0))));
}

i32 z_order(f64 x, f64 y, f64 min_x, f64 min_y, f64 inv_size) {
  u32 ix = static_cast<u32>(to_int32((x - min_x) * inv_size));
  u32 iy = static_cast<u32>(to_int32((y - min_y) * inv_size));
  ix = (ix | (ix << 8)) & 0x00FF00FFu;
  ix = (ix | (ix << 4)) & 0x0F0F0F0Fu;
  ix = (ix | (ix << 2)) & 0x33333333u;
  ix = (ix | (ix << 1)) & 0x55555555u;
  iy = (iy | (iy << 8)) & 0x00FF00FFu;
  iy = (iy | (iy << 4)) & 0x0F0F0F0Fu;
  iy = (iy | (iy << 2)) & 0x33333333u;
  iy = (iy | (iy << 1)) & 0x55555555u;
  return static_cast<i32>(ix | (iy << 1));
}

}  // namespace

#define AREA(p, q, r) tri_area((p)->x, (p)->y, (q)->x, (q)->y, (r)->x, (r)->y)
#define EQUALS(a, b) ((a)->x == (b)->x && (a)->y == (b)->y)

Earcut::~Earcut() {
  for (Node* b : blocks_) delete[] b;
}

Earcut::Node* Earcut::create(u32 i, f64 x, f64 y) {
  const u32 block = used_ / kBlock, slot = used_ % kBlock;
  if (block >= blocks_.size()) blocks_.push_back(new Node[kBlock]);
  ++used_;
  Node* n = &blocks_[block][slot];
  *n = Node{i, x, y, nullptr, nullptr, 0, nullptr, nullptr, false};
  return n;
}

Earcut::Node* Earcut::insert(u32 i, f64 x, f64 y, Node* last) {
  Node* p = create(i, x, y);
  if (!last) {
    p->prev = p;
    p->next = p;
  } else {
    p->next = last->next;
    p->prev = last;
    last->next->prev = p;
    last->next = p;
  }
  return p;
}

void Earcut::remove(Node* p) {
  p->next->prev = p->prev;
  p->prev->next = p->next;
  if (p->prev_z) p->prev_z->next_z = p->next_z;
  if (p->next_z) p->next_z->prev_z = p->prev_z;
}

Earcut::Node* Earcut::linked_list(const f64* xy, u32 start, u32 end, bool clockwise) {
  Node* last = nullptr;
  if (clockwise == (signed_area(xy, start, end) > 0)) {
    for (u32 i = start; i < end; ++i) last = insert(i, xy[i * 2], xy[i * 2 + 1], last);
  } else {
    for (u32 i = end; i-- > start;) last = insert(i, xy[i * 2], xy[i * 2 + 1], last);
  }
  if (last && EQUALS(last, last->next)) {
    remove(last);
    last = last->next;
  }
  return last;
}

Earcut::Node* Earcut::filter_points(Node* start, Node* end) {
  if (!start) return start;
  if (!end) end = start;
  Node* p = start;
  bool again;
  do {
    again = false;
    if (!p->steiner && (EQUALS(p, p->next) || AREA(p->prev, p, p->next) == 0)) {
      remove(p);
      p = end = p->prev;
      if (p == p->next) break;
      again = true;
    } else {
      p = p->next;
    }
  } while (again || p != end);
  return end;
}

void Earcut::earcut_linked(Node* ear, std::vector<u32>& out, f64 min_x, f64 min_y, f64 inv_size, int pass) {
  if (!ear) return;
  if (!pass && inv_size != 0) index_curve(ear, min_x, min_y, inv_size);
  Node* stop = ear;
  while (ear->prev != ear->next) {
    Node* prev = ear->prev;
    Node* next = ear->next;
    if (inv_size != 0 ? is_ear_hashed(ear, min_x, min_y, inv_size) : is_ear(ear)) {
      out.push_back(prev->i);
      out.push_back(ear->i);
      out.push_back(next->i);
      remove(ear);
      ear = next->next;
      stop = next->next;
      continue;
    }
    ear = next;
    if (ear == stop) {
      if (!pass) {
        earcut_linked(filter_points(ear), out, min_x, min_y, inv_size, 1);
      } else if (pass == 1) {
        ear = cure_local_intersections(filter_points(ear), out);
        earcut_linked(ear, out, min_x, min_y, inv_size, 2);
      } else if (pass == 2) {
        split_earcut(ear, out, min_x, min_y, inv_size);
      }
      break;
    }
  }
}

bool Earcut::is_ear(const Node* ear) {
  const Node *a = ear->prev, *b = ear, *c = ear->next;
  if (AREA(a, b, c) >= 0) return false;
  const f64 ax = a->x, bx = b->x, cx = c->x, ay = a->y, by = b->y, cy = c->y;
  const f64 x0 = std::min({ax, bx, cx}), y0 = std::min({ay, by, cy}), x1 = std::max({ax, bx, cx}), y1 = std::max({ay, by, cy});
  const Node* p = c->next;
  while (p != a) {
    if (p->x >= x0 && p->x <= x1 && p->y >= y0 && p->y <= y1 && point_in_triangle_except_first(ax, ay, bx, by, cx, cy, p->x, p->y) &&
        AREA(p->prev, p, p->next) >= 0)
      return false;
    p = p->next;
  }
  return true;
}

bool Earcut::is_ear_hashed(const Node* ear, f64 min_x, f64 min_y, f64 inv_size) {
  const Node *a = ear->prev, *b = ear, *c = ear->next;
  if (AREA(a, b, c) >= 0) return false;
  const f64 ax = a->x, bx = b->x, cx = c->x, ay = a->y, by = b->y, cy = c->y;
  const f64 x0 = std::min({ax, bx, cx}), y0 = std::min({ay, by, cy}), x1 = std::max({ax, bx, cx}), y1 = std::max({ay, by, cy});
  const i32 min_z = z_order(x0, y0, min_x, min_y, inv_size), max_z = z_order(x1, y1, min_x, min_y, inv_size);
  const Node* p = ear->prev_z;
  const Node* n = ear->next_z;
  auto inside = [&](const Node* q) {
    return q->x >= x0 && q->x <= x1 && q->y >= y0 && q->y <= y1 && q != a && q != c &&
           point_in_triangle_except_first(ax, ay, bx, by, cx, cy, q->x, q->y) && AREA(q->prev, q, q->next) >= 0;
  };
  while (p && p->z >= min_z && n && n->z <= max_z) {
    if (inside(p)) return false;
    p = p->prev_z;
    if (inside(n)) return false;
    n = n->next_z;
  }
  while (p && p->z >= min_z) {
    if (inside(p)) return false;
    p = p->prev_z;
  }
  while (n && n->z <= max_z) {
    if (inside(n)) return false;
    n = n->next_z;
  }
  return true;
}

namespace {

bool on_segment(f64 px, f64 py, f64 qx, f64 qy, f64 rx, f64 ry) {
  return qx <= std::max(px, rx) && qx >= std::min(px, rx) && qy <= std::max(py, ry) && qy >= std::min(py, ry);
}

}  // namespace

#define INTERSECTS(p1, q1, p2, q2) intersects_xy((p1)->x, (p1)->y, (q1)->x, (q1)->y, (p2)->x, (p2)->y, (q2)->x, (q2)->y)

static bool intersects_xy(f64 p1x, f64 p1y, f64 q1x, f64 q1y, f64 p2x, f64 p2y, f64 q2x, f64 q2y) {
  const int o1 = sign(tri_area(p1x, p1y, q1x, q1y, p2x, p2y));
  const int o2 = sign(tri_area(p1x, p1y, q1x, q1y, q2x, q2y));
  const int o3 = sign(tri_area(p2x, p2y, q2x, q2y, p1x, p1y));
  const int o4 = sign(tri_area(p2x, p2y, q2x, q2y, q1x, q1y));
  if (o1 != o2 && o3 != o4) return true;
  if (o1 == 0 && on_segment(p1x, p1y, p2x, p2y, q1x, q1y)) return true;
  if (o2 == 0 && on_segment(p1x, p1y, q2x, q2y, q1x, q1y)) return true;
  if (o3 == 0 && on_segment(p2x, p2y, p1x, p1y, q2x, q2y)) return true;
  if (o4 == 0 && on_segment(p2x, p2y, q1x, q1y, q2x, q2y)) return true;
  return false;
}

template <class N>
static bool locally_inside(const N* a, const N* b) {
  return AREA(a->prev, a, a->next) < 0 ? AREA(a, b, a->next) >= 0 && AREA(a, a->prev, b) >= 0
                                       : AREA(a, b, a->prev) < 0 || AREA(a, a->next, b) < 0;
}

template <class N>
static bool middle_inside(const N* a, const N* b) {
  const N* p = a;
  bool inside = false;
  const f64 px = (a->x + b->x) / 2, py = (a->y + b->y) / 2;
  do {
    if (((p->y > py) != (p->next->y > py)) && p->next->y != p->y && (px < (p->next->x - p->x) * (py - p->y) / (p->next->y - p->y) + p->x))
      inside = !inside;
    p = p->next;
  } while (p != a);
  return inside;
}

template <class N>
static bool intersects_polygon(const N* a, const N* b) {
  const N* p = a;
  do {
    if (p->i != a->i && p->next->i != a->i && p->i != b->i && p->next->i != b->i && INTERSECTS(p, p->next, a, b)) return true;
    p = p->next;
  } while (p != a);
  return false;
}

template <class N>
static bool is_valid_diagonal(const N* a, const N* b) {
  return a->next->i != b->i && a->prev->i != b->i && !intersects_polygon(a, b) &&
         ((locally_inside(a, b) && locally_inside(b, a) && middle_inside(a, b) && (AREA(a->prev, a, b->prev) != 0 || AREA(a, b->prev, b) != 0)) ||
          (EQUALS(a, b) && AREA(a->prev, a, a->next) > 0 && AREA(b->prev, b, b->next) > 0));
}

template <class N>
static bool sector_contains_sector(const N* m, const N* p) {
  return AREA(m->prev, m, p->prev) < 0 && AREA(p->next, m, m->next) < 0;
}

Earcut::Node* Earcut::cure_local_intersections(Node* start, std::vector<u32>& out) {
  Node* p = start;
  do {
    Node* a = p->prev;
    Node* b = p->next->next;
    if (!EQUALS(a, b) && INTERSECTS(a, p, p->next, b) && locally_inside(a, b) && locally_inside(b, a)) {
      out.push_back(a->i);
      out.push_back(p->i);
      out.push_back(b->i);
      remove(p);
      remove(p->next);
      p = start = b;
    }
    p = p->next;
  } while (p != start);
  return filter_points(p);
}

void Earcut::split_earcut(Node* start, std::vector<u32>& out, f64 min_x, f64 min_y, f64 inv_size) {
  Node* a = start;
  do {
    Node* b = a->next->next;
    while (b != a->prev) {
      if (a->i != b->i && is_valid_diagonal(a, b)) {
        Node* c = split_polygon(a, b);
        a = filter_points(a, a->next);
        c = filter_points(c, c->next);
        earcut_linked(a, out, min_x, min_y, inv_size, 0);
        earcut_linked(c, out, min_x, min_y, inv_size, 0);
        return;
      }
      b = b->next;
    }
    a = a->next;
  } while (a != start);
}

Earcut::Node* Earcut::eliminate_holes(const f64* xy, u32 vertex_count, const u32* hole_starts, u32 hole_count, Node* outer) {
  std::vector<Node*> queue;
  queue.reserve(hole_count);
  for (u32 i = 0; i < hole_count; ++i) {
    const u32 start = hole_starts[i];
    const u32 end = i < hole_count - 1 ? hole_starts[i + 1] : vertex_count;
    Node* list = linked_list(xy, start, end, false);
    if (!list) continue;
    if (list == list->next) list->steiner = true;
    // leftmost node of the ring
    Node* p = list;
    Node* leftmost = list;
    do {
      if (p->x < leftmost->x || (p->x == leftmost->x && p->y < leftmost->y)) leftmost = p;
      p = p->next;
    } while (p != list);
    queue.push_back(leftmost);
  }
  std::stable_sort(queue.begin(), queue.end(), [](const Node* a, const Node* b) {
    f64 result = a->x - b->x;
    if (result == 0) {
      result = a->y - b->y;
      if (result == 0) {
        const f64 a_slope = (a->next->y - a->y) / (a->next->x - a->x);
        const f64 b_slope = (b->next->y - b->y) / (b->next->x - b->x);
        result = a_slope - b_slope;
      }
    }
    return result < 0;
  });
  for (Node* hole : queue) outer = eliminate_hole(hole, outer);
  return outer;
}

Earcut::Node* Earcut::eliminate_hole(Node* hole, Node* outer) {
  Node* bridge = find_hole_bridge(hole, outer);
  if (!bridge) return outer;
  Node* bridge_reverse = split_polygon(bridge, hole);
  filter_points(bridge_reverse, bridge_reverse->next);
  return filter_points(bridge, bridge->next);
}

Earcut::Node* Earcut::find_hole_bridge(Node* hole, Node* outer) {
  Node* p = outer;
  const f64 hx = hole->x, hy = hole->y;
  f64 qx = -INFINITY;
  Node* m = nullptr;
  if (EQUALS(hole, p)) return p;
  do {
    if (EQUALS(hole, p->next)) return p->next;
    if (hy <= p->y && hy >= p->next->y && p->next->y != p->y) {
      const f64 x = p->x + (hy - p->y) * (p->next->x - p->x) / (p->next->y - p->y);
      if (x <= hx && x > qx) {
        qx = x;
        m = p->x < p->next->x ? p : p->next;
        if (x == hx) return m;
      }
    }
    p = p->next;
  } while (p != outer);
  if (!m) return nullptr;
  Node* stop = m;
  const f64 mx = m->x, my = m->y;
  f64 tan_min = INFINITY;
  p = m;
  do {
    if (hx >= p->x && p->x >= mx && hx != p->x &&
        point_in_triangle(hy < my ? hx : qx, hy, mx, my, hy < my ? qx : hx, hy, p->x, p->y)) {
      const f64 tan = std::fabs(hy - p->y) / (hx - p->x);
      if (locally_inside(p, hole) && (tan < tan_min || (tan == tan_min && (p->x > m->x || (p->x == m->x && sector_contains_sector(m, p)))))) {
        m = p;
        tan_min = tan;
      }
    }
    p = p->next;
  } while (p != stop);
  return m;
}

void Earcut::index_curve(Node* start, f64 min_x, f64 min_y, f64 inv_size) {
  Node* p = start;
  do {
    if (p->z == 0) p->z = z_order(p->x, p->y, min_x, min_y, inv_size);
    p->prev_z = p->prev;
    p->next_z = p->next;
    p = p->next;
  } while (p != start);
  p->prev_z->next_z = nullptr;
  p->prev_z = nullptr;
  sort_linked(p);
}

Earcut::Node* Earcut::sort_linked(Node* list) {
  int num_merges;
  int in_size = 1;
  do {
    Node* p = list;
    Node* e = nullptr;
    list = nullptr;
    Node* tail = nullptr;
    num_merges = 0;
    while (p) {
      ++num_merges;
      Node* q = p;
      int p_size = 0;
      for (int i = 0; i < in_size; ++i) {
        ++p_size;
        q = q->next_z;
        if (!q) break;
      }
      int q_size = in_size;
      while (p_size > 0 || (q_size > 0 && q)) {
        if (p_size != 0 && (q_size == 0 || !q || p->z <= q->z)) {
          e = p;
          p = p->next_z;
          --p_size;
        } else {
          e = q;
          q = q->next_z;
          --q_size;
        }
        if (tail) tail->next_z = e;
        else list = e;
        e->prev_z = tail;
        tail = e;
      }
      p = q;
    }
    tail->next_z = nullptr;
    in_size *= 2;
  } while (num_merges > 1);
  return list;
}

Earcut::Node* Earcut::split_polygon(Node* a, Node* b) {
  Node* a2 = create(a->i, a->x, a->y);
  Node* b2 = create(b->i, b->x, b->y);
  Node* an = a->next;
  Node* bp = b->prev;
  a->next = b;
  b->prev = a;
  a2->next = an;
  an->prev = a2;
  b2->next = a2;
  a2->prev = b2;
  bp->next = b2;
  b2->prev = bp;
  return b2;
}

void Earcut::triangulate(const f64* xy, u32 vertex_count, const u32* hole_starts, u32 hole_count, std::vector<u32>& out) {
  used_ = 0;
  const bool has_holes = hole_count > 0;
  const u32 outer_len = has_holes ? hole_starts[0] : vertex_count;
  Node* outer = linked_list(xy, 0, outer_len, true);
  if (!outer || outer->next == outer->prev) return;
  f64 min_x = 0, min_y = 0, inv_size = 0;
  if (has_holes) outer = eliminate_holes(xy, vertex_count, hole_starts, hole_count, outer);
  if (vertex_count > 80) {
    min_x = xy[0];
    min_y = xy[1];
    f64 max_x = min_x, max_y = min_y;
    for (u32 i = 1; i < outer_len; ++i) {
      const f64 x = xy[i * 2], y = xy[i * 2 + 1];
      if (x < min_x) min_x = x;
      if (y < min_y) min_y = y;
      if (x > max_x) max_x = x;
      if (y > max_y) max_y = y;
    }
    inv_size = std::max(max_x - min_x, max_y - min_y);
    inv_size = inv_size != 0 ? 32767 / inv_size : 0;
  }
  earcut_linked(outer, out, min_x, min_y, inv_size, 0);
}

f64 Earcut::deviation(const f64* xy, u32 vertex_count, const u32* hole_starts, u32 hole_count, const u32* t, u32 index_count) {
  const u32 outer_len = hole_count ? hole_starts[0] : vertex_count;
  f64 polygon_area = std::fabs(signed_area(xy, 0, outer_len));
  for (u32 i = 0; i < hole_count; ++i) {
    const u32 start = hole_starts[i], end = i < hole_count - 1 ? hole_starts[i + 1] : vertex_count;
    polygon_area -= std::fabs(signed_area(xy, start, end));
  }
  f64 triangles_area = 0;
  for (u32 i = 0; i < index_count; i += 3) {
    const u32 a = t[i] * 2, b = t[i + 1] * 2, c = t[i + 2] * 2;
    triangles_area += std::fabs((xy[a] - xy[c]) * (xy[b + 1] - xy[a + 1]) - (xy[a] - xy[b]) * (xy[c + 1] - xy[a + 1]));
  }
  return polygon_area == 0 && triangles_area == 0 ? 0 : std::fabs((triangles_area - polygon_area) / polygon_area);
}

#undef AREA
#undef EQUALS
#undef INTERSECTS

}  // namespace kern
