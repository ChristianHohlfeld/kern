#include "kern/gfx/renderer.h"

#include <algorithm>
#include <cmath>

#include "kern/scene/bvh.h"

namespace kern {

namespace {

constexpr u32 kBvhMinTriangles = 36;

bool all_finite(const f32* v, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i)
    if (!std::isfinite(v[i])) return false;
  return true;
}

template <class T>
void assign(std::vector<T>& dst, Span<const T> src) { dst.assign(src.begin(), src.end()); }

}  // namespace

Renderer::Renderer() = default;
Renderer::~Renderer() { shutdown(); }

bool Renderer::init(const RendererOptions& options, Span<ShaderExtension* const> extensions) {
  shutdown();
  options_ = options;
  extensions_.assign(extensions.begin(), extensions.end());
  material_keys_.clear();
  extension_attributes_.clear();
  u32 location = attr::extension0;
  for (ShaderExtension* e : extensions_) {
    e->material_base = static_cast<u32>(material_keys_.size());
    for (const char* key : e->material_keys()) material_keys_.push_back(key);
    e->attribute_base = location;
    for (const ExtensionAttribute& a : e->attributes()) {
      if (a.size < 1 || a.size > 4) return fail("extension %s: attribute %s size must be 1..4", e->name(), a.key);
      extension_attributes_.push_back(&a);
      ++location;
    }
  }
  if (material_keys_.size() > kMaxMaterialParams) return fail("extensions declare more than %u material parameters", kMaxMaterialParams);
  if (extension_attributes_.size() > kMaxExtensionAttributes) return fail("extensions declare more than %u vertex streams", kMaxExtensionAttributes);
  shadow_requested_ = options.shadows;
  shadow_options_ = options.shadow;
  bloom_.configure(options.bloom);
  slots_.clear();
  slots_.emplace_back();  // id 0 is "none"
  free_ids_.clear();
  allocated_.assign(1, 0);
  textures_.assign(1, Texture{});
  if (!init_gpu()) return false;
  initialised_ = true;
  return resize(options.width, options.height);
}

bool Renderer::init_gpu() {
  const ShaderSource src = build_standard_shader(extensions());
  if (!program_.build(src.vertex, src.fragment, "standard")) return false;
  cache_.reset();
  const Program& p = program_;
  u_.view_projection = p.location("uViewProjection");
  u_.material = p.location("uMaterial");
  u_.emissive = p.location("uEmissive");
  u_.alpha_cutoff = p.location("uAlphaCutoff");
  u_.unlit = p.location("uUnlit");
  u_.linear_color = p.location("uLinearColor");
  u_.pbr = p.location("uPbr");
  u_.has_texture = p.location("uHasTexture");
  u_.texture = p.location("uTexture");
  u_.skin_enabled = p.location("uSkinEnabled");
  u_.bones = p.location("uBones[0]");
  u_.sun_direction = p.location("uSunDirection");
  u_.sun_color = p.location("uSunColor");
  u_.sky_color = p.location("uSkyColor");
  u_.ground_color = p.location("uGroundColor");
  u_.fog_color = p.location("uFogColor");
  u_.eye = p.location("uEye");
  u_.env_zenith = p.location("uEnvZenith");
  u_.env_horizon = p.location("uEnvHorizon");
  u_.env_ground = p.location("uEnvGround");
  u_.fog_density = p.location("uFogDensity");
  u_.exposure = p.location("uExposure");
  u_.env_intensity = p.location("uEnvIntensity");
  u_.grade = p.location("uGrade");
  u_.fog_height_falloff = p.location("uFogHeightFalloff");
  u_.fog_sun_scatter = p.location("uFogSunScatter");
  u_.point_count = p.location("uPointLightCount");
  u_.point_pos_range = p.location("uPointPosRange[0]");
  u_.point_color_intensity = p.location("uPointColorIntensity[0]");
  u_.shadow_enabled = p.location("uSunShadowEnabled");
  u_.shadow_map = p.location("uSunShadowMap");
  u_.shadow_projection = p.location("uSunShadowProjection");
  u_.shadow_bias = p.location("uSunShadowBias");
  for (ShaderExtension* e : extensions_) e->bind(program_);
  if (!sky_pass_.init()) return false;
  timer_supported_ = gl::has_extension("EXT_disjoint_timer_query_webgl2");
  pending_queries_.clear();
  free_queries_.clear();
  last_gpu_ms_ = -1;
  shadow_.reset();
  shadow_key_valid_ = false;
  if (shadow_requested_) {
    auto map = std::make_unique<SunShadowMap>();
    if (!map->init(shadow_options_)) return false;
    shadow_ = std::move(map);
  }
  return true;
}

void Renderer::shutdown() {
  if (!initialised_) return;
  for (Mesh* m : live_) free_gpu(*m);
  live_.clear();
  slots_.clear();
  free_ids_.clear();
  allocated_.clear();
  if (!lost_) {
    for (u32 q : pending_queries_) gl::deleteQuery(q);
    for (u32 q : free_queries_) gl::deleteQuery(q);
    for (Texture& t : textures_)
      if (t.alive && !t.external && t.handle) gl::deleteTexture(t.handle);
  }
  textures_.clear();
  pending_queries_.clear();
  free_queries_.clear();
  if (shadow_) shadow_->destroy(lost_);
  shadow_.reset();
  sky_pass_.destroy(lost_);
  bloom_.destroy(lost_);
  program_.destroy(lost_);
  initialised_ = false;
}

