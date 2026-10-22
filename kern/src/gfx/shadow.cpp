#include "kern/gfx/passes.h"

namespace kern {

namespace {

constexpr const char* kShadowVertex = R"GLSL(#version 300 es
precision highp float;
layout(location=0) in vec3 aPosition;
layout(location=4) in vec4 iModel0;
layout(location=5) in vec4 iModel1;
layout(location=6) in vec4 iModel2;
layout(location=7) in vec4 iModel3;
layout(location=12) in vec4 aJoints;
layout(location=13) in vec4 aWeights;
uniform mat4 uLightViewProjection, uBones[32];
uniform int uSkinned;
void main() {
  vec4 p = vec4(aPosition, 1.0);
  if (uSkinned == 1) {
    mat4 b = aWeights.x * uBones[int(aJoints.x)] + aWeights.y * uBones[int(aJoints.y)]
           + aWeights.z * uBones[int(aJoints.z)] + aWeights.w * uBones[int(aJoints.w)];
    p = b * p;
  }
  gl_Position = uLightViewProjection * mat4(iModel0, iModel1, iModel2, iModel3) * p;
}
)GLSL";

constexpr const char* kShadowFragment = "#version 300 es\nprecision mediump float;\nvoid main() {}\n";

struct LightBasis { Vec3 dir, right, up; };

LightBasis light_basis(Vec3 sun) {
  LightBasis b;
  b.dir = normalize(sun);
  const Vec3 up = std::fabs(b.dir.z) > 0.94f ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
  b.right = normalize(cross(up, b.dir), Vec3{1, 0, 0});
  b.up = cross(b.dir, b.right);
  return b;
}

}  // namespace

bool SunShadowMap::init(const ShadowOptions& o) {
  const i32 r = o.resolution;
  if (r < 128 || r > 4096 || (r & (r - 1))) return fail("shadow resolution must be a power of two 128-4096");
  if (!(o.extent >= 10 && o.extent <= 2000) || !(o.bias >= 0 && o.bias < 0.04f)) return fail("invalid sun shadow options");
  options_ = o;
  if (!program_.build(kShadowVertex, kShadowFragment, "sun shadow")) return false;
  u_matrix_ = program_.location("uLightViewProjection");
  u_bones_ = program_.location("uBones[0]");
  u_skinned_ = program_.location("uSkinned");

  depth_ = gl::createTexture();
  gl::bindTexture(gl::TEXTURE_2D, depth_);
  gl::texImage2D(gl::TEXTURE_2D, 0, gl::DEPTH_COMPONENT24, r, r, gl::DEPTH_COMPONENT, gl::UNSIGNED_INT, nullptr, 0);
  gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
  gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
  gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
  gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
  gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_COMPARE_MODE, gl::COMPARE_REF_TO_TEXTURE);
  gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_COMPARE_FUNC, gl::LEQUAL);
  fbo_ = gl::createFramebuffer();
  gl::bindFramebuffer(gl::FRAMEBUFFER, fbo_);
  gl::framebufferTexture2D(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::TEXTURE_2D, depth_, 0);
  const u32 none = gl::NONE;
  gl::drawBuffers(&none, 1);
  gl::readBuffer(gl::NONE);
  const u32 status = gl::checkFramebufferStatus(gl::FRAMEBUFFER);
  gl::bindFramebuffer(gl::FRAMEBUFFER, 0);
  gl::bindTexture(gl::TEXTURE_2D, 0);
  if (status != gl::FRAMEBUFFER_COMPLETE) {
    destroy(false);
    return fail("sun shadow framebuffer incomplete 0x%x", status);
  }
  return true;
}

void SunShadowMap::destroy(bool contextLost) {
  if (!contextLost) {
    if (fbo_) gl::deleteFramebuffer(fbo_);
    if (depth_) gl::deleteTexture(depth_);
  }
  program_.destroy(contextLost);
  fbo_ = depth_ = 0;
}

