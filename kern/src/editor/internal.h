// Shared helpers of the editor implementation files (not a public header).
#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include "kern/editor/editor.h"

namespace kern::editor {

using namespace kern::world;

struct Editor::ItemRef {
  Mode mode = Mode::body;
  std::string id;
  std::string fixture;  // vertex: owning fixture
  i32 index = -1;       // vertex: index in the hull
  D3 position{};        // vertex: fixture-space position
  std::string name;
};

inline u32 mode_index(Mode m) { return static_cast<u32>(m); }

/// JavaScript Number(text): "" -> 0, invalid -> NaN.
f64 js_number(std::string_view text);
/// clean(n) of the previous editor: 6 decimals, tiny values become 0.
std::string clean_number(f64 v);
std::string capitalize(std::string s);
inline bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
inline f64 deg2rad(f64 d) { return d * kPiD / 180; }
inline f64 rad2deg(f64 r) { return r * 180 / kPiD; }
inline i32 axis_index(char a) { return a == 'y' ? 1 : a == 'z' ? 2 : 0; }

json::Value ok_result();
json::Value error_result(const std::string& message);

/// HTML escaping for help texts the page inserts as markup.
std::string html_escape(std::string_view s);

/// Body pose used for drawing in a view (the runtime pose in player views).
DPose display_pose(const View& v, const Body& b);

/// World-space axis-aligned bounds of everything drawable in a scene.
struct Bounds {
  D3 lo{INFINITY, INFINITY, INFINITY}, hi{-INFINITY, -INFINITY, -INFINITY};
  void add(const D3& p) {
    for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
  }
  bool empty() const { return !(lo[0] <= hi[0]); }
};

/// The world bounds of a fixture placed by `body`.
void add_fixture_bounds(Bounds& b, const DPose& body, const Fixture& f);

}  // namespace kern::editor