bool Renderer::resize(i32 width, i32 height) {
  if (width < 1 || height < 1) return fail("invalid drawing dimensions");
  width_ = width;
  height_ = height;
  return true;
}

// ---------------------------------------------------------------------------
// meshes

u32 Renderer::allocate_id() {
  if (!free_ids_.empty()) {
    const u32 id = free_ids_.back();
    free_ids_.pop_back();
    allocated_[id] = 1;
    return id;
  }
  slots_.emplace_back();
  allocated_.push_back(1);
  return static_cast<u32>(slots_.size() - 1);
}

void Renderer::release_id(u32 id) {
  if (!id_allocated(id) || slots_[id]) return;
  allocated_[id] = 0;
  free_ids_.push_back(id);
}

bool Renderer::build_instances(Span<const f32> transforms, Span<const f32> tints, std::vector<f32>& out_t,
                               std::vector<f32>& out_tint, std::vector<f32>& out_normals, u32& count) const {
  static constexpr f32 kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  if (transforms.empty()) transforms = Span<const f32>(kIdentity, 16);
  if (transforms.size % 16) return fail("instance transforms must be 16 floats each");
  if (!all_finite(transforms.data, transforms.size)) return fail("non-finite instance transform");
  count = static_cast<u32>(transforms.size / 16);
  assign(out_t, transforms);
  if (tints.size == static_cast<std::size_t>(count) * 4) {
    if (!all_finite(tints.data, tints.size)) return fail("non-finite instance tint");
    assign(out_tint, tints);
  }
  else out_tint.assign(static_cast<std::size_t>(count) * 4, 1.0f);
  out_normals.resize(static_cast<std::size_t>(count) * 9);
  for (u32 k = 0; k < count; ++k)
    if (!normal_matrix3(out_t.data() + k * 16, out_normals.data() + k * 9)) return fail("singular world transform (instance %u)", k);
  return true;
}

