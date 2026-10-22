// Image planes and sampler volumes: textures from page-decoded pixels, alpha
// tracing into box fixtures, sampler compositing and Sobel / local-maxima
// edge images (all pixel work in C++).
#include "internal.h"
#include "kern/world/scene_meshes.h"

namespace kern::editor {

namespace {

std::string image_key(const std::string& src) {
  u64 h = 0xcbf29ce484222325ull;
  for (unsigned char c : src) { h ^= c; h *= 0x100000001b3ull; }
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

using Run = std::array<i32, 4>;  // x0, x1, y0, y1 (pixels, end exclusive)

/// Horizontal runs of opaque pixels merged vertically into rectangles (mergedAlphaRuns).
std::vector<Run> merged_alpha_runs(const u8* rgba, i32 w, i32 h, f64 threshold) {
  std::vector<Run> out;
  std::vector<std::pair<std::pair<i32, i32>, Run>> open, next;
  for (i32 y = 0; y < h; ++y) {
    next.clear();
    for (i32 x = 0; x < w;) {
      while (x < w && rgba[(static_cast<std::size_t>(y) * w + x) * 4 + 3] < threshold) ++x;
      if (x >= w) break;
      const i32 x0 = x;
      while (x < w && rgba[(static_cast<std::size_t>(y) * w + x) * 4 + 3] >= threshold) ++x;
      const std::pair<i32, i32> key{x0, x};
      Run r{x0, x, y, y + 1};
      for (std::size_t k = 0; k < open.size(); ++k)
        if (open[k].first == key) {
          r = open[k].second;
          r[3] = y + 1;
          open.erase(open.begin() + static_cast<std::ptrdiff_t>(k));
          break;
        }
      next.push_back({key, r});
    }
    for (auto& o : open) out.push_back(o.second);
    open = next;
  }
  for (auto& o : open) out.push_back(o.second);
  return out;
}

/// Area-average downscale (what a smoothing canvas drawImage does closely enough).
std::vector<u8> downscale(const ImageData& im, i32 w, i32 h) {
  std::vector<u8> out(static_cast<std::size_t>(w) * h * 4);
  const f64 sx = static_cast<f64>(im.width) / w, sy = static_cast<f64>(im.height) / h;
  for (i32 y = 0; y < h; ++y)
    for (i32 x = 0; x < w; ++x) {
      const i32 x0 = static_cast<i32>(x * sx), x1 = std::max(x0 + 1, static_cast<i32>((x + 1) * sx));
      const i32 y0 = static_cast<i32>(y * sy), y1 = std::max(y0 + 1, static_cast<i32>((y + 1) * sy));
      f64 acc[4] = {0, 0, 0, 0};
      f64 n = 0;
      for (i32 yy = y0; yy < std::min(y1, im.height); ++yy)
        for (i32 xx = x0; xx < std::min(x1, im.width); ++xx) {
          const u8* p = &im.rgba[(static_cast<std::size_t>(yy) * im.width + xx) * 4];
          const f64 a = p[3] / 255.0;
          acc[0] += p[0] * a; acc[1] += p[1] * a; acc[2] += p[2] * a; acc[3] += p[3];
          ++n;
        }
      u8* o = &out[(static_cast<std::size_t>(y) * w + x) * 4];
      const f64 alpha = n ? acc[3] / n : 0;
      for (int c = 0; c < 3; ++c) o[c] = static_cast<u8>(alpha > 0 ? std::clamp(acc[c] / n / (alpha / 255), 0.0, 255.0) : 0);
      o[3] = static_cast<u8>(std::clamp(alpha, 0.0, 255.0));
    }
  return out;
}

void edge_image(std::vector<u8>& rgba, i32 w, i32 h, bool maxima) {
  std::vector<f32> gray(static_cast<std::size_t>(w) * h), mag(static_cast<std::size_t>(w) * h, 0);
  for (std::size_t i = 0; i < gray.size(); ++i)
    gray[i] = (0.2126f * rgba[i * 4] + 0.7152f * rgba[i * 4 + 1] + 0.0722f * rgba[i * 4 + 2]) * rgba[i * 4 + 3] / 255.0f;
  for (i32 y = 1; y < h - 1; ++y)
    for (i32 x = 1; x < w - 1; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * w + x, W = static_cast<std::size_t>(w);
      const f32 gx = -gray[i - W - 1] + gray[i - W + 1] - 2 * gray[i - 1] + 2 * gray[i + 1] - gray[i + W - 1] + gray[i + W + 1];
      const f32 gy = -gray[i - W - 1] - 2 * gray[i - W] - gray[i - W + 1] + gray[i + W - 1] + 2 * gray[i + W] + gray[i + W + 1];
      mag[i] = min_of(255.0f, std::hypot(gx, gy));
    }
  for (i32 y = 0; y < h; ++y)
    for (i32 x = 0; x < w; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * w + x, W = static_cast<std::size_t>(w);
      f32 v = mag[i];
      if (maxima && x > 0 && x < w - 1 && y > 0 && y < h - 1 && (v < mag[i - 1] || v < mag[i + 1] || v < mag[i - W] || v < mag[i + W])) v = 0;
      const u8 g = static_cast<u8>(v);
      rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = g;
      rgba[i * 4 + 3] = 255;
    }
}

}  // namespace

ImageData* Editor::image_for(const world::SharedText& src) {
  if (!src || src->empty()) return nullptr;
  const std::string key = image_key(*src);
  image_sources_[key] = src;
  ImageData& im = images_[key];
  return &im;
}

u32 Editor::image_texture(const world::SharedText& src, bool nearest) {
  ImageData* im = image_for(src);
  if (!im || im->rgba.empty()) return 0;
  u32& tex = nearest ? im->texture_nearest : im->texture;
  if (!tex) {
    TextureDesc d;
    d.rgba = im->rgba.data();
    d.width = im->width;
    d.height = im->height;
    d.mipmaps = true;
    d.nearest = nearest;
    tex = renderer_->create_texture(d);
  }
  return tex;
}

json::Value Editor::pending_images() {
  json::Value out = json::Value::array();
  auto visit = [&](const world::Scene& s) {
    for (const ImagePlane& i : s.images) {
      ImageData* im = image_for(i.src);
      if (!im || im->requested || !im->rgba.empty()) continue;
      im->requested = true;
      out.push(image_key(*i.src));
    }
  };
  for (const auto& d : documents_) visit(d->scene);
  for (const auto& v : views_)
    if (v->runtime) visit(v->runtime->source());
  return out;
}

const std::string* Editor::image_source(std::string_view key) const {
  const auto it = image_sources_.find(std::string(key));
  return it == image_sources_.end() || !it->second ? nullptr : it->second.get();
}

bool Editor::set_image_pixels(std::string_view key, i32 width, i32 height, const u8* rgba) {
  auto it = images_.find(std::string(key));
  if (it == images_.end()) return fail("unknown image");
  ImageData& im = it->second;
  if (width < 1 || height < 1 || width > 4096 || height > 4096 || !rgba) {
    report("Could not decode image", "warn");
    return false;
  }
  im.width = width;
  im.height = height;
  im.rgba.assign(rgba, rgba + static_cast<std::size_t>(width) * height * 4);
  if (im.texture) { renderer_->delete_texture(im.texture); im.texture = 0; }
  if (im.texture_nearest) { renderer_->delete_texture(im.texture_nearest); im.texture_nearest = 0; }
  for (auto& v : views_) v->needs_sync = true;
  return true;
}

void Editor::add_run_fixtures(const std::string& body_id, const D3& center, const DQ& rotation, f64 width, f64 height,
                              const std::vector<Run>& runs, f64 res, f64 thickness, const std::string& label) {
  world::Scene& s = *scene();
  Body* bound = body_id.empty() ? nullptr : s.body(body_id);
  const DPose wt = item_world_pose(s, body_id, center, rotation);
  if (!bound) {
    Body b = new_body(BodyKind::fixed, wt.position, label + " body");
    b.rotation = wt.rotation;
    s.bodies.push_back(std::move(b));
  }
  Body& body = bound ? *bound : s.bodies.back();
  std::vector<std::string> ids;
  for (const Run& r : runs) {
    Fixture f = new_fixture(FixtureShape::box, label);
    const f64 x = (r[0] + r[1]) / 2.0 / res - width / 2;
    const f64 y = height / 2 - (r[2] + r[3]) / 2.0 / res;
    const D3 local{x, y, 0};
    f.size = {std::max(0.001, (r[1] - r[0]) / res), std::max(0.001, (r[3] - r[2]) / res), std::max(0.005, thickness)};
    if (bound) {
      f.center = add(center, qrotate(rotation, local));
      f.rotation = rotation;
    } else {
      f.center = local;
      f.rotation = kIdentityQ;
    }
    ids.push_back(f.id);
    body.fixtures.push_back(std::move(f));
  }
  mode_ = Mode::fixture;
  doc()->selection[Mode::fixture] = ids;
}

bool Editor::trace_image_to_boxes() {
  const std::vector<ItemRef> chosen_images = chosen(Mode::image);
  if (chosen_images.empty()) { report("Select an image first.", "warn"); return false; }
  const ImagePlane item = *scene()->image(chosen_images[0].id);
  ImageData* im = image_for(item.src);
  if (!im) { report("Image data is missing", "error"); return false; }
  if (im->rgba.empty()) { report("Image is still loading; try again in a moment.", "warn"); return false; }
  const f64 scale = std::min(1.0, 160.0 / std::max(im->width, im->height));
  const i32 w = std::max(1, static_cast<i32>(std::lround(im->width * scale))), h = std::max(1, static_cast<i32>(std::lround(im->height * scale)));
  const std::vector<u8> small = downscale(*im, w, h);
  const f64 res = w / item.size[0];
  const std::vector<Run> runs = merged_alpha_runs(small.data(), w, h, 32);
  if (runs.empty()) { report("No opaque image pixels", "error"); return false; }
  if (runs.size() > 3000) { report("More than 3000 generated fixtures; reduce resolution", "error"); return false; }
  const bool ok = edit("Create fixtures from image alpha", [&] {
    add_run_fixtures(item.body, item.center, item.rotation, item.size[0], item.size[1], runs, res, 0.08, "image alpha");
    return true;
  });
  if (ok) report("Created " + std::to_string(runs.size()) + " thin 3D fixtures from image alpha");
  return ok;
}

bool Editor::compose_sampler(const world::Sampler& sample, i32 limit, std::vector<u8>& out, i32& W, i32& H, f64& res) {
  const world::Scene& s = *scene();
  const f64 wm = std::max(0.001, sample.size[0] != 0 ? sample.size[0] : 2), hm = std::max(0.001, sample.size[1] != 0 ? sample.size[1] : 2);
  res = std::max(4.0, std::min(sample.resolution != 0 ? sample.resolution : 100, limit / std::max(wm, hm)));
  W = std::max(1, static_cast<i32>(std::lround(wm * res)));
  H = std::max(1, static_cast<i32>(std::lround(hm * res)));
  std::vector<f32> acc(static_cast<std::size_t>(W) * H * 4, 0.0f);  // premultiplied RGBA 0..1
  const DPose sw = item_world_pose(s, sample.body, sample.center, sample.rotation);
  const DQ inv = qinv(sw.rotation);
  const D3 sn = qrotate(sw.rotation, {0, 0, 1});
  bool missing = false;
  for (const ImagePlane& im : s.images) {
    ImageData* data = image_for(im.src);
    if (!data) continue;
    if (data->rgba.empty()) { missing = true; continue; }
    const DPose iw = item_world_pose(s, im.body, im.center, im.rotation);
    const D3 normal = qrotate(iw.rotation, {0, 0, 1});
    if (std::fabs(dot(sn, normal)) < 0.96) continue;
    const D3 local = qrotate(inv, sub(iw.position, sw.position));
    if (std::fabs(local[2]) > std::max(0.1, (sample.size[2] != 0 ? sample.size[2] : 0.1) / 2)) continue;
    const D3 e = euler_xyz(qmul_raw(inv, iw.rotation));
    const f64 cs = std::cos(e[2]), sn_ = std::sin(e[2]);
    const f64 ox = W / 2.0 + local[0] * res, oy = H / 2.0 - local[1] * res;
    const f64 dw = im.size[0] * res, dh = im.size[1] * res;
    const f64 alpha = im.opacity.value_or(1);
    const Vec3 tint = parse_hex_color(im.tint, {1, 1, 1});
    const bool nearest = im.filter == 1;
    for (i32 py = 0; py < H; ++py)
      for (i32 px = 0; px < W; ++px) {
        const f64 u = px + 0.5 - ox, v = py + 0.5 - oy;
        f64 lx = u * cs - v * sn_, ly = u * sn_ + v * cs;
        if (im.flip) lx = -lx;
        const f64 fx = (lx + dw / 2) / dw * data->width, fy = (ly + dh / 2) / dh * data->height;
        if (fx < 0 || fy < 0 || fx >= data->width || fy >= data->height) continue;
        f32 c[4];
        if (nearest) {
          const u8* p = &data->rgba[(static_cast<std::size_t>(fy) * data->width + static_cast<std::size_t>(fx)) * 4];
          for (int k = 0; k < 4; ++k) c[k] = p[k] / 255.0f;
        } else {
          const f64 gx = std::clamp(fx - 0.5, 0.0, data->width - 1.0), gy = std::clamp(fy - 0.5, 0.0, data->height - 1.0);
          const i32 x0 = static_cast<i32>(gx), y0 = static_cast<i32>(gy);
          const i32 x1 = std::min(x0 + 1, data->width - 1), y1 = std::min(y0 + 1, data->height - 1);
          const f32 tx = static_cast<f32>(gx - x0), ty = static_cast<f32>(gy - y0);
          auto px_at = [&](i32 xx, i32 yy) { return &data->rgba[(static_cast<std::size_t>(yy) * data->width + xx) * 4]; };
          const u8 *a = px_at(x0, y0), *b = px_at(x1, y0), *cc = px_at(x0, y1), *d = px_at(x1, y1);
          // Interpolate premultiplied values.
          f32 pa = 0;
          for (int k = 0; k < 4; ++k) c[k] = 0;
          const f32 w4[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
          const u8* q4[4] = {a, b, cc, d};
          for (int n = 0; n < 4; ++n) {
            const f32 al = q4[n][3] / 255.0f;
            for (int k = 0; k < 3; ++k) c[k] += q4[n][k] / 255.0f * al * w4[n];
            pa += al * w4[n];
          }
          for (int k = 0; k < 3; ++k) c[k] = pa > 0 ? c[k] / pa : 0;
          c[3] = pa;
        }
        const f32 a = c[3] * static_cast<f32>(alpha);
        if (a <= 0) continue;
        f32* dst = &acc[(static_cast<std::size_t>(py) * W + px) * 4];
        const f32 rgb[3] = {c[0] * tint.x, c[1] * tint.y, c[2] * tint.z};
        for (int k = 0; k < 3; ++k) dst[k] = rgb[k] * a + dst[k] * (1 - a);
        dst[3] = a + dst[3] * (1 - a);
      }
  }
  if (missing) report("Some images are still loading; the sample may be incomplete.", "warn");
  out.assign(static_cast<std::size_t>(W) * H * 4, 0);
  const f64 opacity = sample.opacity.value_or(1);
  const Vec3 tint = parse_hex_color(sample.tint, {1, 1, 1});
  for (i32 py = 0; py < H; ++py)
    for (i32 px = 0; px < W; ++px) {
      const i32 sx = sample.flip ? W - 1 - px : px;
      const f32* p = &acc[(static_cast<std::size_t>(py) * W + sx) * 4];
      u8* o = &out[(static_cast<std::size_t>(py) * W + px) * 4];
      const f32 a = p[3];
      const f32 rgb[3] = {a > 0 ? p[0] / a : 0, a > 0 ? p[1] / a : 0, a > 0 ? p[2] / a : 0};
      o[0] = static_cast<u8>(std::clamp(rgb[0] * tint.x * 255.0f, 0.0f, 255.0f));
      o[1] = static_cast<u8>(std::clamp(rgb[1] * tint.y * 255.0f, 0.0f, 255.0f));
      o[2] = static_cast<u8>(std::clamp(rgb[2] * tint.z * 255.0f, 0.0f, 255.0f));
      o[3] = static_cast<u8>(std::clamp(a * static_cast<f32>(opacity) * 255.0f, 0.0f, 255.0f));
    }
  return true;
}

bool Editor::sampler_action(std::string_view action, std::vector<u8>* rgba, i32* width, i32* height) {
  const std::vector<ItemRef> samplers = chosen(Mode::sampler);
  if (samplers.empty()) { report("Select a sampler first.", "warn"); return false; }
  const Sampler sample = *scene()->sampler(samplers[0].id);
  std::vector<u8> px;
  i32 w = 0, h = 0;
  f64 res = 0;
  compose_sampler(sample, action == "trace" ? 512 : 1536, px, w, h, res);
  if (action == "trace") {
    const std::vector<Run> runs = merged_alpha_runs(px.data(), w, h, sample.threshold != 0 ? sample.threshold : 32);
    if (runs.empty()) { report("No opaque pixels in sampler", "error"); return false; }
    if (runs.size() > 3000) { report("More than 3000 generated fixtures; reduce resolution", "error"); return false; }
    const bool ok = edit("Create fixtures from sampler alpha", [&] {
      add_run_fixtures(sample.body, sample.center, sample.rotation, sample.size[0], sample.size[1], runs, res,
                       std::max(0.01, sample.size[2] != 0 ? sample.size[2] : 0.08), "sampled alpha");
      return true;
    });
    if (ok) report("Created " + std::to_string(runs.size()) + " sampler fixture(s)");
    return ok;
  }
  if (action == "sobel" || action == "maxima") edge_image(px, w, h, action == "maxima");
  if (rgba) *rgba = std::move(px);
  if (width) *width = w;
  if (height) *height = h;
  return true;
}

}  // namespace kern::editor
