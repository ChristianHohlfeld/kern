// GPU program wrapper and a redundant-state filter for uniforms.
#pragma once

#include <string_view>
#include <vector>

#include "kern/gfx/gl.h"
#include "kern/math/math.h"

namespace kern {

/// A linked vertex + fragment program. Uniform locations are resolved once
/// after linking (location()) and kept by the owner.
class Program {
 public:
  Program() = default;
  Program(const Program&) = delete;
  Program& operator=(const Program&) = delete;
  ~Program() { destroy(false); }

  /// Compiles and links. On failure records the driver log with fail().
  bool build(std::string_view vertex, std::string_view fragment, const char* label);
  /// contextLost: the handles are already dead, only forget them.
  void destroy(bool contextLost);
  u32 handle() const { return handle_; }
  bool valid() const { return handle_ != 0; }
  /// 0 when the uniform is not active in the linked program.
  u32 location(const char* name) const;
  void use() const { gl::useProgram(handle_); }

 private:
  u32 handle_ = 0;
};

/// Remembers the last value sent to each uniform location of ONE program and
/// skips gl.uniform* calls that would not change anything. Uniform values are
/// program state in GL, so the cache stays valid across frames and program
/// switches; reset() is only needed after the program is (re)linked.
class UniformCache {
 public:
  void reset() { entries_.clear(); }
  void set_int(u32 location, i32 v);
  void set_float(u32 location, f32 v);
  void set_vec2(u32 location, f32 x, f32 y);
  void set_vec3(u32 location, Vec3 v);
  void set_vec4(u32 location, Vec4 v);
  /// Arrays and matrices are compared by content (up to 64 floats cached).
  void set_vec4_array(u32 location, const f32* values, i32 count);
  void set_vec3_array(u32 location, const f32* values, i32 count);
  void set_mat4(u32 location, const f32* values);
  /// Uncached upload (large or always-changing data such as bone palettes).
  static void set_mat4_array(u32 location, const f32* values, i32 count) {
    if (location) gl::uniformMatrix4fv(location, values, count);
  }

 private:
  struct Entry {
    u8 kind = 0;  // 0 unset, 1 int, 2 float values
    u8 count = 0;
    i32 i = 0;
    f32 f[4] = {};
    u64 signature = 0;  // content hash for arrays / matrices
  };
  Entry& entry(u32 location);
  bool same_floats(u32 location, const f32* values, u32 count);
  std::vector<Entry> entries_;
};

}  // namespace kern