bool Renderer::upsert(u32 id, const MeshInput& in) {
  if (!initialised_) return fail("renderer not initialised");
  if (!id_allocated(id)) return fail("mesh id %u was not allocated", id);
  const std::size_t p = in.positions.size;
  if (!p || p % 3) return fail("positions must be xyz triplets");
  const u32 vertices = static_cast<u32>(p / 3);
  const u32 per_primitive = in.material.lines ? 2 : 3;
  if (in.indices.empty() && vertices % per_primitive) return fail(in.material.lines ? "invalid lines" : "invalid triangles");
  if (in.material.lines && !in.bones.empty()) return fail("line meshes cannot be skinned");
  if (!in.indices.empty()) {
    if (in.indices.size % per_primitive) return fail("invalid indices");
    for (u32 i : in.indices)
      if (i >= vertices) return fail("index %u outside %u vertices", i, vertices);
  }
  if (!all_finite(in.positions.data, p)) return fail("non-finite geometry");
  if (!in.normals.empty() && in.normals.size != p) return fail("geometry attribute count mismatch (normals)");
  if (!in.colors.empty() && in.colors.size != p) return fail("geometry attribute count mismatch (colors)");
  if (!in.uvs.empty() && in.uvs.size != static_cast<std::size_t>(vertices) * 2) return fail("geometry attribute count mismatch (uvs)");
  const bool skinned = !in.bones.empty();
  if (skinned) {
    if (in.joints.size != static_cast<std::size_t>(vertices) * 4 || in.weights.size != static_cast<std::size_t>(vertices) * 4 ||
        in.bones.size % 16 || in.bones.size > kMaxBones * 16)
      return fail("invalid skin vertex/joint palette sizes");
    const u32 bone_count = static_cast<u32>(in.bones.size / 16);
    for (std::size_t k = 0; k < in.joints.size; ++k) {
      const f32 j = in.joints[k], w = in.weights[k];
      if (j != std::floor(j) || j < 0 || j >= static_cast<f32>(bone_count) || !(w >= 0 && w <= 1)) return fail("invalid skin joint/weight");
    }
    for (std::size_t k = 0; k < in.weights.size; k += 4) {
      const f32 sum = in.weights[k] + in.weights[k + 1] + in.weights[k + 2] + in.weights[k + 3];
      if (std::fabs(sum - 1) > 0.001f) return fail("skin weights must sum to 1");
    }
  } else if (!in.joints.empty() || !in.weights.empty()) {
    return fail("skin streams without bone palette");
  }
  for (std::size_t a = 0; a < extension_attributes_.size(); ++a) {
    const Span<const f32> s = in.extension[a];
    if (!s.empty() && s.size != static_cast<std::size_t>(vertices) * extension_attributes_[a]->size)
      return fail("%s attribute count mismatch", extension_attributes_[a]->key);
  }

  // Stage the complete replacement before the live mesh changes.
  auto mesh = std::make_unique<Mesh>();
  Mesh& m = *mesh;
  m.id = id;
  m.visible = in.visible;
  m.layers = in.layers;
  m.material = in.material;
  if (m.material.lines) { m.material.unlit = true; m.material.cast_shadow = false; }
  m.texture = in.texture;
  m.linear_colors = in.material.linear_colors;
  assign(m.positions, in.positions);
  assign(m.indices, in.indices);
  if (!in.normals.empty()) {
    assign(m.normals, in.normals);
  } else if (m.material.lines) {
    m.normals.assign(p, 0.0f);
    for (std::size_t k = 2; k < p; k += 3) m.normals[k] = 1;
  } else if (m.indices.empty()) {
    // Flat face normals for triangle soups.
    m.normals.resize(p);
    for (std::size_t k = 0; k < p; k += 9) {
      const Vec3 a = Vec3::load(&m.positions[k]), b = Vec3::load(&m.positions[k + 3]), c = Vec3::load(&m.positions[k + 6]);
      const Vec3 n = normalize(cross(b - a, c - a), {0, 0, 1});
      for (int j = 0; j < 3; ++j) n.store(&m.normals[k + j * 3]);
    }
  } else {
    m.normals.assign(p, 0.0f);
    for (std::size_t k = 2; k < p; k += 3) m.normals[k] = 1;
  }
  if (!in.colors.empty()) assign(m.colors, in.colors); else m.colors.assign(p, 1.0f);
  if (!in.uvs.empty()) assign(m.uvs, in.uvs); else m.uvs.assign(static_cast<std::size_t>(vertices) * 2, 0.0f);
  for (std::size_t a = 0; a < extension_attributes_.size(); ++a) {
    m.extension_size[a] = static_cast<u8>(extension_attributes_[a]->size);
    assign(m.extension[a], in.extension[a]);
  }
  if (skinned) { assign(m.joints, in.joints); assign(m.weights, in.weights); assign(m.bones, in.bones); }
  if (!build_instances(in.transforms, in.tints, m.transforms, m.tints, m.instance_normals, m.instance_count)) return false;
  m.mirror = mirror_state(m.transforms.data(), m.instance_count);
  m.local_bounds = bounding_sphere(m.positions.data(), vertices);
  m.world_bounds = instanced_bounds(m.local_bounds, m.transforms.data(), m.instance_count);
  if (!m.world_bounds.valid()) return fail("invalid world bounds");
  // Static world-space meshes get a BVH for picking; moving ones use the exact brute-force path.
  if (!m.material.lines && m.instance_count == 1 && !skinned && m.triangle_count() >= kBvhMinTriangles &&
      Mat4::load(m.transforms.data()).is_identity()) {
    m.bvh = std::make_unique<Bvh>();
    m.bvh->build(m.positions.data(), vertices, m.indices.empty() ? nullptr : m.indices.data(), static_cast<u32>(m.indices.size()));
  }
  if (!lost_) upload(m);

  std::unique_ptr<Mesh>& slot = slots_[id];
  bool old_casts = false;
  if (slot) {
    old_casts = slot->visible && slot->material.casts_shadow();
    m.live_index = slot->live_index;
    free_gpu(*slot);
    live_[m.live_index] = &m;
  } else {
    m.live_index = static_cast<u32>(live_.size());
    live_.push_back(&m);
  }
  slot = std::move(mesh);
  ++scene_revision_;
  if (m.material.casts_shadow() || old_casts) ++shadow_revision_;
  return true;
}

