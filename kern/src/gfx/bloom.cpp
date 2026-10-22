#include <string>

#include "kern/gfx/passes.h"
#include "kern/gfx/shader.h"

namespace kern {

namespace {

constexpr f64 kDisableFrameMs = 27, kDisableAfterMs = 3500;
constexpr f32 kStrengthNear = 0.13f, kStrengthWide = 0.08f, kThreshold = 0.88f;

constexpr const char* kFullscreenVertex = R"GLSL(#version 300 es
out vec2 vUv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  vUv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

constexpr const char* kHead = "#version 300 es\nprecision highp float;\nin vec2 vUv;\nout vec4 o;\n";

constexpr const char* kBright = R"GLSL(
uniform sampler2D tSrc;
uniform float uThreshold;
void main() {
  vec3 c = max(texture(tSrc, vUv).rgb, 0.0);
  float m = max(c.r, max(c.g, c.b));
  o = vec4(c * smoothstep(uThreshold, uThreshold + 0.35, m), 1.0);
}
)GLSL";

constexpr const char* kBlur = R"GLSL(
uniform sampler2D tSrc;
uniform vec2 uDir;
void main() {
  vec3 c = texture(tSrc, vUv).rgb * 0.2270;
  c += texture(tSrc, vUv + uDir * 1.3846).rgb * 0.3162;
  c += texture(tSrc, vUv - uDir * 1.3846).rgb * 0.3162;
  c += texture(tSrc, vUv + uDir * 3.2308).rgb * 0.0703;
  c += texture(tSrc, vUv - uDir * 3.2308).rgb * 0.0703;
  o = vec4(c, 1.0);
}
)GLSL";

constexpr const char* kCopy = "uniform sampler2D tSrc;\nvoid main() { o = vec4(texture(tSrc, vUv).rgb, 1.0); }\n";

constexpr const char* kCombine = R"GLSL(
uniform sampler2D tScene, tHdr, tB1, tB2;
uniform float uS1, uS2;
uniform int uGrade;
vec3 present(vec3 hdr) {
  vec3 rgb = acesFilmic(hdr);
  if (uGrade == 1) rgb = gradeColor(rgb);
  return linearToSrgb(clamp(rgb, 0.0, 1.0));
}
void main() {
  vec3 scene = texture(tScene, vUv).rgb;
  vec3 b = texture(tB1, vUv).rgb * uS1 + texture(tB2, vUv).rgb * uS2;
  if (max(b.r, max(b.g, b.b)) < 1e-4) { o = vec4(scene, 1.0); return; }
  vec3 h = max(texture(tHdr, vUv).rgb, 0.0);
  o = vec4(clamp(scene + present(h + b) - present(h), 0.0, 1.0), 1.0);
}
)GLSL";

u32 make_texture(i32 w, i32 h, bool hdr) {
  const u32 t = gl::createTexture();
  gl::bindTexture(gl::TEXTURE_2D, t);
  gl::texImage2D(gl::TEXTURE_2D, 0, hdr ? gl::RGBA16F : gl::RGBA8, w, h, gl::RGBA, hdr ? gl::HALF_FLOAT : gl::UNSIGNED_BYTE, nullptr, 0);
  gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
  gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
  gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
  gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
  return t;
}

void bind_unit(u32 unit, u32 texture) {
  gl::activeTexture(gl::TEXTURE0 + unit);
  gl::bindTexture(gl::TEXTURE_2D, texture);
}

}  // namespace

BloomState BloomPass::state() const {
  if (!options_.enabled) return BloomState::off;
  if (!supported_) return BloomState::unsupported;
  return glow_on_ ? BloomState::active : BloomState::disabled_slow_device;
}

