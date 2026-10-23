// Ground-Truth Ambient Occlusion, see passes.h.
//
// References:
//   Jimenez, Wu, Pesce, Jarabo: "Practical Real-Time Strategies for Accurate
//   Indirect Occlusion", SIGGRAPH 2016 course / Activision tech report.
//   Intel XeGTAO (MIT): https://github.com/GameTechDev/XeGTAO - slice and
//   horizon math, falloff, sample distribution and quality presets follow it.
#include <cmath>

#include "kern/gfx/passes.h"

namespace kern {

namespace {

constexpr f64 kSlowFrameMs = 27, kSlowForMs = 3500;

constexpr const char* kFullscreenVertex = R"GLSL(#version 300 es
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// Main pass: one AO texel per fragment, written as (visibility, view depth).
constexpr const char* kMain = R"GLSL(#version 300 es
precision highp float;
precision highp sampler2D;
uniform sampler2D tDepth;   // hardware depth of the scene, full resolution
uniform vec2 uFullSize, uAoSize;
uniform vec4 uProj;         // P[0][0], P[1][1], P[2][2], P[3][2] of the GL perspective matrix
uniform float uRadius, uPower;
uniform int uSlices, uSteps;
out vec4 o;
const float PI = 3.14159265358979, HALF_PI = 1.57079632679490;

float viewDepth(float d) { return uProj.w / ((d * 2.0 - 1.0) + uProj.z); }
vec3 viewPos(vec2 uv, float w) { vec2 ndc = uv * 2.0 - 1.0; return vec3(ndc.x * w / uProj.x, ndc.y * w / uProj.y, -w); }
// Position of the depth texel under uv, at that texel's own centre: a point rebuilt at another uv than its
// depth was read from sits off the surface on slopes and occludes itself (banding at half resolution).
vec3 posAt(vec2 uv) {
  ivec2 t = clamp(ivec2(uv * uFullSize), ivec2(0), ivec2(uFullSize) - 1);
  return viewPos((vec2(t) + 0.5) / uFullSize, viewDepth(texelFetch(tDepth, t, 0).r));
}
// XeGTAO_FastACos (Lagarde): max error 0.0068 rad.
float fastAcos(float x) {
  float a = abs(x), r = (-0.156583 * a + HALF_PI) * sqrt(1.0 - a);
  return x >= 0.0 ? r : PI - r;
}
// 4x4 ordered pattern: every 4x4 block holds each slice rotation once, so the 4x4 denoise integrates it exactly.
float bayer(ivec2 p) {
  int i = (p.x & 3) + (p.y & 3) * 4;
  const float b[16] = float[16](0., 8., 2., 10., 12., 4., 14., 6., 3., 11., 1., 9., 15., 7., 13., 5.);
  return b[i];
}

void main() {
  // The full-resolution texel this AO texel stands for (top-left of its 2x2 block at half resolution).
  ivec2 home = clamp(ivec2(floor(gl_FragCoord.xy) * uFullSize / uAoSize), ivec2(0), ivec2(uFullSize) - 1);
  vec2 uv = (vec2(home) + 0.5) / uFullSize;
  float d = texelFetch(tDepth, home, 0).r;
  if (d >= 1.0) { o = vec4(1.0, 65000.0, 0.0, 1.0); return; }
  float w = viewDepth(d);
  vec3 P = viewPos(uv, w);

  // Normal from depth: per axis the neighbour on the same surface (smaller depth step).
  vec2 px = 1.0 / uFullSize;
  vec3 L = posAt(uv - vec2(px.x, 0.0)), R = posAt(uv + vec2(px.x, 0.0));
  vec3 D = posAt(uv - vec2(0.0, px.y)), U = posAt(uv + vec2(0.0, px.y));
  vec3 dx = abs(R.z - P.z) < abs(P.z - L.z) ? R - P : P - L;
  vec3 dy = abs(U.z - P.z) < abs(P.z - D.z) ? U - P : P - D;
  vec3 V = normalize(-P);
  vec3 N = normalize(cross(dx, dy));
  if (dot(N, V) < 0.0) N = -N;

  // Effect radius in AO pixels at this depth (capped: very close surfaces would sample the whole screen).
  float pxPerUnit = uProj.x * 0.5 * uAoSize.x / w;
  float screenRadius = min(uRadius * pxPerUnit, uAoSize.y * 0.2);
  if (screenRadius < 1.5) { o = vec4(1.0, w, 0.0, 1.0); return; }
  float falloffRange = 0.615 * uRadius;
  float falloffMul = -1.0 / falloffRange, falloffAdd = (uRadius - falloffRange) / falloffRange + 1.0;
  float minS = 1.3 / screenRadius;

  ivec2 pix = ivec2(gl_FragCoord.xy);
  float sliceNoise = (bayer(pix) + 0.5) / 16.0;
  float stepNoise = fract((bayer(pix.yx) + 0.5) / 16.0 + 0.3125);

  float visibility = 0.0;
  for (int slice = 0; slice < uSlices; ++slice) {
    float phi = (float(slice) + sliceNoise) / float(uSlices) * PI;
    vec2 omega = vec2(cos(phi), sin(phi));
    vec3 dir = vec3(omega, 0.0);
    vec3 orthoDir = dir - dot(dir, V) * V;
    vec3 axis = normalize(cross(orthoDir, V));
    vec3 projN = N - axis * dot(N, axis);
    float projLen = length(projN);
    float signN = sign(dot(orthoDir, projN));
    float cosN = clamp(dot(projN, V) / max(projLen, 1e-5), 0.0, 1.0);
    float n = signN * fastAcos(cosN);
    float low0 = cos(n + HALF_PI), low1 = cos(n - HALF_PI);
    float hc0 = low0, hc1 = low1;
    for (int s = 0; s < uSteps; ++s) {
      float sn = fract(stepNoise + float(slice + s * uSteps) * 0.6180339887);
      float t = (float(s) + sn) / float(uSteps);
      t = t * t + minS;  // sample distribution power 2, skip the centre texel
      vec2 offset = round(t * omega * screenRadius) / uAoSize;
      vec3 d0 = posAt(uv + offset) - P, d1 = posAt(uv - offset) - P;
      float l0 = length(d0), l1 = length(d1);
      float c0 = dot(d0, V) / max(l0, 1e-5), c1 = dot(d1, V) / max(l1, 1e-5);
      c0 = mix(low0, c0, clamp(l0 * falloffMul + falloffAdd, 0.0, 1.0));
      c1 = mix(low1, c1, clamp(l1 * falloffMul + falloffAdd, 0.0, 1.0));
      hc0 = max(hc0, c0);
      hc1 = max(hc1, c1);
    }
    float h0 = -fastAcos(clamp(hc1, -1.0, 1.0)), h1 = fastAcos(clamp(hc0, -1.0, 1.0));
    h0 = n + clamp(h0 - n, -HALF_PI, HALF_PI);
    h1 = n + clamp(h1 - n, -HALF_PI, HALF_PI);
    float sinN = sin(n);
    float arc0 = (cosN + 2.0 * h0 * sinN - cos(2.0 * h0 - n)) * 0.25;
    float arc1 = (cosN + 2.0 * h1 * sinN - cos(2.0 * h1 - n)) * 0.25;
    visibility += projLen * (arc0 + arc1);
  }
  visibility = pow(clamp(visibility / float(uSlices), 0.0, 1.0), uPower);
  o = vec4(max(visibility, 0.03), w, 0.0, 1.0);
}
)GLSL";

// Depth-aware 4x4 filter: removes the 4x4 slice pattern without bleeding across depth edges.
constexpr const char* kDenoise = R"GLSL(#version 300 es
precision highp float;
precision highp sampler2D;  // (visibility, view depth in metres): lowp, the GLSL ES default, is 8 bit within +-2
uniform sampler2D tAo;
out vec4 o;
void main() {
  ivec2 c = ivec2(gl_FragCoord.xy), last = textureSize(tAo, 0) - 1;
  vec2 centre = texelFetch(tAo, c, 0).rg;
  if (centre.g > 60000.0) { o = vec4(1.0, centre.g, 0.0, 1.0); return; }
  float tolerance = centre.g * 0.04 + 0.05, sum = 0.0, weights = 0.0;
  for (int y = -2; y < 2; ++y)
    for (int x = -2; x < 2; ++x) {
      vec2 s = texelFetch(tAo, clamp(c + ivec2(x, y), ivec2(0), last), 0).rg;
      float wt = max(0.0, 1.0 - abs(s.g - centre.g) / tolerance);
      sum += s.r * wt;
      weights += wt;
    }
  o = vec4(weights > 1e-4 ? sum / weights : centre.r, centre.g, 0.0, 1.0);
}
)GLSL";

