// Polygon triangulation: a C++ port of Mapbox Earcut v3.0.2 (ISC licence,
// see kern/LICENSES.md). Same algorithm and the same results as the
// upstream JavaScript: ear slicing with z-order hashing for large rings,
// hole bridging (Eberly), local self-intersection curing and splitting.
#pragma once

#include <vector>

#include "kern/core/base.h"

namespace kern {

/// Triangulates a polygon given as xy pairs (outer ring first, then holes;
/// `hole_starts` are vertex indices where each hole begins). Triangle vertex
/// indices are appended to `out`. Reuses internal storage between calls.
class Earcut {
 public:
  void triangulate(const f64* xy, u32 vertex_count, const u32* hole_starts, u32 hole_count, std::vector<u32>& out);

  /// Relative difference between polygon area and triangulated area (0 = exact).
  static f64 deviation(const f64* xy, u32 vertex_count, const u32* hole_starts, u32 hole_count, const u32* triangles, u32 index_count);

 private:
  struct Node {
    u32 i;
    f64 x, y;
    Node *prev, *next;
    i32 z;
    Node *prev_z, *next_z;
    bool steiner;
  };

  Node* create(u32 i, f64 x, f64 y);
  Node* insert(u32 i, f64 x, f64 y, Node* last);
  static void remove(Node* p);
  Node* linked_list(const f64* xy, u32 start, u32 end, bool clockwise);
  static Node* filter_points(Node* start, Node* end = nullptr);
  void earcut_linked(Node* ear, std::vector<u32>& out, f64 min_x, f64 min_y, f64 inv_size, int pass);
  static bool is_ear(const Node* ear);
  static bool is_ear_hashed(const Node* ear, f64 min_x, f64 min_y, f64 inv_size);
  static Node* cure_local_intersections(Node* start, std::vector<u32>& out);
  void split_earcut(Node* start, std::vector<u32>& out, f64 min_x, f64 min_y, f64 inv_size);
  Node* eliminate_holes(const f64* xy, u32 vertex_count, const u32* hole_starts, u32 hole_count, Node* outer);
  Node* eliminate_hole(Node* hole, Node* outer);
  static Node* find_hole_bridge(Node* hole, Node* outer);
  static void index_curve(Node* start, f64 min_x, f64 min_y, f64 inv_size);
  static Node* sort_linked(Node* list);
  Node* split_polygon(Node* a, Node* b);

  // Stable node storage: blocks are never moved while a triangulation runs.
  std::vector<Node*> blocks_;
  u32 used_ = 0;  // nodes used in total across blocks
  static constexpr u32 kBlock = 512;
 public:
  Earcut() = default;
  Earcut(const Earcut&) = delete;
  Earcut& operator=(const Earcut&) = delete;
  ~Earcut();
};

}  // namespace kern