void Renderer::upload(Mesh& m) {
  MeshGpu& g = m.gpu;
  g = MeshGpu{};
  g.vao = gl::createVertexArray();
  gl::bindVertexArray(g.vao);
  auto stream = [](u32 location, const std::vector<f32>& values, i32 size) {
    const u32 b = gl::createBuffer();
    gl::bindBuffer(gl::ARRAY_BUFFER, b);
    gl::bufferData(gl::ARRAY_BUFFER, values.data(), static_cast<u32>(values.size() * 4), gl::STATIC_DRAW);
    gl::enableVertexAttribArray(location);
    gl::vertexAttribPointer(location, size, gl::FLOAT, 0, 0, 0);
    return b;
  };
  g.position = stream(attr::position, m.positions, 3);
  g.normal = stream(attr::normal, m.normals, 3);
  g.color = stream(attr::color, m.colors, 3);
  g.uv = stream(attr::uv, m.uvs, 2);
  // Instance streams (re-specified by update_instances with geometric growth).
  auto instanced = [](u32 first_location, u32 columns, i32 size) {
    const u32 b = gl::createBuffer();
    gl::bindBuffer(gl::ARRAY_BUFFER, b);
    for (u32 i = 0; i < columns; ++i) {
      gl::enableVertexAttribArray(first_location + i);
      gl::vertexAttribPointer(first_location + i, size, gl::FLOAT, 0, static_cast<i32>(columns) * size * 4, static_cast<i32>(i) * size * 4);
      gl::vertexAttribDivisor(first_location + i, 1);
    }
    return b;
  };
  g.transforms = instanced(attr::model0, 4, 4);
  gl::bufferData(gl::ARRAY_BUFFER, m.transforms.data(), static_cast<u32>(m.transforms.size() * 4), gl::DYNAMIC_DRAW);
  g.tints = instanced(attr::tint, 1, 4);
  gl::bufferData(gl::ARRAY_BUFFER, m.tints.data(), static_cast<u32>(m.tints.size() * 4), gl::DYNAMIC_DRAW);
  g.instance_normals = instanced(attr::normal0, 3, 3);
  gl::bufferData(gl::ARRAY_BUFFER, m.instance_normals.data(), static_cast<u32>(m.instance_normals.size() * 4), gl::DYNAMIC_DRAW);
  g.capacity[0] = static_cast<u32>(m.transforms.size());
  g.capacity[1] = static_cast<u32>(m.tints.size());
  g.capacity[2] = static_cast<u32>(m.instance_normals.size());
  if (m.skinned()) {
    g.joints = stream(attr::joints, m.joints, 4);
    g.weights = stream(attr::weights, m.weights, 4);
  }
  for (u32 a = 0; a < kMaxExtensionAttributes; ++a)
    if (!m.extension[a].empty()) g.extension[a] = stream(attr::extension0 + a, m.extension[a], m.extension_size[a]);
  if (!m.indices.empty()) {
    g.element = gl::createBuffer();
    gl::bindBuffer(gl::ELEMENT_ARRAY_BUFFER, g.element);
    if (m.vertex_count() <= 65536) {
      // 16-bit indices halve index bandwidth for the common case.
      index16_scratch_.resize(m.indices.size());
      for (std::size_t i = 0; i < m.indices.size(); ++i) index16_scratch_[i] = static_cast<u16>(m.indices[i]);
      gl::bufferData(gl::ELEMENT_ARRAY_BUFFER, index16_scratch_.data(), static_cast<u32>(index16_scratch_.size() * 2), gl::STATIC_DRAW);
      g.element_type = gl::UNSIGNED_SHORT;
    } else {
      gl::bufferData(gl::ELEMENT_ARRAY_BUFFER, m.indices.data(), static_cast<u32>(m.indices.size() * 4), gl::STATIC_DRAW);
      g.element_type = gl::UNSIGNED_INT;
    }
  }
  gl::bindVertexArray(0);
  gl::bindBuffer(gl::ARRAY_BUFFER, 0);
}

void Renderer::free_gpu(Mesh& m) {
  MeshGpu& g = m.gpu;
  if (!lost_ && g.vao) {
    gl::deleteVertexArray(g.vao);
    for (u32 b : {g.position, g.normal, g.color, g.uv, g.transforms, g.tints, g.instance_normals, g.joints, g.weights,
                  g.extension[0], g.extension[1], g.element})
      if (b) gl::deleteBuffer(b);
  }
  g = MeshGpu{};
}

void Renderer::write_instance_streams(Mesh& m, u32 mask) {
  MeshGpu& g = m.gpu;
  const std::vector<f32>* data[3] = {&m.transforms, &m.tints, &m.instance_normals};
  const u32 buffers[3] = {g.transforms, g.tints, g.instance_normals};
  for (int s = 0; s < 3; ++s) {
    if (!(mask & (1u << s))) continue;
    gl::bindBuffer(gl::ARRAY_BUFFER, buffers[s]);
    const u32 n = static_cast<u32>(data[s]->size());
    if (n > g.capacity[s]) {
      // Geometric growth: no reallocation while instance counts oscillate.
      g.capacity[s] = max_of(n, g.capacity[s] * 2);
      gl::bufferData(gl::ARRAY_BUFFER, nullptr, g.capacity[s] * 4, gl::DYNAMIC_DRAW);
    }
    gl::bufferSubData(gl::ARRAY_BUFFER, 0, data[s]->data(), n * 4);
  }
  gl::bindBuffer(gl::ARRAY_BUFFER, 0);
}