bool BloomPass::init_programs() {
  if (!gl::has_extension("EXT_color_buffer_float") && !gl::has_extension("EXT_color_buffer_half_float")) {
    supported_ = false;
    return false;
  }
  std::string combine = kHead;
  combine += "uniform float uExposure;\n";
  combine += kGlslToneMap;
  combine += kCombine;
  if (!bright_.build(kFullscreenVertex, std::string(kHead) + kBright, "bloom bright") ||
      !blur_.build(kFullscreenVertex, std::string(kHead) + kBlur, "bloom blur") ||
      !copy_.build(kFullscreenVertex, std::string(kHead) + kCopy, "bloom copy") ||
      !combine_.build(kFullscreenVertex, combine, "bloom combine")) {
    supported_ = false;
    log(LogLevel::warn, "bloom disabled: %s", last_error());
    return false;
  }
  bright_src_ = bright_.location("tSrc");
  bright_threshold_ = bright_.location("uThreshold");
  blur_src_ = blur_.location("tSrc");
  blur_dir_ = blur_.location("uDir");
  copy_src_ = copy_.location("tSrc");
  c_scene_ = combine_.location("tScene");
  c_hdr_ = combine_.location("tHdr");
  c_b1_ = combine_.location("tB1");
  c_b2_ = combine_.location("tB2");
  c_s1_ = combine_.location("uS1");
  c_s2_ = combine_.location("uS2");
  c_exposure_ = combine_.location("uExposure");
  c_grade_ = combine_.location("uGrade");
  vao_ = gl::createVertexArray();
  initialised_ = true;
  return true;
}

BloomPass::Target BloomPass::make_target(i32 w, i32 h, bool hdr) {
  Target t;
  t.w = w; t.h = h;
  t.texture = make_texture(w, h, hdr);
  t.framebuffer = gl::createFramebuffer();
  gl::bindFramebuffer(gl::FRAMEBUFFER, t.framebuffer);
  gl::framebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, t.texture, 0);
  return t;
}

void BloomPass::free_targets() {
  for (Target* t : {&main_, &a1_, &b1_, &a2_, &b2_}) {
    if (t->texture) gl::deleteTexture(t->texture);
    if (t->hdr) gl::deleteTexture(t->hdr);
    if (t->depth) gl::deleteRenderbuffer(t->depth);
    if (t->framebuffer) gl::deleteFramebuffer(t->framebuffer);
    *t = Target{};
  }
  w_ = h_ = 0;
}

bool BloomPass::resize(i32 w, i32 h) {
  if (w == w_ && h == h_ && main_.framebuffer) return true;
  free_targets();
  main_ = make_target(w, h, false);
  main_.hdr = make_texture(w, h, true);
  gl::bindFramebuffer(gl::FRAMEBUFFER, main_.framebuffer);
  gl::framebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT1, gl::TEXTURE_2D, main_.hdr, 0);
  main_.depth = gl::createRenderbuffer();
  gl::bindRenderbuffer(gl::RENDERBUFFER, main_.depth);
  gl::renderbufferStorage(gl::RENDERBUFFER, gl::DEPTH_COMPONENT24, w, h);
  gl::framebufferRenderbuffer(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::RENDERBUFFER, main_.depth);
  if (gl::checkFramebufferStatus(gl::FRAMEBUFFER) != gl::FRAMEBUFFER_COMPLETE) {
    gl::bindFramebuffer(gl::FRAMEBUFFER, 0);
    free_targets();
    supported_ = false;
    return false;
  }
  const i32 qw = max_of(1, w >> 2), qh = max_of(1, h >> 2), ew = max_of(1, w >> 3), eh = max_of(1, h >> 3);
  a1_ = make_target(qw, qh, true);
  b1_ = make_target(qw, qh, true);
  a2_ = make_target(ew, eh, true);
  b2_ = make_target(ew, eh, true);
  gl::bindFramebuffer(gl::FRAMEBUFFER, 0);
  w_ = w; h_ = h;
  return true;
}