constexpr i32 kSlices[4] = {1, 2, 3, 9};
constexpr i32 kSteps[4] = {2, 2, 3, 3};

constexpr u32 RG = 0x8227, RG16F = 0x822F;

}  // namespace

void AmbientOcclusionPass::configure(const AoOptions& options) {
  options_ = options;
  options_.radius = std::isfinite(options.radius) && options.radius > 0.05f ? options.radius : 1.6f;
  options_.power = std::isfinite(options.power) && options.power > 0.1f ? options.power : 2.2f;
  quality_ = options_.quality;
  half_ = options_.half_resolution;
  slow_since_ = 0;
}

bool AmbientOcclusionPass::init_programs() {
  if (!gl::has_extension("EXT_color_buffer_float") && !gl::has_extension("EXT_color_buffer_half_float")) {
    supported_ = false;
    return false;
  }
  if (!main_.build(kFullscreenVertex, kMain, "gtao") || !denoise_.build(kFullscreenVertex, kDenoise, "gtao denoise")) {
    supported_ = false;
    log(LogLevel::warn, "ambient occlusion disabled: %s", last_error());
    return false;
  }
  m_depth_ = main_.location("tDepth");
  m_full_ = main_.location("uFullSize");
  m_ao_size_ = main_.location("uAoSize");
  m_proj_ = main_.location("uProj");
  m_radius_ = main_.location("uRadius");
  m_power_ = main_.location("uPower");
  m_slices_ = main_.location("uSlices");
  m_steps_ = main_.location("uSteps");
  d_src_ = denoise_.location("tAo");
  vao_ = gl::createVertexArray();
  initialised_ = true;
  return true;
}

