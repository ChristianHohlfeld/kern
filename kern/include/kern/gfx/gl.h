// WebGL2 entry points imported from the platform layer (kern/web/kern.mjs).
//
// GPU objects are opaque u32 handles (0 = null) managed by the host. Data
// arguments are pointers into the module's linear memory plus a byte count.
// The set below is exactly what the engine uses; adding an entry point means
// adding it here and in the host's `kern_gl` import object.
#pragma once

#include "kern/core/base.h"

#if defined(__wasm__)
#define KERN_GL(name) __attribute__((import_module("kern_gl"), import_name(#name)))
#else
#define KERN_GL(name)
#endif

namespace kern::gl {

// ---- enums (WebGL2 values) -------------------------------------------------
enum : u32 {
  NONE = 0,
  ONE = 1,
  POINTS = 0x0000,
  LINES = 0x0001,
  TRIANGLES = 0x0004,
  DEPTH_BUFFER_BIT = 0x0100,
  COLOR_BUFFER_BIT = 0x4000,
  LESS = 0x0201,
  LEQUAL = 0x0203,
  ALWAYS = 0x0207,
  SRC_ALPHA = 0x0302,
  ONE_MINUS_SRC_ALPHA = 0x0303,
  FRONT = 0x0404,
  BACK = 0x0405,
  CULL_FACE = 0x0B44,
  DEPTH_TEST = 0x0B71,
  BLEND = 0x0BE2,
  SCISSOR_TEST = 0x0C11,
  TEXTURE_2D = 0x0DE1,
  UNSIGNED_BYTE = 0x1401,
  UNSIGNED_SHORT = 0x1403,
  UNSIGNED_INT = 0x1405,
  FLOAT = 0x1406,
  HALF_FLOAT = 0x140B,
  DEPTH_COMPONENT = 0x1902,
  RGBA = 0x1908,
  NEAREST = 0x2600,
  LINEAR = 0x2601,
  LINEAR_MIPMAP_LINEAR = 0x2703,
  TEXTURE_MAG_FILTER = 0x2800,
  TEXTURE_MIN_FILTER = 0x2801,
  TEXTURE_WRAP_S = 0x2802,
  TEXTURE_WRAP_T = 0x2803,
  REPEAT = 0x2901,
  POLYGON_OFFSET_FILL = 0x8037,
  RGBA8 = 0x8058,
  CLAMP_TO_EDGE = 0x812F,
  DEPTH_COMPONENT24 = 0x81A6,
  TEXTURE0 = 0x84C0,
  RGBA16F = 0x881A,
  TEXTURE_COMPARE_MODE = 0x884C,
  TEXTURE_COMPARE_FUNC = 0x884D,
  COMPARE_REF_TO_TEXTURE = 0x884E,
  ARRAY_BUFFER = 0x8892,
  ELEMENT_ARRAY_BUFFER = 0x8893,
  STREAM_DRAW = 0x88E0,
  STATIC_DRAW = 0x88E4,
  DYNAMIC_DRAW = 0x88E8,
  TIME_ELAPSED = 0x88BF,
  FRAGMENT_SHADER = 0x8B30,
  VERTEX_SHADER = 0x8B31,
  COMPILE_STATUS = 0x8B81,
  LINK_STATUS = 0x8B82,
  FRAMEBUFFER_COMPLETE = 0x8CD5,
  COLOR_ATTACHMENT0 = 0x8CE0,
  COLOR_ATTACHMENT1 = 0x8CE1,
  DEPTH_ATTACHMENT = 0x8D00,
  FRAMEBUFFER = 0x8D40,
  RENDERBUFFER = 0x8D41,
};

extern "C" {
// buffers & vertex arrays
KERN_GL(createBuffer) u32 createBuffer();
KERN_GL(deleteBuffer) void deleteBuffer(u32 buffer);
KERN_GL(bindBuffer) void bindBuffer(u32 target, u32 buffer);
/// data == nullptr allocates `bytes` uninitialised bytes.
KERN_GL(bufferData) void bufferData(u32 target, const void* data, u32 bytes, u32 usage);
KERN_GL(bufferSubData) void bufferSubData(u32 target, u32 offset, const void* data, u32 bytes);
KERN_GL(createVertexArray) u32 createVertexArray();
KERN_GL(deleteVertexArray) void deleteVertexArray(u32 vao);
KERN_GL(bindVertexArray) void bindVertexArray(u32 vao);
KERN_GL(enableVertexAttribArray) void enableVertexAttribArray(u32 index);
KERN_GL(vertexAttribPointer) void vertexAttribPointer(u32 index, i32 size, u32 type, i32 normalized, i32 stride, i32 offset);
KERN_GL(vertexAttribDivisor) void vertexAttribDivisor(u32 index, u32 divisor);

// textures
KERN_GL(createTexture) u32 createTexture();
KERN_GL(deleteTexture) void deleteTexture(u32 texture);
KERN_GL(bindTexture) void bindTexture(u32 target, u32 texture);
KERN_GL(activeTexture) void activeTexture(u32 unit);
KERN_GL(texImage2D) void texImage2D(u32 target, i32 level, i32 internalFormat, i32 width, i32 height, u32 format, u32 type,
                                    const void* data, u32 bytes);
KERN_GL(texParameteri) void texParameteri(u32 target, u32 pname, i32 value);
KERN_GL(generateMipmap) void generateMipmap(u32 target);

// framebuffers
KERN_GL(createFramebuffer) u32 createFramebuffer();
KERN_GL(deleteFramebuffer) void deleteFramebuffer(u32 framebuffer);
KERN_GL(bindFramebuffer) void bindFramebuffer(u32 target, u32 framebuffer);
KERN_GL(framebufferTexture2D) void framebufferTexture2D(u32 target, u32 attachment, u32 texTarget, u32 texture, i32 level);
KERN_GL(checkFramebufferStatus) u32 checkFramebufferStatus(u32 target);
KERN_GL(drawBuffers) void drawBuffers(const u32* buffers, i32 count);
KERN_GL(readBuffer) void readBuffer(u32 source);
KERN_GL(createRenderbuffer) u32 createRenderbuffer();
KERN_GL(deleteRenderbuffer) void deleteRenderbuffer(u32 renderbuffer);
KERN_GL(bindRenderbuffer) void bindRenderbuffer(u32 target, u32 renderbuffer);
KERN_GL(renderbufferStorage) void renderbufferStorage(u32 target, u32 internalFormat, i32 width, i32 height);
KERN_GL(framebufferRenderbuffer) void framebufferRenderbuffer(u32 target, u32 attachment, u32 rbTarget, u32 renderbuffer);

// shaders & programs
KERN_GL(createShader) u32 createShader(u32 type);
KERN_GL(shaderSource) void shaderSource(u32 shader, const char* source, u32 length);
KERN_GL(compileShader) void compileShader(u32 shader);
KERN_GL(getShaderParameter) i32 getShaderParameter(u32 shader, u32 pname);
/// Writes at most `capacity` bytes, returns the number written.
KERN_GL(getShaderInfoLog) u32 getShaderInfoLog(u32 shader, char* out, u32 capacity);
KERN_GL(deleteShader) void deleteShader(u32 shader);
KERN_GL(createProgram) u32 createProgram();
KERN_GL(attachShader) void attachShader(u32 program, u32 shader);
KERN_GL(linkProgram) void linkProgram(u32 program);
KERN_GL(getProgramParameter) i32 getProgramParameter(u32 program, u32 pname);
KERN_GL(getProgramInfoLog) u32 getProgramInfoLog(u32 program, char* out, u32 capacity);
KERN_GL(deleteProgram) void deleteProgram(u32 program);
KERN_GL(useProgram) void useProgram(u32 program);
/// Returns 0 when the uniform is not active.
KERN_GL(getUniformLocation) u32 getUniformLocation(u32 program, const char* name, u32 length);

// uniforms
KERN_GL(uniform1i) void uniform1i(u32 location, i32 v);
KERN_GL(uniform1f) void uniform1f(u32 location, f32 v);
KERN_GL(uniform2f) void uniform2f(u32 location, f32 x, f32 y);
KERN_GL(uniform3f) void uniform3f(u32 location, f32 x, f32 y, f32 z);
KERN_GL(uniform4f) void uniform4f(u32 location, f32 x, f32 y, f32 z, f32 w);
KERN_GL(uniform3fv) void uniform3fv(u32 location, const f32* values, i32 count);
KERN_GL(uniform4fv) void uniform4fv(u32 location, const f32* values, i32 count);
KERN_GL(uniformMatrix4fv) void uniformMatrix4fv(u32 location, const f32* values, i32 count);

// state & drawing
KERN_GL(enable) void enable(u32 cap);
KERN_GL(disable) void disable(u32 cap);
KERN_GL(depthFunc) void depthFunc(u32 func);
KERN_GL(depthMask) void depthMask(i32 flag);
KERN_GL(blendFunc) void blendFunc(u32 src, u32 dst);
KERN_GL(cullFace) void cullFace(u32 mode);
KERN_GL(colorMask) void colorMask(i32 r, i32 g, i32 b, i32 a);
KERN_GL(clearColor) void clearColor(f32 r, f32 g, f32 b, f32 a);
KERN_GL(clearDepth) void clearDepth(f32 depth);
KERN_GL(clear) void clear(u32 mask);
KERN_GL(viewport) void viewport(i32 x, i32 y, i32 width, i32 height);
KERN_GL(scissor) void scissor(i32 x, i32 y, i32 width, i32 height);
KERN_GL(polygonOffset) void polygonOffset(f32 factor, f32 units);
KERN_GL(drawArrays) void drawArrays(u32 mode, i32 first, i32 count);
KERN_GL(drawArraysInstanced) void drawArraysInstanced(u32 mode, i32 first, i32 count, i32 instances);
KERN_GL(drawElementsInstanced) void drawElementsInstanced(u32 mode, i32 count, u32 type, i32 offset, i32 instances);

// capabilities & timer queries
/// 1 when the named WebGL extension is available (and enabled).
KERN_GL(getExtension) i32 getExtension(const char* name, u32 length);
KERN_GL(createQuery) u32 createQuery();
KERN_GL(deleteQuery) void deleteQuery(u32 query);
KERN_GL(beginQuery) void beginQuery(u32 target, u32 query);
KERN_GL(endQuery) void endQuery(u32 target);
KERN_GL(queryAvailable) i32 queryAvailable(u32 query);
KERN_GL(queryResultMs) f64 queryResultMs(u32 query);
KERN_GL(gpuDisjoint) i32 gpuDisjoint();
}

inline bool has_extension(const char* name) { return getExtension(name, static_cast<u32>(std::strlen(name))) != 0; }

}  // namespace kern::gl