bool Renderer::update_instances(u32 id, Span<const f32> transforms, Span<const f32> tints, bool* changed) {
  if (changed) *changed = false;
  Mesh* m = mesh(id);
  if (!m) return fail("unknown mesh %u", id);
  // Empty transforms keep the current instances (tint-only update).
  const bool keep_transforms = transforms.empty();
  if (!keep_transforms && transforms.size % 16) return fail("instance transforms must be 16 floats each");
  const std::size_t count = keep_transforms ? m->instance_count : transforms.size / 16;
  const bool same_transforms = keep_transforms || (transforms.size == m->transforms.size() &&
                                                   std::memcmp(transforms.data, m->transforms.data(), transforms.size * 4) == 0);
  const bool default_tints = tints.size != count * 4;
  bool same_tints;
  if (default_tints) {
    same_tints = m->tints.size() == count * 4;
    for (std::size_t i = 0; same_tints && i < m->tints.size(); ++i) same_tints = m->tints[i] == 1.0f;
  } else {
    same_tints = m->tints.size() == tints.size && std::memcmp(tints.data, m->tints.data(), tints.size * 4) == 0;
  }
  if (same_transforms && same_tints) return true;

  if (same_transforms) {
    // Colour only: geometry, bounds, BVH and the shadow map stay valid.
    if (default_tints) {
      m->tints.assign(count * 4, 1.0f);
    } else {
      if (!all_finite(tints.data, tints.size)) return fail("non-finite instance tint");
      assign(m->tints, tints);
    }
    if (!lost_ && m->gpu.vao) write_instance_streams(*m, 2);
    ++scene_revision_;
    if (changed) *changed = true;
    return true;
  }

  // Scratch vectors swap with the mesh's, so steady-state updates never allocate.
  u32 n = 0;
  if (!build_instances(transforms, tints, scratch_transforms_, scratch_tints_, scratch_normals_, n)) return false;
  const Sphere bounds = instanced_bounds(m->local_bounds, scratch_transforms_.data(), n);
  if (!bounds.valid()) return fail("invalid world bounds");
  m->transforms.swap(scratch_transforms_);
  m->tints.swap(scratch_tints_);
  m->instance_normals.swap(scratch_normals_);
  m->instance_count = n;
  m->mirror = mirror_state(m->transforms.data(), n);
  m->world_bounds = bounds;
  // The BVH holds mesh-space triangles and is used for a single identity instance.
  const bool static_world = n == 1 && !m->skinned() && !m->material.lines && Mat4::load(m->transforms.data()).is_identity();
  if (!static_world) {
    m->bvh.reset();
  } else if (!m->bvh && m->triangle_count() >= kBvhMinTriangles) {
    m->bvh = std::make_unique<Bvh>();
    m->bvh->build(m->positions.data(), m->vertex_count(), m->indices.empty() ? nullptr : m->indices.data(), static_cast<u32>(m->indices.size()));
  }
  if (!lost_ && m->gpu.vao) write_instance_streams(*m);
  touch(*m);
  if (changed) *changed = true;
  return true;
}

bool Renderer::update_bones(u32 id, Span<const f32> bones) {
  Mesh* m = mesh(id);
  if (!m || !m->skinned()) return fail("not a skinned mesh %u", id);
  if (bones.size != m->bones.size()) return fail("joint count cannot change without rebuilding the mesh");
  if (!all_finite(bones.data, bones.size)) return fail("non-finite bone matrix");
  std::memcpy(m->bones.data(), bones.data, bones.size * 4);
  touch(*m);
  return true;
}

bool Renderer::set_layers(u32 id, u32 layers) {
  Mesh* m = mesh(id);
  if (!m) return fail("unknown mesh %u", id);
  if (m->layers == layers) return true;
  m->layers = layers;
  touch(*m);
  return true;
}

bool Renderer::set_visible(u32 id, bool visible) {
  Mesh* m = mesh(id);
  if (!m || m->visible == visible) return false;
  m->visible = visible;
  touch(*m);
  return true;
}

bool Renderer::set_material(u32 id, const Material& material) {
  Mesh* m = mesh(id);
  if (!m) return fail("unknown mesh %u", id);
  if (material.lines != m->material.lines) return fail("the primitive type of mesh %u cannot change without a new upsert", id);
  const bool old_casts = m->material.casts_shadow();
  m->material = material;
  if (m->material.lines) { m->material.unlit = true; m->material.cast_shadow = false; }
  m->linear_colors = material.linear_colors;
  ++scene_revision_;
  if (old_casts || material.casts_shadow()) ++shadow_revision_;
  return true;
}

bool Renderer::remove(u32 id) {
  Mesh* m = mesh(id);
  if (!m) return false;
  touch(*m);
  free_gpu(*m);
  // Swap-remove from the dense live list.
  Mesh* last = live_.back();
  live_[m->live_index] = last;
  last->live_index = m->live_index;
  live_.pop_back();
  slots_[id].reset();
  allocated_[id] = 0;
  free_ids_.push_back(id);
  return true;
}

// ---------------------------------------------------------------------------
// textures

void Renderer::upload_texture(Texture& t) {
  t.handle = gl::createTexture();
  gl::bindTexture(gl::TEXTURE_2D, t.handle);
  gl::texImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, t.desc.width, t.desc.height, gl::RGBA, gl::UNSIGNED_BYTE, t.pixels.data(),
                 static_cast<u32>(t.pixels.size()));
  const bool mipmaps = t.desc.mipmaps && !t.desc.nearest;
  if (mipmaps) gl::generateMipmap(gl::TEXTURE_2D);
  gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, t.desc.nearest ? gl::NEAREST : mipmaps ? gl::LINEAR_MIPMAP_LINEAR : gl::LINEAR);
  gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, t.desc.nearest ? gl::NEAREST : gl::LINEAR);
  const i32 wrap = t.desc.repeat ? gl::REPEAT : gl::CLAMP_TO_EDGE;
  gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, wrap);
  gl::texParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, wrap);
  gl::bindTexture(gl::TEXTURE_2D, 0);
  bound_texture_ = ~0u;
}