void AmbientOcclusionPass::free_targets() {
  for (Target* t : {&a_, &b_}) {
    if (t->texture) gl::deleteTexture(t->texture);
    if (t->framebuffer) gl::deleteFramebuffer(t->framebuffer);
    *t = Target{};
  }
}

bool AmbientOcclusionPass::resize(i32 w, i32 h) {
  if (w == a_.w && h == a_.h && a_.framebuffer) return true;
  free_targets();
  for (Target* t : {&a_, &b_}) {
    t->w = w;
    t->h = h;
    t->texture = gl::createTexture();
    gl::bindTexture(gl::TEXTURE_2D, t->texture);
    gl::texImage2D(gl::TEXTURE_2D, 0, static_cast<i32>(RG16F), w, h, RG, gl::HALF_FLOAT, nullptr, 0);
    gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::NEAREST);
    gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
    gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    t->framebuffer = gl::createFramebuffer();
    gl::bindFramebuffer(gl::FRAMEBUFFER, t->framebuffer);
    gl::framebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, t->texture, 0);
    if (gl::checkFramebufferStatus(gl::FRAMEBUFFER) != gl::FRAMEBUFFER_COMPLETE) {
      gl::bindFramebuffer(gl::FRAMEBUFFER, 0);
      free_targets();
      supported_ = false;
      log(LogLevel::warn, "ambient occlusion disabled: RG16F target unsupported");
      return false;
    }
  }
  gl::bindFramebuffer(gl::FRAMEBUFFER, 0);
  gl::bindTexture(gl::TEXTURE_2D, 0);
  return true;
}

