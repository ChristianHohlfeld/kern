#include "kern/gfx/program.h"

namespace kern {

namespace {

u32 compile(u32 type, std::string_view source, const char* label) {
  const u32 shader = gl::createShader(type);
  if (!shader) { fail("%s: createShader failed (context lost?)", label); return 0; }
  gl::shaderSource(shader, source.data(), static_cast<u32>(source.size()));
  gl::compileShader(shader);
  if (!gl::getShaderParameter(shader, gl::COMPILE_STATUS)) {
    char info[768];
    const u32 n = gl::getShaderInfoLog(shader, info, sizeof info - 1);
    info[min_of<u32>(n, sizeof info - 1)] = 0;
    gl::deleteShader(shader);
    fail("%s %s shader: %s", label, type == gl::VERTEX_SHADER ? "vertex" : "fragment", info);
    return 0;
  }
  return shader;
}

}  // namespace

bool Program::build(std::string_view vertex, std::string_view fragment, const char* label) {
  destroy(false);
  const u32 vs = compile(gl::VERTEX_SHADER, vertex, label);
  if (!vs) return false;
  const u32 fs = compile(gl::FRAGMENT_SHADER, fragment, label);
  if (!fs) { gl::deleteShader(vs); return false; }
  const u32 program = gl::createProgram();
  gl::attachShader(program, vs);
  gl::attachShader(program, fs);
  gl::linkProgram(program);
  gl::deleteShader(vs);
  gl::deleteShader(fs);
  if (!gl::getProgramParameter(program, gl::LINK_STATUS)) {
    char info[768];
    const u32 n = gl::getProgramInfoLog(program, info, sizeof info - 1);
    info[min_of<u32>(n, sizeof info - 1)] = 0;
    gl::deleteProgram(program);
    return fail("%s link: %s", label, info);
  }
  handle_ = program;
  return true;
}

void Program::destroy(bool contextLost) {
  if (handle_ && !contextLost) gl::deleteProgram(handle_);
  handle_ = 0;
}

u32 Program::location(const char* name) const {
  return handle_ ? gl::getUniformLocation(handle_, name, static_cast<u32>(std::strlen(name))) : 0;
}

UniformCache::Entry& UniformCache::entry(u32 location) {
  if (location >= entries_.size()) entries_.resize(location + 16);
  return entries_[location];
}

void UniformCache::set_int(u32 location, i32 v) {
  if (!location) return;
  Entry& e = entry(location);
  if (e.kind == 1 && e.i == v) return;
  e.kind = 1; e.i = v;
  gl::uniform1i(location, v);
}

void UniformCache::set_float(u32 location, f32 v) {
  if (!location) return;
  Entry& e = entry(location);
  if (e.kind == 2 && e.count == 1 && e.f[0] == v) return;
  e.kind = 2; e.count = 1; e.f[0] = v;
  gl::uniform1f(location, v);
}

void UniformCache::set_vec2(u32 location, f32 x, f32 y) {
  if (!location) return;
  Entry& e = entry(location);
  if (e.kind == 2 && e.count == 2 && e.f[0] == x && e.f[1] == y) return;
  e.kind = 2; e.count = 2; e.f[0] = x; e.f[1] = y;
  gl::uniform2f(location, x, y);
}

void UniformCache::set_vec3(u32 location, Vec3 v) {
  if (!location) return;
  Entry& e = entry(location);
  if (e.kind == 2 && e.count == 3 && e.f[0] == v.x && e.f[1] == v.y && e.f[2] == v.z) return;
  e.kind = 2; e.count = 3; e.f[0] = v.x; e.f[1] = v.y; e.f[2] = v.z;
  gl::uniform3f(location, v.x, v.y, v.z);
}

void UniformCache::set_vec4(u32 location, Vec4 v) {
  if (!location) return;
  Entry& e = entry(location);
  if (e.kind == 2 && e.count == 4 && e.f[0] == v.x && e.f[1] == v.y && e.f[2] == v.z && e.f[3] == v.w) return;
  e.kind = 2; e.count = 4; e.f[0] = v.x; e.f[1] = v.y; e.f[2] = v.z; e.f[3] = v.w;
  gl::uniform4f(location, v.x, v.y, v.z, v.w);
}

bool UniformCache::same_floats(u32 location, const f32* values, u32 count) {
  Entry& e = entry(location);
  const u64 signature = hash_bytes(values, count * sizeof(f32), 0x9e3779b97f4a7c15ull + count);
  if (e.kind == 3 && e.signature == signature) return true;
  e.kind = 3; e.signature = signature;
  return false;
}

void UniformCache::set_vec4_array(u32 location, const f32* values, i32 count) {
  if (!location || count <= 0) return;
  if (same_floats(location, values, static_cast<u32>(count) * 4)) return;
  gl::uniform4fv(location, values, count);
}

void UniformCache::set_vec3_array(u32 location, const f32* values, i32 count) {
  if (!location || count <= 0) return;
  if (same_floats(location, values, static_cast<u32>(count) * 3)) return;
  gl::uniform3fv(location, values, count);
}

void UniformCache::set_mat4(u32 location, const f32* values) {
  if (!location) return;
  if (same_floats(location, values, 16)) return;
  gl::uniformMatrix4fv(location, values, 1);
}

}  // namespace kern