u32 Renderer::create_texture(const TextureDesc& desc) {
  if (!desc.rgba || desc.width < 1 || desc.height < 1) { fail("invalid texture"); return 0; }
  Texture t;
  t.desc = desc;
  t.pixels.assign(desc.rgba, desc.rgba + static_cast<std::size_t>(desc.width) * desc.height * 4);
  t.desc.rgba = nullptr;
  t.alive = true;
  if (!lost_) upload_texture(t);
  textures_.push_back(std::move(t));
  return static_cast<u32>(textures_.size() - 1);
}

u32 Renderer::register_external_texture(u32 gl_handle) {
  Texture t;
  t.handle = gl_handle;
  t.external = true;
  t.alive = true;
  textures_.push_back(std::move(t));
  return static_cast<u32>(textures_.size() - 1);
}

bool Renderer::delete_texture(u32 id) {
  if (!id || id >= textures_.size() || !textures_[id].alive) return false;
  Texture& t = textures_[id];
  if (!t.external && t.handle && !lost_) gl::deleteTexture(t.handle);
  t = Texture{};
  bound_texture_ = ~0u;
  return true;
}

// ---------------------------------------------------------------------------
// state

bool Renderer::set_point_lights(const PointLights& lights) {
  if (lights.count > kMaxPointLights) return fail("at most %u point lights", kMaxPointLights);
  if (!all_finite(lights.pos_range, kMaxPointLights * 4) || !all_finite(lights.color_intensity, kMaxPointLights * 4))
    return fail("invalid point light data");
  point_lights_ = lights;
  return true;
}

bool Renderer::set_shadows(const ShadowOptions* options) {
  if (shadow_) shadow_->destroy(lost_);
  shadow_.reset();
  shadow_key_valid_ = false;
  shadow_requested_ = options != nullptr;
  if (!options) return true;
  shadow_options_ = *options;
  if (lost_) return true;
  auto map = std::make_unique<SunShadowMap>();
  if (!map->init(*options)) { shadow_requested_ = false; return false; }
  shadow_ = std::move(map);
  return true;
}

// ---------------------------------------------------------------------------
// frame

void Renderer::set_frame_uniforms(const Mat4& vp) {
  UniformCache& c = cache_;
  const Lighting& l = lighting;
  c.set_mat4(u_.view_projection, vp.m);
  c.set_vec3(u_.sun_direction, l.sun_direction);
  c.set_vec3(u_.sun_color, l.sun_color);
  c.set_vec3(u_.sky_color, l.sky_color);
  c.set_vec3(u_.ground_color, l.ground_color);
  c.set_vec3(u_.fog_color, l.fog_color);
  c.set_vec3(u_.eye, camera.eye);
  c.set_vec3(u_.env_zenith, l.env_zenith);
  c.set_vec3(u_.env_horizon, l.env_horizon);
  c.set_vec3(u_.env_ground, l.env_ground);
  c.set_float(u_.fog_density, l.fog_density);
  c.set_float(u_.fog_height_falloff, l.fog_height_falloff > 0 ? l.fog_height_falloff : 0.0f);
  c.set_float(u_.fog_sun_scatter, clamp(l.fog_sun_scatter, 0.0f, 1.0f));
  c.set_float(u_.exposure, l.exposure);
  c.set_float(u_.env_intensity, std::isfinite(l.env_intensity) ? l.env_intensity : 0.9f);
  c.set_int(u_.grade, options_.grade ? 1 : 0);
  c.set_int(u_.texture, 0);
  c.set_int(u_.point_count, static_cast<i32>(point_lights_.count));
  if (point_lights_.count) {
    c.set_vec4_array(u_.point_pos_range, point_lights_.pos_range, kMaxPointLights);
    c.set_vec4_array(u_.point_color_intensity, point_lights_.color_intensity, kMaxPointLights);
  }
  c.set_int(u_.shadow_enabled, shadow_ ? 1 : 0);
  c.set_int(u_.shadow_map, 1);
  if (shadow_) {
    c.set_mat4(u_.shadow_projection, shadow_->matrix().m);
    c.set_float(u_.shadow_bias, shadow_->options().bias);
    gl::activeTexture(gl::TEXTURE0 + 1);
    gl::bindTexture(gl::TEXTURE_2D, shadow_->depth_texture());
    gl::activeTexture(gl::TEXTURE0);
  }
  for (ShaderExtension* e : extensions_) e->frame_uniforms(cache_, *this);
}

