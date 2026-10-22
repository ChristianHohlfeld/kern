#include <cmath>
#include <string>

#include "kern/gfx/passes.h"
#include "kern/gfx/shader.h"

namespace kern {

namespace {

constexpr const char* kSkyVertex = R"GLSL(#version 300 es
precision highp float;
uniform vec3 uForward, uRight, uUp;
uniform vec2 uTanHalf;
out vec3 vDir;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  vec2 ndc = p * 2.0 - 1.0;
  vDir = uForward + ndc.x * uTanHalf.x * uRight + ndc.y * uTanHalf.y * uUp;
  gl_Position = vec4(ndc, 1.0, 1.0);   // far plane: only uncovered pixels pass LEQUAL
}
)GLSL";

constexpr const char* kSkyFragmentHead = R"GLSL(#version 300 es
precision highp float;
in vec3 vDir;
uniform float uNight, uSunUp, uTime, uClouds, uExposure, uBlastAge;
uniform vec3 uSun, uMoon, uBlastDir;
uniform vec2 uWind;
layout(location=0) out vec4 outColor;
layout(location=1) out vec4 outHdr;
float h21(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
float vnoise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(h21(i), h21(i + vec2(1, 0)), f.x), mix(h21(i + vec2(0, 1)), h21(i + vec2(1, 1)), f.x), f.y);
}
float fbm(vec2 p) {
  float a = 0.5, s = 0.0;
  for (int i = 0; i < 5; i++) { s += a * vnoise(p); p = p * 2.03 + vec2(17.1, 9.3); a *= 0.5; }
  return s;
}
)GLSL";

constexpr const char* kSkyFragmentMain = R"GLSL(
void main() {
  vec3 d = normalize(vDir);
  float h = max(d.z, 0.0);
  vec3 s = normalize(uSun);
  float mu = max(dot(d, s), 0.0);
  // day -> golden hour -> moonlit night (never pitch black)
  float dusk = clamp(1.0 - abs(s.z) * 4.0, 0.0, 1.0) * (1.0 - uNight * 0.6);
  vec3 zen = mix(vec3(0.05, 0.2, 0.62), vec3(0.012, 0.022, 0.06), uNight);
  vec3 hor = mix(vec3(0.52, 0.66, 0.86), vec3(0.05, 0.075, 0.13), uNight);
  hor = mix(hor, vec3(0.95, 0.48, 0.22), dusk * 0.55);
  zen = mix(zen, vec3(0.16, 0.14, 0.32), dusk * 0.35);
  vec3 col = mix(hor, zen, pow(h, 0.45));
  col += vec3(1.0, 0.62, 0.32) * pow(1.0 - h, 5.0) * (0.18 + 0.5 * dusk)
       * (0.5 + 0.5 * dot(normalize(d.xy + 1e-4), normalize(s.xy + 1e-4))) * uSunUp;
  col += (vec3(1.0, 0.93, 0.8) * (pow(mu, 8.0) * 0.22 + pow(mu, 64.0) * 0.5)
       + vec3(1.0, 0.97, 0.9) * smoothstep(0.9993, 0.99975, mu) * 12.0) * uSunUp;
  if (uNight > 0.01) {
    vec3 m = normalize(uMoon);
    float mm = max(dot(d, m), 0.0);
    col += uNight * (vec3(0.55, 0.65, 0.9) * pow(mm, 24.0) * 0.12 + vec3(0.9, 0.94, 1.0) * smoothstep(0.99955, 0.9998, mm) * 3.2);
    vec2 sp = vec2(atan(d.y, d.x) * 90.0, asin(clamp(d.z, 0.0, 1.0)) * 120.0);
    vec2 cell = floor(sp);
    float r = h21(cell);
    float star = step(0.985, r) * smoothstep(0.5, 0.0, length(fract(sp) - 0.5)) * (0.6 + 0.4 * sin(uTime * 2.0 + r * 40.0));
    col += uNight * vec3(0.9, 0.95, 1.0) * star * smoothstep(0.05, 0.3, d.z) * 0.9;
  }
  if (uClouds > 0.5 && d.z > 0.02) {
    vec2 uv = d.xy / (d.z + 0.12) * 1.6 + uWind * uTime;
    if (uBlastAge > 0.0) {
      vec3 b = normalize(uBlastDir);
      float ang = acos(clamp(dot(normalize(vec3(d.xy, 0.0)), normalize(vec3(b.xy, 0.0))), -1.0, 1.0));
      float push = uBlastAge * 0.35 * exp(-ang * 1.4);
      uv += normalize(d.xy - b.xy + 1e-4) * push;
    }
    float c = fbm(uv * 1.3);
    float cov = smoothstep(0.52, 0.78, c) * smoothstep(0.02, 0.25, d.z);
    float lit = 0.75 + 0.35 * fbm(uv * 1.3 + s.xy * 0.08) - 0.25 * smoothstep(0.6, 0.95, c);
    vec3 cc = mix(vec3(0.62, 0.66, 0.72), vec3(1.0, 0.98, 0.95), lit) + vec3(1.0, 0.85, 0.6) * pow(mu, 6.0) * 0.4 * uSunUp;
    cc = mix(cc, mix(vec3(0.03, 0.04, 0.07), vec3(0.16, 0.18, 0.24), lit), uNight);
    cc = mix(cc, cc * vec3(1.1, 0.75, 0.6), dusk * 0.6);
    col = mix(col, cc, cov * 0.92);
  }
  col = mix(col, hor * 0.92, smoothstep(0.0, -0.2, d.z));
  outHdr = vec4(max(col, vec3(0.0)), 1.0);
  outColor = vec4(linearToSrgb(acesFilmic(col)), 1.0);
}
)GLSL";

}  // namespace

