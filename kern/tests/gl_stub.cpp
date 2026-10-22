// Host-side WebGL2 stand-in for unit tests: hands out handles, accepts every
// call and counts what the engine sends, so renderer logic (validation,
// caching, context loss, draw lists) can be tested without a GPU.
#include "gl_stub.h"

#include <string>
#include <unordered_map>

#include "kern/gfx/gl.h"

using namespace kern;

GlStub g_gl;

namespace {
u32 g_next = 1;
std::unordered_map<std::string, u32> g_locations;
u32 next_handle() { return g_next++; }
}  // namespace

void GlStub::reset() { *this = GlStub{}; }

namespace kern::gl {
extern "C" {
u32 createBuffer() { g_gl.buffers_created++; return next_handle(); }
void deleteBuffer(u32) { g_gl.buffers_deleted++; }
void bindBuffer(u32, u32) {}
void bufferData(u32, const void*, u32 bytes, u32) { g_gl.buffer_bytes += bytes; g_gl.buffer_data_calls++; }
void bufferSubData(u32, u32, const void*, u32 bytes) { g_gl.buffer_bytes += bytes; g_gl.buffer_sub_data_calls++; }
u32 createVertexArray() { g_gl.vaos_created++; return next_handle(); }
void deleteVertexArray(u32) { g_gl.vaos_deleted++; }
void bindVertexArray(u32) {}
void enableVertexAttribArray(u32) {}
void vertexAttribPointer(u32, i32, u32, i32, i32, i32) {}
void vertexAttribDivisor(u32, u32) {}
u32 createTexture() { g_gl.textures_created++; return next_handle(); }
void deleteTexture(u32) { g_gl.textures_deleted++; }
void bindTexture(u32, u32) { g_gl.texture_binds++; }
void activeTexture(u32) {}
void texImage2D(u32, i32, i32, i32, i32, u32, u32, const void*, u32) {}
void texParameteri(u32, u32, i32) {}
void generateMipmap(u32) {}
u32 createFramebuffer() { return next_handle(); }
void deleteFramebuffer(u32) {}
void bindFramebuffer(u32, u32) {}
void framebufferTexture2D(u32, u32, u32, u32, i32) {}
u32 checkFramebufferStatus(u32) { return FRAMEBUFFER_COMPLETE; }
void drawBuffers(const u32*, i32) {}
void readBuffer(u32) {}
u32 createRenderbuffer() { return next_handle(); }
void deleteRenderbuffer(u32) {}
void bindRenderbuffer(u32, u32) {}
void renderbufferStorage(u32, u32, i32, i32) {}
void framebufferRenderbuffer(u32, u32, u32, u32) {}
u32 createShader(u32) { return next_handle(); }
void shaderSource(u32, const char*, u32 length) { g_gl.shader_source_bytes += length; }
void compileShader(u32) {}
i32 getShaderParameter(u32, u32) { return 1; }
u32 getShaderInfoLog(u32, char*, u32) { return 0; }
void deleteShader(u32) {}
u32 createProgram() { g_gl.programs_created++; return next_handle(); }
void attachShader(u32, u32) {}
void linkProgram(u32) {}
i32 getProgramParameter(u32, u32) { return g_gl.fail_link ? 0 : 1; }
u32 getProgramInfoLog(u32, char*, u32) { return 0; }
void deleteProgram(u32) { g_gl.programs_deleted++; }
void useProgram(u32) {}
u32 getUniformLocation(u32 program, const char* name, u32 length) {
  const std::string key = std::to_string(program) + ":" + std::string(name, length);
  auto it = g_locations.find(key);
  if (it != g_locations.end()) return it->second;
  const u32 id = static_cast<u32>(g_locations.size() + 1);
  g_locations.emplace(key, id);
  return id;
}
void uniform1i(u32, i32) { g_gl.uniform_calls++; }
void uniform1f(u32, f32) { g_gl.uniform_calls++; }
void uniform2f(u32, f32, f32) { g_gl.uniform_calls++; }
void uniform3f(u32, f32, f32, f32) { g_gl.uniform_calls++; }
void uniform4f(u32, f32, f32, f32, f32) { g_gl.uniform_calls++; }
void uniform3fv(u32, const f32*, i32) { g_gl.uniform_calls++; }
void uniform4fv(u32, const f32*, i32) { g_gl.uniform_calls++; }
void uniformMatrix4fv(u32, const f32*, i32) { g_gl.uniform_calls++; }
void enable(u32) {}
void disable(u32) {}
void depthFunc(u32) {}
void depthMask(i32) {}
void blendFunc(u32, u32) {}
void cullFace(u32 mode) { (mode == FRONT ? g_gl.cull_front : g_gl.cull_back)++; }
void colorMask(i32, i32, i32, i32) {}
void clearColor(f32, f32, f32, f32) {}
void clearDepth(f32) {}
void clear(u32) {}
void viewport(i32, i32, i32, i32) {}
void scissor(i32, i32, i32, i32) {}
void polygonOffset(f32, f32) {}
void drawArrays(u32, i32, i32) { g_gl.draws++; }
void drawArraysInstanced(u32, i32, i32, i32 instances) { g_gl.draws++; g_gl.instances += instances; }
void drawElementsInstanced(u32, i32, u32, i32, i32 instances) { g_gl.draws++; g_gl.instances += instances; }
i32 getExtension(const char*, u32) { return g_gl.extensions_available ? 1 : 0; }
u32 createQuery() { g_gl.queries_created++; return next_handle(); }
void deleteQuery(u32) {}
void beginQuery(u32, u32) {}
void endQuery(u32) {}
i32 queryAvailable(u32) { return 1; }
f64 queryResultMs(u32) { return 1.5; }
i32 gpuDisjoint() { return 0; }
}
}  // namespace kern::gl