bool BloomPass::begin(i32 width, i32 height, f64 now, bool page_hidden) {
  if (!options_.enabled || !supported_) return false;
  if (!initialised_ && !init_programs()) return false;
  if (!resize(width, height)) return false;
  // Frame-time guard (exponential moving average of the frame interval).
  if (last_frame_at_ > 0) {
    const f64 dt = now - last_frame_at_;
    if (dt < 200) frame_avg_ += (dt - frame_avg_) * 0.05;
  }
  last_frame_at_ = now;
  if (page_hidden) slow_since_ = 0;
  else if (frame_avg_ > kDisableFrameMs && !options_.forced) {
    if (!slow_since_) slow_since_ = now;
    if (now - slow_since_ > kDisableAfterMs && glow_on_) glow_on_ = false;
  } else slow_since_ = 0;
  gl::bindFramebuffer(gl::FRAMEBUFFER, main_.framebuffer);
  const u32 buffers[2] = {gl::COLOR_ATTACHMENT0, gl::COLOR_ATTACHMENT1};
  gl::drawBuffers(buffers, 2);
  return true;
}

void BloomPass::draw(const Program& p, const Target* target) {
  gl::bindFramebuffer(gl::FRAMEBUFFER, target ? target->framebuffer : 0);
  gl::viewport(0, 0, target ? target->w : w_, target ? target->h : h_);
  p.use();
  gl::drawArrays(gl::TRIANGLES, 0, 3);
}

void BloomPass::blur(Target& src, Target& tmp) {
  bind_unit(0, src.texture);
  blur_.use();
  gl::uniform1i(blur_src_, 0);
  gl::uniform2f(blur_dir_, 1.0f / static_cast<f32>(src.w), 0);
  draw(blur_, &tmp);
  bind_unit(0, tmp.texture);
  gl::uniform2f(blur_dir_, 0, 1.0f / static_cast<f32>(src.h));
  draw(blur_, &src);
}

void BloomPass::end(f32 exposure) {
  gl::disable(gl::DEPTH_TEST);
  gl::disable(gl::BLEND);
  gl::disable(gl::CULL_FACE);
  gl::disable(gl::SCISSOR_TEST);
  gl::depthMask(0);
  gl::bindVertexArray(vao_);
  if (glow_on_) {
    bind_unit(0, main_.hdr);
    bright_.use();
    gl::uniform1i(bright_src_, 0);
    gl::uniform1f(bright_threshold_, kThreshold);
    draw(bright_, &a1_);
    blur(a1_, b1_);
    bind_unit(0, a1_.texture);
    copy_.use();
    gl::uniform1i(copy_src_, 0);
    draw(copy_, &a2_);
    blur(a2_, b2_);
  }
  bind_unit(0, main_.texture);
  bind_unit(1, main_.hdr);
  bind_unit(2, a1_.texture);
  bind_unit(3, a2_.texture);
  combine_.use();
  gl::uniform1i(c_scene_, 0);
  gl::uniform1i(c_hdr_, 1);
  gl::uniform1i(c_b1_, 2);
  gl::uniform1i(c_b2_, 3);
  gl::uniform1f(c_s1_, glow_on_ ? kStrengthNear : 0);
  gl::uniform1f(c_s2_, glow_on_ ? kStrengthWide : 0);
  gl::uniform1f(c_exposure_, exposure);
  gl::uniform1i(c_grade_, options_.grade ? 1 : 0);
  draw(combine_, nullptr);
  // Unbind: a sampler left on a render target would be a feedback loop next frame.
  for (i32 i = 3; i >= 0; --i) bind_unit(static_cast<u32>(i), 0);
  gl::bindVertexArray(0);
  gl::depthMask(1);
}

void BloomPass::destroy(bool contextLost) {
  if (!contextLost) {
    free_targets();
    if (vao_) gl::deleteVertexArray(vao_);
  } else {
    main_ = a1_ = b1_ = a2_ = b2_ = Target{};
    w_ = h_ = 0;
  }
  vao_ = 0;
  bright_.destroy(contextLost);
  blur_.destroy(contextLost);
  copy_.destroy(contextLost);
  combine_.destroy(contextLost);
  initialised_ = false;
}

}  // namespace kern