bool SkyPass::init() {
  std::string fragment = kSkyFragmentHead;
  fragment += kGlslToneMap;
  fragment += kSkyFragmentMain;
  if (!program_.build(kSkyVertex, fragment, "sky")) return false;
  u_forward_ = program_.location("uForward");
  u_right_ = program_.location("uRight");
  u_up_ = program_.location("uUp");
  u_tan_half_ = program_.location("uTanHalf");
  u_night_ = program_.location("uNight");
  u_sun_up_ = program_.location("uSunUp");
  u_time_ = program_.location("uTime");
  u_clouds_ = program_.location("uClouds");
  u_exposure_ = program_.location("uExposure");
  u_blast_age_ = program_.location("uBlastAge");
  u_sun_ = program_.location("uSun");
  u_moon_ = program_.location("uMoon");
  u_blast_dir_ = program_.location("uBlastDir");
  u_wind_ = program_.location("uWind");
  vao_ = gl::createVertexArray();
  cache_.reset();
  return true;
}

void SkyPass::destroy(bool contextLost) {
  if (vao_ && !contextLost) gl::deleteVertexArray(vao_);
  vao_ = 0;
  program_.destroy(contextLost);
  cache_.reset();
}

void SkyPass::draw(const Camera& camera, f32 aspect, const SkyState& state, f32 exposure, f64 time_seconds) {
  if (!program_.valid()) return;
  const Vec3 forward = normalize(camera.target - camera.eye, {0, 1, 0});
  Vec3 right = cross(forward, camera.up);
  right = length(right) > 1e-9f ? normalize(right) : normalize(cross(forward, Vec3{0, 1, 0}), Vec3{1, 0, 0});
  const Vec3 up = cross(right, forward);
  const f32 t = std::tan(camera.fov * 0.5f);
  const Vec3 sun = normalize(state.sun, {-0.45f, -0.52f, 0.72f});

  gl::enable(gl::DEPTH_TEST);
  gl::depthFunc(gl::LEQUAL);
  gl::depthMask(0);
  gl::disable(gl::BLEND);
  gl::disable(gl::CULL_FACE);
  program_.use();
  gl::bindVertexArray(vao_);
  cache_.set_vec3(u_forward_, forward);
  cache_.set_vec3(u_right_, right);
  cache_.set_vec3(u_up_, up);
  cache_.set_vec2(u_tan_half_, t * (aspect > 0 ? aspect : 1), t);
  cache_.set_float(u_night_, clamp(state.night, 0.0f, 1.0f));
  cache_.set_float(u_sun_up_, sun.z > -0.05f ? 1.0f : 0.0f);
  cache_.set_float(u_time_, static_cast<f32>(std::fmod(time_seconds, 3600.0)));
  cache_.set_float(u_clouds_, state.clouds ? 1.0f : 0.0f);
  cache_.set_float(u_exposure_, exposure);
  cache_.set_float(u_blast_age_, state.blast_age);
  cache_.set_vec3(u_sun_, sun);
  cache_.set_vec3(u_moon_, normalize(state.moon, {0.4f, 0.5f, 0.76f}));
  cache_.set_vec3(u_blast_dir_, state.blast_direction);
  cache_.set_vec2(u_wind_, state.wind.x, state.wind.y);
  gl::drawArrays(gl::TRIANGLES, 0, 3);
  gl::bindVertexArray(0);
  gl::depthMask(1);
}

}  // namespace kern