void Renderer::draw_mesh(const Mesh& m, RenderStats& stats) {
  UniformCache& c = cache_;
  const Material& mat = m.material;
  c.set_vec4(u_.material, mat.color);
  c.set_vec3(u_.emissive, mat.emissive);
  c.set_float(u_.alpha_cutoff, mat.alpha == AlphaMode::mask ? mat.alpha_cutoff : 0.0f);
  c.set_int(u_.unlit, mat.unlit ? 1 : 0);
  c.set_int(u_.linear_color, m.linear_colors ? 1 : 0);
  c.set_vec2(u_.pbr, std::isfinite(mat.roughness) ? mat.roughness : 1.0f, std::isfinite(mat.metalness) ? mat.metalness : 0.0f);
  const u32 texture = texture_handle(m.texture);
  c.set_int(u_.has_texture, texture ? 1 : 0);
  c.set_int(u_.skin_enabled, m.skinned() ? 1 : 0);
  if (m.skinned()) UniformCache::set_mat4_array(u_.bones, m.bones.data(), static_cast<i32>(m.bones.size() / 16));
  for (ShaderExtension* e : extensions_) e->material_uniforms(c, mat, mat.params + e->material_base);
  if (texture && texture != bound_texture_) {
    gl::activeTexture(gl::TEXTURE0);
    gl::bindTexture(gl::TEXTURE_2D, texture);
    bound_texture_ = texture;
  }
  const i32 cull = m.cull_mode();
  if (cull != cull_state_) { apply_cull_mode(cull); cull_state_ = cull; }
  gl::bindVertexArray(m.gpu.vao);
  const i32 instances = static_cast<i32>(m.instance_count);
  const u32 mode = mat.lines ? gl::LINES : gl::TRIANGLES;
  if (!m.indices.empty()) gl::drawElementsInstanced(mode, static_cast<i32>(m.indices.size()), m.gpu.element_type, 0, instances);
  else gl::drawArraysInstanced(mode, 0, static_cast<i32>(m.vertex_count()), instances);
  stats.draw_calls++;
  stats.instances += m.instance_count;
  stats.triangles += static_cast<f64>(m.triangle_count()) * m.instance_count;
}