bool AmbientOcclusionPass::compute(u32 depth, i32 width, i32 height, const Mat4& projection, f64 now) {
  if (!options_.enabled || !(options_.radius > 0) || !supported_ || !depth || width < 4 || height < 4) return false;
  if (!initialised_ && !init_programs()) return false;

  // Frame-time guard: on a device that cannot hold ~37 fps step the cost down (quality, then resolution).
  if (last_frame_at_ > 0) {
    const f64 dt = now - last_frame_at_;
    if (dt < 200) frame_avg_ += (dt - frame_avg_) * 0.05;
  }
  last_frame_at_ = now;
  if (options_.adaptive && frame_avg_ > kSlowFrameMs) {
    if (!slow_since_) slow_since_ = now;
    if (now - slow_since_ > kSlowForMs) {
      if (quality_ != AoQuality::low) quality_ = static_cast<AoQuality>(static_cast<u8>(quality_) - 1);
      else half_ = true;
      slow_since_ = 0;
      frame_avg_ = 16;
    }
  } else slow_since_ = 0;

  const i32 w = half_ ? max_of(1, (width + 1) / 2) : width, h = half_ ? max_of(1, (height + 1) / 2) : height;
  if (!resize(w, h)) return false;
  full_w_ = width;
  full_h_ = height;
  const i32 q = static_cast<i32>(quality_);

  gl::disable(gl::DEPTH_TEST);
  gl::disable(gl::BLEND);
  gl::disable(gl::CULL_FACE);
  gl::disable(gl::SCISSOR_TEST);
  gl::depthMask(0);
  gl::bindVertexArray(vao_);
  gl::viewport(0, 0, w, h);

  gl::bindFramebuffer(gl::FRAMEBUFFER, a_.framebuffer);
  gl::activeTexture(gl::TEXTURE0);
  gl::bindTexture(gl::TEXTURE_2D, depth);
  main_.use();
  gl::uniform1i(m_depth_, 0);
  gl::uniform2f(m_full_, static_cast<f32>(width), static_cast<f32>(height));
  gl::uniform2f(m_ao_size_, static_cast<f32>(w), static_cast<f32>(h));
  gl::uniform4f(m_proj_, projection.m[0], projection.m[5], projection.m[10], projection.m[14]);
  gl::uniform1f(m_radius_, options_.radius);
  gl::uniform1f(m_power_, options_.power);
  gl::uniform1i(m_slices_, kSlices[q]);
  gl::uniform1i(m_steps_, kSteps[q]);
  gl::drawArrays(gl::TRIANGLES, 0, 3);

  gl::bindFramebuffer(gl::FRAMEBUFFER, b_.framebuffer);
  gl::bindTexture(gl::TEXTURE_2D, a_.texture);
  denoise_.use();
  gl::uniform1i(d_src_, 0);
  gl::drawArrays(gl::TRIANGLES, 0, 3);

  gl::bindTexture(gl::TEXTURE_2D, 0);
  gl::bindVertexArray(0);
  gl::bindFramebuffer(gl::FRAMEBUFFER, 0);
  gl::depthMask(1);
  return true;
}

void AmbientOcclusionPass::destroy(bool contextLost) {
  if (!contextLost) {
    free_targets();
    if (vao_) gl::deleteVertexArray(vao_);
  } else {
    a_ = b_ = Target{};
  }
  vao_ = 0;
  main_.destroy(contextLost);
  denoise_.destroy(contextLost);
  initialised_ = false;
  supported_ = true;
}

}  // namespace kern