u64 SunShadowMap::focus_key(Vec3 focus, Vec3 sun) {
  const LightBasis b = light_basis(sun);
  const f32 texel = 2 * options_.extent / static_cast<f32>(options_.resolution);
  const f64 a = std::round(dot(focus, b.right) / texel);
  const f64 c = std::round(dot(focus, b.up) / texel);
  // Movement along the light direction does not move texels; a coarse step keeps the depth range valid.
  const f64 d = std::round(dot(focus, b.dir) / 8.0f);
  snapped_focus_ = b.right * static_cast<f32>(a * texel) + b.up * static_cast<f32>(c * texel) + b.dir * static_cast<f32>(d * 8);
  const i32 key[6] = {static_cast<i32>(a), static_cast<i32>(c), static_cast<i32>(d), static_cast<i32>(std::lround(b.dir.x * 1e4f)),
                      static_cast<i32>(std::lround(b.dir.y * 1e4f)), static_cast<i32>(std::lround(b.dir.z * 1e4f))};
  return hash_bytes(key, sizeof key);
}

u32 SunShadowMap::draw(Span<Mesh* const> meshes, Vec3 focus, Vec3 sun, u32 layers) {
  focus_key(focus, sun);
  const LightBasis b = light_basis(sun);
  const f32 e = options_.extent;
  const Vec3 eye = snapped_focus_ + b.dir * (e * 2);
  const Vec3 up = std::fabs(b.dir.z) > 0.94f ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
  matrix_ = Mat4::ortho(-e, e, -e, e, 0.2f, e * 4) * Mat4::look_at(eye, snapped_focus_, up);
  const Frustum frustum = Frustum::from_matrix(matrix_);

  gl::bindFramebuffer(gl::FRAMEBUFFER, fbo_);
  gl::viewport(0, 0, options_.resolution, options_.resolution);
  gl::disable(gl::SCISSOR_TEST);
  gl::colorMask(0, 0, 0, 0);
  gl::clearDepth(1);
  gl::depthMask(1);
  gl::clear(gl::DEPTH_BUFFER_BIT);
  gl::enable(gl::DEPTH_TEST);
  gl::depthFunc(gl::LESS);
  gl::disable(gl::BLEND);
  gl::enable(gl::POLYGON_OFFSET_FILL);
  gl::polygonOffset(2, 3);
  program_.use();
  gl::uniformMatrix4fv(u_matrix_, matrix_.m, 1);
  u32 draws = 0;
  i32 skinned_state = -1, cull_state = -1;
  for (Mesh* m : meshes) {
    if (!m->visible || !m->gpu.vao || !m->material.casts_shadow() || !(m->layers & layers)) continue;
    if (!m->skinned() && !frustum.sphere_visible(m->world_bounds)) continue;
    const i32 skinned = m->skinned() ? 1 : 0;
    if (skinned != skinned_state) { gl::uniform1i(u_skinned_, skinned); skinned_state = skinned; }
    if (skinned) gl::uniformMatrix4fv(u_bones_, m->bones.data(), static_cast<i32>(m->bones.size() / 16));
    const i32 cull = m->cull_mode();
    if (cull != cull_state) { apply_cull_mode(cull); cull_state = cull; }
    gl::bindVertexArray(m->gpu.vao);
    if (!m->indices.empty())
      gl::drawElementsInstanced(gl::TRIANGLES, static_cast<i32>(m->indices.size()), m->gpu.element_type, 0, static_cast<i32>(m->instance_count));
    else
      gl::drawArraysInstanced(gl::TRIANGLES, 0, static_cast<i32>(m->vertex_count()), static_cast<i32>(m->instance_count));
    ++draws;
  }
  gl::bindVertexArray(0);
  gl::disable(gl::CULL_FACE);
  gl::disable(gl::POLYGON_OFFSET_FILL);
  gl::colorMask(1, 1, 1, 1);
  gl::bindFramebuffer(gl::FRAMEBUFFER, 0);
  return draws;
}

}  // namespace kern