bool Renderer::render(const RenderParams& params, RenderStats& stats) {
  if (!initialised_) return fail("renderer not initialised");
  if (lost_) return fail("GL context lost; waiting for restoration");
  const f64 start = now_ms();
  stats = RenderStats{};

  // Validate everything before any GL state changes: a rejected frame leaves
  // no framebuffer bound, no scissor enabled and no timer query open.
  Viewport rect{0, 0, width_, height_};
  if (params.has_viewport) {
    rect = params.viewport;
    if (rect.width < 1 || rect.height < 1 || rect.x < 0 || rect.y < 0 || rect.x + rect.width > width_ || rect.y + rect.height > height_)
      return fail("viewport must fit inside the drawing buffer");
  }
  if (!camera_valid(camera)) return fail("invalid camera projection");
  bool ok = false;
  const Mat4 view = Mat4::look_at(camera.eye, camera.target, camera.up, &ok);
  if (!ok) return fail("degenerate camera direction/up");
  const f32 aspect = static_cast<f32>(rect.width) / static_cast<f32>(rect.height);
  const Mat4 vp = projection_matrix(camera, aspect) * view;
  const Frustum frustum = Frustum::from_matrix(vp);

  // GPU timer results arrive asynchronously, a few frames late.
  if (timer_supported_) {
    std::size_t keep = 0;
    for (u32 q : pending_queries_) {
      if (gl::queryAvailable(q)) {
        if (!gl::gpuDisjoint()) last_gpu_ms_ = gl::queryResultMs(q);
        free_queries_.push_back(q);  // reused: no query object churn per frame
      } else pending_queries_[keep++] = q;
    }
    pending_queries_.resize(keep);
  }
  u32 timer_query = 0;
  if (timer_supported_ && pending_queries_.size() < 4) {
    if (!free_queries_.empty()) { timer_query = free_queries_.back(); free_queries_.pop_back(); }
    else timer_query = gl::createQuery();
    if (timer_query) gl::beginQuery(gl::TIME_ELAPSED, timer_query);
  }

  if (shadow_) {
    const u64 key = shadow_->focus_key(camera.target, lighting.sun_direction) ^ (shadow_revision_ * 0x9e3779b97f4a7c15ull) ^
                    (static_cast<u64>(params.layers) * 0xc2b2ae3d27d4eb4full);
    if (shadow_key_valid_ && key == shadow_key_) {
      stats.shadow_reused = true;
    } else {
      stats.shadow_draw_calls = shadow_->draw(meshes(), camera.target, lighting.sun_direction, params.layers);
      shadow_key_ = key;
      shadow_key_valid_ = true;
    }
    stats.shadow_resolution = static_cast<u32>(shadow_->options().resolution);
  }

  if (params.has_viewport) { gl::enable(gl::SCISSOR_TEST); gl::scissor(rect.x, rect.y, rect.width, rect.height); }
  else gl::disable(gl::SCISSOR_TEST);
  const bool post = !params.has_viewport && bloom_.begin(width_, height_, start, params.page_hidden);
  gl::viewport(rect.x, rect.y, rect.width, rect.height);
  gl::clearColor(params.clear_color.x, params.clear_color.y, params.clear_color.z, params.clear_color.w);
  gl::clearDepth(1);
  gl::depthMask(1);
  gl::clear(gl::COLOR_BUFFER_BIT | gl::DEPTH_BUFFER_BIT);

  opaque_.clear();
  transparent_.clear();
  viewmodel_.clear();
  for (Mesh* m : live_) {
    if (!m->visible || !m->gpu.vao || !(m->layers & params.layers)) continue;
    const Material& mat = m->material;
    if (!m->skinned() && mat.frustum_culled && !frustum.sphere_visible(m->world_bounds)) { stats.culled_meshes++; continue; }
    if (mat.viewmodel) { viewmodel_.push_back(m); continue; }
    m->sort_distance = distance_sq(m->world_bounds.center, camera.eye);
    (mat.alpha == AlphaMode::blend ? transparent_ : opaque_).push_back(m);
  }
  std::sort(opaque_.begin(), opaque_.end(), [](const Mesh* a, const Mesh* b) { return a->sort_distance < b->sort_distance; });
  std::sort(transparent_.begin(), transparent_.end(), [](const Mesh* a, const Mesh* b) { return a->sort_distance > b->sort_distance; });
  stats.visible_meshes = static_cast<u32>(opaque_.size() + transparent_.size() + viewmodel_.size());

  gl::enable(gl::DEPTH_TEST);
  gl::depthFunc(gl::LEQUAL);
  gl::disable(gl::BLEND);
  program_.use();
  bound_texture_ = ~0u;
  cull_state_ = -1;
  set_frame_uniforms(vp);
  for (Mesh* m : opaque_) draw_mesh(*m, stats);

  if (params.sky && !camera.orthographic) {
    SkyAtmosphere atmosphere;
    atmosphere.fog_color = lighting.fog_color;
    atmosphere.sun_color = lighting.sun_color;
    atmosphere.sun_direction = lighting.sun_direction;
    atmosphere.sun_scatter = lighting.fog_density > 0 ? clamp(lighting.fog_sun_scatter, 0.0f, 1.0f) : 0.0f;
    // How fogged the far end of the view is: thin fog gives only a thin horizon haze.
    const f32 far_fog = lighting.fog_density * min_of(camera.far, 2000.0f);
    atmosphere.horizon = clamp(lighting.fog_horizon, 0.0f, 1.0f) * (1 - std::exp(-far_fog * far_fog));
    sky_pass_.draw(camera, aspect, sky, lighting.exposure, start / 1000.0, atmosphere);
  }
  program_.use();
  cull_state_ = -1;

  if (!transparent_.empty()) {
    gl::enable(gl::DEPTH_TEST);
    gl::enable(gl::BLEND);
    gl::blendFunc(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
    gl::depthMask(0);
    for (Mesh* m : transparent_) draw_mesh(*m, stats);
    gl::depthMask(1);
    gl::disable(gl::BLEND);
  }
  // The first-person layer is drawn last after a depth clear, so a wall in front of the camera cannot cut it.
  if (!viewmodel_.empty()) {
    gl::clear(gl::DEPTH_BUFFER_BIT);
    for (Mesh* m : viewmodel_) draw_mesh(*m, stats);
  }
  gl::bindVertexArray(0);
  if (post) {
    bloom_.end(lighting.exposure);
    gl::enable(gl::DEPTH_TEST);
    gl::viewport(rect.x, rect.y, rect.width, rect.height);
    bound_texture_ = ~0u;
  }
  if (params.has_viewport) gl::disable(gl::SCISSOR_TEST);
  if (timer_query) {
    gl::endQuery(gl::TIME_ELAPSED);
    pending_queries_.push_back(timer_query);
  }
  stats.gpu_ms = last_gpu_ms_;
  stats.bloom = bloom_.state();
  stats.cpu_ms = now_ms() - start;
  stats_ = stats;
  return true;
}

// ---------------------------------------------------------------------------
// queries

bool Renderer::screen_ray(f32 x, f32 y, Ray& ray) const {
  return kern::screen_ray(camera, x, y, static_cast<f32>(width_), static_cast<f32>(height_), ray);
}

bool Renderer::pick(f32 x, f32 y, const PickQuery& query, PickHit& hit, PickStats* stats) const {
  Ray ray;
  if (!screen_ray(x, y, ray)) return false;
  return pick_ray(ray, query, hit, stats);
}

bool Renderer::pick_ray(const Ray& ray, const PickQuery& query, PickHit& hit, PickStats* stats) const {
  return pick_meshes(meshes(), ray, query, hit, stats);
}

// ---------------------------------------------------------------------------
// context loss

void Renderer::context_lost() {
  if (lost_) return;
  lost_ = true;
  for (Mesh* m : live_) m->gpu = MeshGpu{};
  for (Texture& t : textures_)
    if (!t.external) t.handle = 0;
  pending_queries_.clear();
  free_queries_.clear();
  if (shadow_) shadow_->destroy(true);
  shadow_.reset();
  shadow_key_valid_ = false;
  sky_pass_.destroy(true);
  bloom_.destroy(true);
  program_.destroy(true);
}

bool Renderer::context_restored() {
  if (!lost_) return true;
  // Stay lost until everything is rebuilt; a failed attempt frees what it made
  // (the context is alive) so a later restore can start from scratch.
  if (!init_gpu()) {
    if (shadow_) shadow_->destroy(false);
    shadow_.reset();
    sky_pass_.destroy(false);
    bloom_.destroy(false);
    program_.destroy(false);
    return false;
  }
  lost_ = false;
  for (Texture& t : textures_)
    if (t.alive && !t.external) upload_texture(t);
  for (Mesh* m : live_) upload(*m);
  ++scene_revision_;
  ++shadow_revision_;
  return true;
}

}  // namespace kern
