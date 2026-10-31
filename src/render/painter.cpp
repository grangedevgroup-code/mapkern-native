#include "render/painter.hpp"

#include <chrono>
#include <cmath>
#include <cstring>

#include "geo/mercator.hpp"
#include "style/color.hpp"
#include "render/shaders/map.glsl.h"

namespace mapkern {

namespace {

constexpr double kDeg = kPi / 180;

void copyMat(float* dst, const std::array<float, 16>& m) { std::memcpy(dst, m.data(), sizeof(float) * 16); }

void setColor(float* dst, const Color& c) {
  for (int i = 0; i < 4; i++) dst[i] = static_cast<float>(c[i]);
}

void set4(float* dst, double a, double b = 0, double c = 0, double d = 0) {
  dst[0] = static_cast<float>(a);
  dst[1] = static_cast<float>(b);
  dst[2] = static_cast<float>(c);
  dst[3] = static_cast<float>(d);
}

std::array<double, 2> pairOf(const Value& v) {
  if (v.isArray() && v.size() >= 2 && v[0].isNumber() && v[1].isNumber()) return {v[0].asNumber(), v[1].asNumber()};
  return {0, 0};
}

std::array<double, 2> screenOffset(const StyleLayer& layer, double bearing, double zoom, const char* name, const char* anchor) {
  EvalContext ctx;
  ctx.zoom = zoom;
  const auto [x, y] = pairOf(layer.paintValue(name, ctx));
  if (layer.fixed(anchor).stringOr("map") != "viewport" || bearing == 0) return {x, y};
  const double b = bearing * kDeg;
  return {x * std::cos(b) - y * std::sin(b), x * std::sin(b) + y * std::cos(b)};
}

std::array<double, 3> normalize3(double x, double y, double z) {
  const double l = std::hypot(x, y, z);
  return {x / l, y / l, z / l};
}

const std::array<double, 3> kLight = normalize3(-0.45, -0.65, 0.75);

void hueRows(double degrees, float rows[3][4]) {
  const double a = degrees * kPi / 180;
  const double c = std::cos(a);
  const double k = (1 - c) / 3;
  const double r = std::sin(a) / std::sqrt(3.0);
  const double m[9] = {c + k, k + r, k - r, k - r, c + k, k + r, k + r, k - r, c + k};
  for (int row = 0; row < 3; row++) {
    rows[row][0] = static_cast<float>(m[0 * 3 + row]);
    rows[row][1] = static_cast<float>(m[1 * 3 + row]);
    rows[row][2] = static_cast<float>(m[2 * 3 + row]);
    rows[row][3] = 0;
  }
}


}

sg_pipeline Painter::pipeline(Program program, sg_primitive_type primitive, StencilMode stencil, DepthMode depth, bool colorWrite, Blend blend) {
  PipelineKey key;
  key.program = program;
  key.primitive = primitive;
  key.stencil = stencil;
  key.depth = depth;
  key.colorWrite = colorWrite;
  key.blend = blend;
  key.color = format_.color;
  key.depthFormat = format_.depth;
  key.samples = format_.samples;
  return Gpu::get().pipeline(key);
}

void Painter::bindPaint(sg_bindings& b, const PaintTable& paint) {
  b.views[VIEW_mk_paint_tex] = paint.texture().view;
  b.samplers[SMP_mk_paint_smp] = Gpu::get().sampler(false);
}

bool RenderView::overlaps(int z, int x, int y, int wrap, double margin) const {
  if (!region) return true;
  const double n = std::exp2(z);
  const double pad = margin / n;
  const double x0 = wrap + x / n - pad, y0 = y / n - pad, span = 1 / n + pad * 2;
  return x0 < region->x1 && x0 + span > region->x0 && y0 < region->y1 && y0 + span > region->y0;
}

void DrapeEntry::release() {
  if (sg_isvalid()) {
    if (attachment.id) sg_destroy_view(attachment);
    if (texture.id) sg_destroy_view(texture);
    if (image.id) sg_destroy_image(image);
  }
  attachment = {};
  texture = {};
  image = {};
}

void Painter::resetGpu() {
  drapes_.clear();
  meshVertices_ = {};
  meshIndices_ = {};
  capVertices_ = {};
  capIndices_ = {};
  meshCount_ = capCount_ = 0;
  poleImage_ = {};
  poleTexture_ = {};
  poleAttachment_ = {};
  flatDem_ = {};
  labels_.resetGpu();
}

Painter::~Painter() {
  for (auto& [k, d] : drapes_) d.release();
  meshVertices_.release();
  meshIndices_.release();
  capVertices_.release();
  capIndices_.release();
  flatDem_.release();
  if (sg_isvalid()) {
    if (poleAttachment_.id) sg_destroy_view(poleAttachment_);
    if (poleTexture_.id) sg_destroy_view(poleTexture_);
    if (poleImage_.id) sg_destroy_image(poleImage_);
  }
}

namespace {

constexpr int kGrid = 64;
constexpr size_t kDrapeCacheBytes = 32u << 20;
const Color kSpace{0.035, 0.045, 0.085, 1};
const Color kGlobeLand{0.11, 0.19, 0.27, 1};

bool isDraped(LayerType t) {
  return t == LayerType::Background || t == LayerType::Raster || t == LayerType::Hillshade || t == LayerType::Fill ||
         t == LayerType::Line || t == LayerType::Circle || t == LayerType::Heatmap;
}

uint64_t mix64(uint64_t h, uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h;
}

uint64_t mixd(uint64_t h, double v) {
  uint64_t bits;
  std::memcpy(&bits, &v, 8);
  return mix64(h, bits);
}

Color over(const Color& top, const Color& bottom) {
  const double k = 1 - top[3];
  return {top[0] * top[3] + bottom[0] * k, top[1] * top[3] + bottom[1] * k, top[2] * top[3] + bottom[2] * k, top[3] + bottom[3] * k};
}

int drapeSize(Transform& t, const CoveringTile& slot, double pixelRatio) {
  const double pixels = std::sqrt(t.tileArea(slot.z, slot.x, slot.y, slot.wrap)) * pixelRatio;
  return pixels > 600 ? 1024 : pixels > 300 ? 512 : 256;
}

}

void Painter::ensureMeshes() {
  if (meshCount_ > 0) return;
  Gpu& gpu = Gpu::get();
  std::vector<int16_t> v;
  std::vector<uint32_t> idx;
  const int side = kGrid + 1;
  for (int j = 0; j <= kGrid; j++)
    for (int i = 0; i <= kGrid; i++) v.insert(v.end(), {static_cast<int16_t>(i), static_cast<int16_t>(j), 0, 0});
  for (int j = 0; j < kGrid; j++)
    for (int i = 0; i < kGrid; i++) {
      const uint32_t a = j * side + i, b = a + 1, c = a + side, d = c + 1;
      idx.insert(idx.end(), {a, c, b, b, c, d});
    }
  auto skirt = [&](const std::vector<uint32_t>& edge) {
    const uint32_t base = static_cast<uint32_t>(v.size() / 4);
    for (uint32_t p : edge) v.insert(v.end(), {v[p * 4], v[p * 4 + 1], 1, 0});
    for (size_t k = 0; k + 1 < edge.size(); k++) {
      const uint32_t a = edge[k], b = edge[k + 1], k0 = base + static_cast<uint32_t>(k);
      idx.insert(idx.end(), {a, k0, b, b, k0, k0 + 1});
    }
  };
  std::vector<uint32_t> top, bottom, left, right;
  for (int k = 0; k < side; k++) {
    top.push_back(k);
    bottom.push_back(kGrid * side + k);
    left.push_back(k * side);
    right.push_back(k * side + kGrid);
  }
  skirt(top);
  skirt(bottom);
  skirt(left);
  skirt(right);
  meshVertices_ = gpu.vertexBuffer(v.data(), v.size() * 2);
  meshIndices_ = gpu.indexBuffer(idx.data(), idx.size() * 4);
  meshCount_ = static_cast<uint32_t>(idx.size());
  std::vector<int16_t> cv;
  std::vector<uint32_t> ci;
  for (int row : {0, kGrid}) {
    const uint32_t base = static_cast<uint32_t>(cv.size() / 4);
    for (int i = 0; i <= kGrid; i++) {
      cv.insert(cv.end(), {static_cast<int16_t>(i), static_cast<int16_t>(row), 0, 0});
      cv.insert(cv.end(), {static_cast<int16_t>(i), static_cast<int16_t>(row), 0, 1});
    }
    for (int k = 0; k < kGrid; k++) {
      const uint32_t e = base + k * 2;
      ci.insert(ci.end(), {e, e + 1, e + 2, e + 2, e + 1, e + 3});
    }
  }
  capVertices_ = gpu.vertexBuffer(cv.data(), cv.size() * 2);
  capIndices_ = gpu.indexBuffer(ci.data(), ci.size() * 4);
  capCount_ = static_cast<uint32_t>(ci.size());
  const float zero[4] = {0, 0, 0, 0};
  flatDem_ = gpu.texture(1, 1, SG_PIXELFORMAT_RGBA32F, zero, 16);
  sg_image_desc pd{};
  pd.usage.color_attachment = true;
  pd.width = 2;
  pd.height = 1;
  pd.pixel_format = SG_PIXELFORMAT_RGBA8;
  pd.sample_count = 1;
  poleImage_ = sg_make_image(&pd);
  sg_view_desc tv{};
  tv.texture.image = poleImage_;
  poleTexture_ = sg_make_view(&tv);
  sg_view_desc av{};
  av.color_attachment.image = poleImage_;
  poleAttachment_ = sg_make_view(&av);
}

std::pair<const DrapeEntry*, std::array<float, 4>> Painter::slotDrape(const CoveringTile& slot) {
  auto it = drapes_.find(slotKey(slot));
  if (it != drapes_.end() && it->second.drawn) return {&it->second, {0, 0, 1, 1}};
  for (int dz = 1; dz <= 4 && dz <= slot.z; dz++) {
    const int n = 1 << dz;
    const int x = slot.x >> dz, y = slot.y >> dz;
    auto p = drapes_.find(slotKey({slot.z - dz, x, y, slot.wrap}));
    if (p != drapes_.end() && p->second.drawn)
      return {&p->second, {static_cast<float>(slot.x - x * n) / n, static_cast<float>(slot.y - y * n) / n, 1.0f / n, 1.0f / n}};
  }
  return {nullptr, {0, 0, 1, 1}};
}

void Painter::renderDrapes(const FrameInput& frame, const std::vector<CoveringTile>& slots) {
  Transform& t = *frame.transform;
  const double zoom = t.zoom();
  std::vector<const StyleLayer*> layers;
  for (const auto& l : *frame.layers)
    if (isDraped(l->type) && l->isVisible(zoom)) layers.push_back(l.get());
  uint64_t common = mixd(0, frame.pixelRatio);
  std::vector<const std::vector<RenderPiece<VectorTileData>>*> vectorPieces;
  std::vector<const std::vector<RenderPiece<RasterTileData>>*> rasterPieces;
  std::vector<const std::vector<RenderPiece<DemTileData>>*> demPieces;
  for (const StyleLayer* l : layers) {
    common = mix64(mix64(mix64(common, l->uid), l->revision), l->motion);
    auto it = frame.sources->find(l->source);
    if (it == frame.sources->end()) continue;
    if (auto* v = dynamic_cast<VectorSource*>(it->second.get()); v && v->manager) {
      if (std::find(vectorPieces.begin(), vectorPieces.end(), &v->manager->pieces) == vectorPieces.end()) vectorPieces.push_back(&v->manager->pieces);
    }
    if (auto* r = dynamic_cast<RasterSource*>(it->second.get()); r && r->manager) {
      if (std::find(rasterPieces.begin(), rasterPieces.end(), &r->manager->pieces) == rasterPieces.end()) rasterPieces.push_back(&r->manager->pieces);
    }
    if (auto* d = dynamic_cast<DemSource*>(it->second.get()); d && d->manager) {
      if (std::find(demPieces.begin(), demPieces.end(), &d->manager->pieces) == demPieces.end()) demPieces.push_back(&d->manager->pieces);
    }
  }
  if (std::any_of(layers.begin(), layers.end(), [](const StyleLayer* l) {
        return l->type == LayerType::Hillshade && l->fixed("illuminationAnchor").stringOr("viewport") != "map";
      }))
    common = mixd(common, t.bearing());
  const PassFormat saved = format_;
  format_ = {SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_NONE, 1};
  const double deadline = nowMs() + 6;
  int redrawn = 0;
  drapesBehind_ = false;
  for (const auto& slot : slots) {
    const int size = drapeSize(t, slot, frame.pixelRatio);
    const double n = std::exp2(slot.z);
    RenderView view;
    view.zoom = zoom;
    view.bearing = 0;
    double scale = zoom;
    if (const double g = t.globeAmount(); g > 0) {
      const double north = std::atan(std::sinh(kPi * (1 - 2 * slot.y / n)));
      const double south = std::atan(std::sinh(kPi * (1 - 2 * (slot.y + 1) / n)));
      const double tileCos = south <= 0 && north >= 0 ? 1 : std::cos(std::min(std::abs(south), std::abs(north)));
      const double centerCos = 1 / std::cosh(kPi * (1 - 2 * t.centerWorld()[1]));
      scale = zoom + g * std::log2(tileCos / centerCos);
      view.pixelZoom = scale;
    }
    view.width = view.height = 512 * std::exp2(scale - slot.z);
    const double x0 = slot.wrap + slot.x / n, y0 = slot.y / n, span = 1 / n;
    view.region = Rect{x0, y0, x0 + span, y0 + span};
    view.tileMatrix = [x0, y0, span](int z, int x, int y, int wrap, double extent) {
      const double tn = std::exp2(z);
      const double scale = 1 / (extent * tn * span);
      std::array<float, 16> m{};
      m[0] = static_cast<float>(2 * scale);
      m[5] = static_cast<float>(-2 * scale);
      m[10] = 1;
      m[12] = static_cast<float>(2 * (wrap + x / tn - x0) / span - 1);
      m[13] = static_cast<float>(1 - 2 * (y / tn - y0) / span);
      m[15] = 1;
      return m;
    };
    uint64_t sig = mix64(common, static_cast<uint64_t>(size));
    for (auto* pieces : vectorPieces) {
      for (const auto& p : *pieces) {
        if (!view.overlaps(p.tile->z, p.tile->x, p.tile->y, p.wrap, 0.125)) continue;
        sig = mixd(mixd(mix64(mix64(sig, p.tile->key), p.tile->revision), p.opacity), p.clip.x0 + p.clip.y0 * 3 + p.clip.x1 * 7 + p.wrap * 11);
      }
      sig = mix64(sig, 0xffff);
    }
    for (auto* pieces : rasterPieces) {
      for (const auto& p : *pieces) {
        if (!view.overlaps(p.tile->z, p.tile->x, p.tile->y, p.wrap)) continue;
        sig = mixd(mixd(mix64(mix64(sig, p.tile->key), p.tile->revision + (p.tile->data && p.tile->data->texture.valid() ? 1 : 0)), p.opacity),
                   p.clip.x0 + p.clip.y0 * 3 + p.clip.x1 * 7 + p.wrap * 11);
      }
      sig = mix64(sig, 0xfffe);
    }
    for (auto* pieces : demPieces) {
      for (const auto& p : *pieces) {
        if (!view.overlaps(p.tile->z, p.tile->x, p.tile->y, p.wrap)) continue;
        sig = mixd(mixd(mix64(mix64(sig, p.tile->key), p.tile->revision + (p.tile->data && p.tile->data->gradientTexture.valid() ? 1 : 0)), p.opacity),
                   p.clip.x0 + p.clip.y0 * 3 + p.clip.x1 * 7 + p.wrap * 11);
      }
      sig = mix64(sig, 0xfffd);
    }
    DrapeEntry& entry = drapes_[slotKey(slot)];
    entry.used = frameCount_;
    if (entry.drawn && entry.size == size && entry.signature == sig) continue;
    const bool busy = nowMs() > deadline || redrawn >= 6;
    if (busy && (entry.drawn || slotDrape(slot).first)) {
      drapesBehind_ = true;
      continue;
    }
    if (entry.size != size) {
      entry.release();
      sg_image_desc d{};
      d.usage.color_attachment = true;
      d.width = size;
      d.height = size;
      d.pixel_format = SG_PIXELFORMAT_RGBA8;
      d.sample_count = 1;
      entry.image = sg_make_image(&d);
      sg_view_desc tv{};
      tv.texture.image = entry.image;
      entry.texture = sg_make_view(&tv);
      sg_view_desc av{};
      av.color_attachment.image = entry.image;
      entry.attachment = sg_make_view(&av);
      entry.size = size;
    }
    sg_pass pass{};
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value = {0, 0, 0, 0};
    pass.attachments.colors[0] = entry.attachment;
    sg_begin_pass(&pass);
    FrameInput sub = frame;
    sub.view = &view;
    sub.pixelRatio = size / view.width;
    for (const StyleLayer* l : layers) drawLayer(sub, *l);
    sg_end_pass();
    entry.signature = sig;
    entry.drawn = true;
    entry.zoom = zoom;
    redrawn++;
    stats_.drapes++;
  }
  format_ = saved;
  size_t bytes = 0;
  for (const auto& [k, d] : drapes_) bytes += static_cast<size_t>(d.size) * d.size * 4;
  if (bytes > kDrapeCacheBytes) {
    std::vector<std::pair<uint64_t, uint64_t>> order;
    for (const auto& [k, d] : drapes_)
      if (d.used != frameCount_) order.emplace_back(d.used, k);
    std::sort(order.begin(), order.end());
    for (const auto& [used, k] : order) {
      if (bytes <= kDrapeCacheBytes) break;
      auto& d = drapes_[k];
      bytes -= static_cast<size_t>(d.size) * d.size * 4;
      d.release();
      drapes_.erase(k);
    }
  }
}

void Painter::renderPoles(const std::vector<CoveringTile>& slots) {
  std::vector<const CoveringTile*> north, south;
  for (const auto& s : slots) {
    const int n = 1 << s.z;
    if (s.y == 0) north.push_back(&s);
    if (s.y == n - 1) south.push_back(&s);
  }
  const PassFormat saved = format_;
  format_ = {SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_NONE, 1};
  sg_pass pass{};
  pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
  pass.action.colors[0].clear_value = {0, 0, 0, 0};
  pass.attachments.colors[0] = poleAttachment_;
  sg_begin_pass(&pass);
  const bool flip = Gpu::get().backend() == SG_BACKEND_GLCORE || Gpu::get().backend() == SG_BACKEND_GLES3;
  sg_apply_pipeline(pipeline(Program::Pole, SG_PRIMITIVETYPE_TRIANGLES, StencilMode::Off, DepthMode::Off, true, Blend::Additive));
  for (int side = 0; side < 2; side++) {
    auto& list = side == 0 ? north : south;
    if (list.empty()) continue;
    std::vector<const CoveringTile*> picked;
    const size_t want = std::min<size_t>(16, list.size());
    for (size_t i = 0; i < want; i++) picked.push_back(list[i * list.size() / want]);
    for (const CoveringTile* slot : picked) {
      auto [entry, rect] = slotDrape(*slot);
      if (!entry) continue;
      sg_bindings b{};
      b.vertex_buffers[0] = Gpu::get().quad();
      b.views[VIEW_mk_drape_tex] = entry->texture;
      b.samplers[SMP_mk_drape_smp] = Gpu::get().sampler(true);
      sg_apply_bindings(&b);
      mk_pole_vs_params_t vs{};
      set4(vs.u_target, side == 0 ? -0.5 : 0.5);
      sg_apply_uniforms(UB_mk_pole_vs_params, SG_RANGE(vs));
      mk_pole_fs_params_t fs{};
      set4(fs.u_rect, rect[0], rect[1], rect[2], rect[3]);
      set4(fs.u_info, side == 0 ? 1.0 / 16 : 15.0 / 16, 1.0 / picked.size(), flip ? 1 : 0);
      sg_apply_uniforms(UB_mk_pole_fs_params, SG_RANGE(fs));
      sg_draw(0, 6, 1);
      stats_.drawCalls++;
    }
  }
  sg_end_pass();
  format_ = saved;
}

void Painter::drawAtmosphere(const FrameInput& frame) {
  Transform& t = *frame.transform;
  sg_apply_pipeline(pipeline(Program::Atmosphere, SG_PRIMITIVETYPE_TRIANGLES, StencilMode::Off, DepthMode::Off, true, Blend::None));
  sg_bindings b{};
  b.vertex_buffers[0] = Gpu::get().quad();
  sg_apply_bindings(&b);
  mk_atmosphere_params_t p{};
  copyMat(p.u_inverse, mat4::toFloat(t.globeMatrixInverse));
  const auto cam = t.globeCamera();
  set4(p.u_camera, cam[0], cam[1], cam[2], t.globeRadius());
  setColor(p.u_space, kSpace);
  setColor(p.u_sky, Color{0.66, 0.79, 0.93, 1});
  set4(p.u_sun, 0, 0, 1, 0);
  const double day = std::clamp((t.zoom() - 5) / 4, 0.0, 1.0);
  set4(p.u_misc, day * day * (3 - 2 * day));
  sg_apply_uniforms(UB_mk_atmosphere_params, SG_RANGE(p));
  sg_draw(0, 6, 1);
  stats_.drawCalls++;
}

void Painter::drawGlobe(const FrameInput& frame, const std::vector<CoveringTile>& slots, const Color& blank) {
  Transform& t = *frame.transform;
  Gpu& gpu = Gpu::get();
  const bool flip = gpu.backend() == SG_BACKEND_GLCORE || gpu.backend() == SG_BACKEND_GLES3;
  sg_apply_pipeline(pipeline(Program::Globe, SG_PRIMITIVETYPE_TRIANGLES, StencilMode::Off, DepthMode::TestWrite));
  const double ws = t.worldSize();
  const auto c = t.centerWorld();
  const double m = kPi * (1 - 2 * c[1]);
  const Color base = blank[3] >= 0.999 ? blank : over(blank, kGlobeLand);
  const auto eye = t.globeCamera();
  const double strength = t.globeAmount() * (1 - 0.6 * std::clamp((t.zoom() - 4) / 5, 0.0, 1.0));
  const double exaggeration = frame.terrain ? frame.terrain->exaggeration : 0;
  const double relief = frame.terrain ? frame.terrain->relief : 0;
  const double azimuth = (315 + t.bearing()) * kDeg;
  for (size_t si = 0; si < slots.size(); si++) {
    const CoveringTile& slot = slots[si];
    auto [entry, rect] = slotDrape(slot);
    const SlotDem dem = slotDem(frame, si);
    const int n = 1 << slot.z;
    for (int cap = 0; cap < 2; cap++) {
      if (cap == 1 && !(slot.y == 0 || slot.y == n - 1)) continue;
      sg_bindings b{};
      b.vertex_buffers[0] = cap ? capVertices_.buffer : meshVertices_.buffer;
      b.index_buffer = cap ? capIndices_.buffer : meshIndices_.buffer;
      b.views[VIEW_mk_drape_tex] = entry ? entry->texture : gpu.blankTexture().view;
      b.samplers[SMP_mk_drape_smp] = gpu.sampler(true);
      b.views[VIEW_mk_pole_tex] = poleTexture_;
      b.samplers[SMP_mk_pole_smp] = gpu.sampler(false);
      b.views[VIEW_mk_dem_tex] = dem.dem;
      b.samplers[SMP_mk_dem_smp] = gpu.sampler(false);
      b.views[VIEW_mk_gradient_tex] = dem.gradient;
      b.samplers[SMP_mk_gradient_smp] = gpu.sampler(true);
      sg_apply_bindings(&b);
      mk_globe_vs_params_t vs{};
      copyMat(vs.u_matrix, mat4::toFloat(t.globeMatrix));
      set4(vs.u_globe, t.globeAmount(), t.globeRadius(), ws, std::cosh(m));
      set4(vs.u_center, c[1], std::tanh(m), 1 / std::cosh(m));
      double rx = slot.wrap + static_cast<double>(slot.x) / n - c[0];
      rx -= std::round(rx);
      set4(vs.u_tile, rx, static_cast<double>(slot.y) / n - c[1], 1.0 / n);
      set4(vs.u_tile_world, slot.wrap + static_cast<double>(slot.x) / n, static_cast<double>(slot.y) / n, 1.0 / n);
      set4(vs.u_dem_rect, dem.rect[0], dem.rect[1], dem.rect[2], dem.rect[3]);
      set4(vs.u_dem_info, dem.size, dem.has ? 1 : 0, exaggeration, std::max(30.0, dem.range * 0.2) * std::max(1.0, exaggeration));
      sg_apply_uniforms(UB_mk_globe_vs_params, SG_RANGE(vs));
      mk_globe_fs_params_t fs{};
      set4(fs.u_drape_rect, rect[0], rect[1], rect[2], rect[3]);
      setColor(fs.u_base, base);
      set4(fs.u_atmo, eye[0], eye[1], eye[2], strength);
      set4(fs.u_misc, cap, t.globeRadius(), flip ? 1 : 0);
      if (relief > 0) set4(fs.u_sun, std::sin(azimuth) * std::cos(kPi / 4), -std::cos(azimuth) * std::cos(kPi / 4), std::sin(kPi / 4), relief);
      const double cells = dem.size;
      set4(fs.u_grad, cells / (cells + 2), 1 / (cells + 2), dem.has ? std::exp2(dem.z) * cells / kEarthCircumference : 0, 0);
      set4(fs.u_rows2, static_cast<double>(slot.y) / n, static_cast<double>(slot.y + 1) / n, exaggeration, dem.has ? 1 : 0);
      set4(fs.u_dem_rect2, dem.rect[0], dem.rect[1], dem.rect[2], dem.rect[3]);
      sg_apply_uniforms(UB_mk_globe_fs_params, SG_RANGE(fs));
      if (cap == 0) {
        sg_draw(0, static_cast<int>(meshCount_), 1);
      } else {
        const int half = static_cast<int>(capCount_ / 2);
        if (slot.y == 0) sg_draw(0, half, 1);
        if (slot.y == n - 1) sg_draw(half, half, 1);
      }
      stats_.drawCalls++;
      stats_.pieces++;
    }
  }
}

bool Painter::render(const FrameInput& frame, const FrameTarget& target) {
  const auto started = std::chrono::steady_clock::now();
  stats_ = {};
  frameCount_++;
  format_ = target.offscreen ? target.format
                             : PassFormat{target.swapchain.color_format, target.swapchain.depth_format, target.swapchain.sample_count};
  Transform& t = *frame.transform;
  const double zoom = frame.view->zoom;
  const bool globe = t.globeAmount() > 0;
  std::vector<CoveringTile> slots;
  Color clear = frame.clearColor;
  size_t start = 0;
  const auto& layers = *frame.layers;
  if (!layers.empty() && layers[0]->type == LayerType::Background && layers[0]->isVisible(zoom)) {
    EvalContext ctx;
    ctx.zoom = zoom;
    clear = premultiply(layers[0]->paintColor("color", ctx), layers[0]->paintNumber("opacity", ctx));
    start = 1;
  }
  const bool terrain = frame.terrain && !globe;
  ensureMeshes();
  if (globe || terrain) {
    if (frame.terrain) {
      for (const auto& s : frame.terrain->slots) slots.push_back(s.cover);
    } else {
      CoverOptions cover;
      cover.tileSize = 512;
      cover.minZoom = 0;
      cover.maxZoom = std::min(22, static_cast<int>(std::ceil(t.maxZoom)));
      cover.round = true;
      slots = t.coveringTiles(cover);
    }
    renderDrapes(frame, slots);
    if (globe) renderPoles(slots);
  } else {
    drapesBehind_ = false;
  }
  sg_pass pass{};
  pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
  pass.action.colors[0].clear_value = {static_cast<float>(clear[0]), static_cast<float>(clear[1]), static_cast<float>(clear[2]),
                                       static_cast<float>(clear[3])};
  pass.action.depth.load_action = SG_LOADACTION_CLEAR;
  pass.action.depth.clear_value = 1.0f;
  pass.action.stencil.load_action = SG_LOADACTION_CLEAR;
  pass.action.stencil.clear_value = 0;
  if (target.offscreen) pass.attachments = target.attachments;
  else pass.swapchain = target.swapchain;
  sg_begin_pass(&pass);
  if (globe) {
    drawAtmosphere(frame);
    drawGlobe(frame, slots, clear);
    for (const auto& layer : layers)
      if (layer->type == LayerType::Extrusion && layer->isVisible(zoom) && t.globeAmount() < 0.01) drawLayer(frame, *layer);
  } else if (terrain) {
    drawTerrain(frame, slots);
    for (const auto& layer : layers)
      if (!isDraped(layer->type) && layer->isVisible(zoom)) drawLayer(frame, *layer);
  } else {
    for (size_t i = start; i < layers.size(); i++) {
      const auto& layer = layers[i];
      if (!layer->isVisible(zoom)) continue;
      drawLayer(frame, *layer);
      stats_.layers++;
    }
  }
  drawLabels(frame);
  if (frame.overlay) stats_.drawCalls += frame.overlay->draw(format_, t.width, t.height);
  sg_end_pass();
  sg_commit();
  stats_.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return true;
}

void Painter::drawLabels(const FrameInput& frame) {
  Transform& t = *frame.transform;
  const auto c = t.centerWorld();
  const std::array<double, 8> camera{c[0], c[1], t.zoom(), t.bearing(), t.pitch(), t.width, t.height, t.globeAmount()};
  if (camera != lastCamera_) {
    lastCamera_ = camera;
    cameraVersion_++;
  }
  std::vector<SymbolLayerInput> inputs;
  for (const auto& layer : *frame.layers) {
    if (layer->type != LayerType::Symbol) continue;
    auto it = frame.sources->find(layer->source);
    if (it == frame.sources->end()) continue;
    auto* v = dynamic_cast<VectorSource*>(it->second.get());
    if (!v || !v->manager) continue;
    inputs.push_back({layer.get(), v->manager.get()});
  }
  if (inputs.empty() && labels_.visibleCount() == 0) {
    labelsAnimating_ = false;
    return;
  }
  LabelTerrain ground;
  const LabelTerrain* groundPtr = nullptr;
  if (frame.terrain && t.globeAmount() <= 0) {
    Terrain* terrain = frame.terrain;
    const auto cam = t.cameraPosition();
    const double ws = t.worldSize();
    const std::array<double, 3> eye{cam[0] / ws, cam[1] / ws, cam[2] / ws};
    ground.height = [terrain](double x, double y) { return terrain->heightAt(x, y); };
    ground.hidden = [terrain, eye](double x, double y, double z) { return terrain->hidden(eye, x, y, z); };
    groundPtr = &ground;
    uint64_t sig = 0;
    for (const auto& slot : terrain->slots)
      if (slot.dem) sig = mix64(mix64(sig, slot.dem->key), slot.dem->revision);
    sig = mixd(sig, terrain->exaggeration);
    if (sig != terrainSignature_) {
      terrainSignature_ = sig;
      labels_.invalidatePlacement();
    }
  }
  labelsAnimating_ = labels_.update(t, inputs, frame.now, cameraVersion_, frame.pixelRatio, groundPtr);
  stats_.drawCalls += labels_.draw(t, frame.pixelRatio, format_);
}

void Painter::drawLayer(const FrameInput& frame, const StyleLayer& layer) {
  if (layer.type == LayerType::Background) {
    drawBackground(frame, layer);
    return;
  }
  auto it = frame.sources->find(layer.source);
  if (it == frame.sources->end()) return;
  Source* source = it->second.get();
  if (layer.type == LayerType::Raster) {
    if (auto* raster = dynamic_cast<RasterSource*>(source)) drawRaster(frame, layer, *raster);
    return;
  }
  if (layer.type == LayerType::Hillshade) {
    if (auto* dem = dynamic_cast<DemSource*>(source)) drawHillshade(frame, layer, *dem);
    return;
  }
  if (auto* vector = dynamic_cast<VectorSource*>(source)) drawVector(frame, layer, *vector);
}

void Painter::drawBackground(const FrameInput& frame, const StyleLayer& layer) {
  EvalContext ctx;
  ctx.zoom = frame.view->zoom;
  const Color c = layer.paintColor("color", ctx);
  const double opacity = layer.paintNumber("opacity", ctx);
  const Color pm = premultiply(c, opacity);
  if (pm[3] <= 0) return;
  sg_apply_pipeline(pipeline(Program::Solid, SG_PRIMITIVETYPE_TRIANGLES, StencilMode::Off));
  sg_bindings b{};
  b.vertex_buffers[0] = Gpu::get().quad();
  sg_apply_bindings(&b);
  mk_solid_params_t p{};
  const float m[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1};
  std::memcpy(p.u_matrix, m, sizeof m);
  setColor(p.u_color, pm);
  sg_apply_uniforms(UB_mk_solid_params, SG_RANGE(p));
  sg_draw(0, 6, 1);
  stats_.drawCalls++;
}

void Painter::drawRaster(const FrameInput& frame, const StyleLayer& layer, RasterSource& source) {
  if (!source.manager) return;
  const RenderView& t = *frame.view;
  EvalContext ctx;
  ctx.zoom = t.zoom;
  const double opacity = layer.paintNumber("opacity", ctx);
  if (!(opacity > 0)) return;
  mk_raster_fs_params_t fs{};
  const double contrast = std::clamp(layer.paintNumber("contrast", ctx), -1.0, 0.99);
  set4(fs.u_adjust, layer.paintNumber("brightnessMin", ctx), layer.paintNumber("brightnessMax", ctx),
       std::clamp(layer.paintNumber("saturation", ctx), -1.0, 1.0), contrast > 0 ? 1 / (1 - contrast) : 1 + contrast);
  float hue[3][4];
  hueRows(layer.paintNumber("hueRotate", ctx), hue);
  std::memcpy(fs.u_hue0, hue[0], 16);
  std::memcpy(fs.u_hue1, hue[1], 16);
  std::memcpy(fs.u_hue2, hue[2], 16);
  const bool nearest = layer.fixed("resampling").stringOr("linear") == "nearest";
  sg_apply_pipeline(pipeline(Program::Raster, SG_PRIMITIVETYPE_TRIANGLES, StencilMode::Off));
  for (const auto& piece : source.manager->pieces) {
    const auto& data = piece.tile->data;
    if (!data || !data->texture.valid()) continue;
    if (!t.overlaps(piece.tile->z, piece.tile->x, piece.tile->y, piece.wrap)) continue;
    sg_bindings b{};
    b.vertex_buffers[0] = Gpu::get().quad();
    b.views[VIEW_mk_raster_tex] = data->texture.view;
    const bool mip = data->texture.image.id != 0 && (data->width & (data->width - 1)) == 0;
    b.samplers[SMP_mk_raster_smp] = Gpu::get().sampler(!nearest, mip && !nearest);
    sg_apply_bindings(&b);
    mk_raster_vs_params_t vs{};
    copyMat(vs.u_matrix, t.tileMatrix(piece.tile->z, piece.tile->x, piece.tile->y, piece.wrap, 1));
    set4(vs.u_clip, piece.clip.x0, piece.clip.y0, piece.clip.x1, piece.clip.y1);
    sg_apply_uniforms(UB_mk_raster_vs_params, SG_RANGE(vs));
    set4(fs.u_adjust2, opacity * piece.opacity);
    sg_apply_uniforms(UB_mk_raster_fs_params, SG_RANGE(fs));
    sg_draw(0, 6, 1);
    stats_.drawCalls++;
    stats_.pieces++;
  }
}

void Painter::drawHillshade(const FrameInput& frame, const StyleLayer& layer, DemSource& source) {
  if (!source.manager || source.manager->pieces.empty()) return;
  const RenderView& t = *frame.view;
  const double zoom = t.zoom;
  EvalContext ctx;
  ctx.zoom = zoom;
  const double exaggeration = std::clamp(layer.paintNumber("exaggeration", ctx), 0.0, 1.0);
  if (exaggeration <= 0) return;
  constexpr double kAltitude = 45 * kDeg;
  const double anchor = layer.fixed("illuminationAnchor").stringOr("viewport") == "map" ? 0 : frame.transform->bearing();
  const double azimuth = (layer.fixed("illuminationDirection").numberOr(335) + anchor) * kDeg;
  mk_hillshade_params_t fs{};
  set4(fs.u_light, std::sin(azimuth) * std::cos(kAltitude), -std::cos(azimuth) * std::cos(kAltitude), std::sin(kAltitude), std::sin(kAltitude));
  const bool standard = layer.fixed("method").stringOr("") == "standard";
  setColor(fs.u_shadow, premultiply(layer.paintColor("shadowColor", ctx), 1));
  setColor(fs.u_highlight, premultiply(layer.paintColor("highlightColor", ctx), 1));
  setColor(fs.u_accent, premultiply(layer.paintColor("accentColor", ctx), 1));
  const double vertical = (0.5 + exaggeration) * std::exp2(0.3 * std::max(0.0, 12 - zoom));
  const int displayZ = static_cast<int>(std::lround(zoom + std::log2(512 / source.tileSize())));
  sg_apply_pipeline(pipeline(Program::Hillshade, SG_PRIMITIVETYPE_TRIANGLES, StencilMode::Off));
  Gpu& gpu = Gpu::get();
  for (const auto& piece : source.manager->pieces) {
    auto& data = piece.tile->data;
    if (!data) continue;
    const GpuTexture* gradient = source.gradientTexture(*data);
    if (!gradient) continue;
    if (!t.overlaps(piece.tile->z, piece.tile->x, piece.tile->y, piece.wrap)) continue;
    const int z = piece.tile->z;
    const double n = std::exp2(z);
    const double size = data->grid.size;
    sg_bindings b{};
    b.vertex_buffers[0] = gpu.quad();
    b.views[VIEW_mk_gradient_tex] = gradient->view;
    b.samplers[SMP_mk_gradient_smp] = gpu.sampler(true);
    b.views[VIEW_mk_mask_tex] = data->maskTexture.valid() ? data->maskTexture.view : gpu.whiteTexture().view;
    b.samplers[SMP_mk_mask_smp] = gpu.sampler(true);
    sg_apply_bindings(&b);
    mk_raster_vs_params_t vs{};
    copyMat(vs.u_matrix, t.tileMatrix(z, piece.tile->x, piece.tile->y, piece.wrap, 1));
    set4(vs.u_clip, piece.clip.x0, piece.clip.y0, piece.clip.x1, piece.clip.y1);
    sg_apply_uniforms(UB_mk_raster_vs_params, SG_RANGE(vs));
    set4(fs.u_texel_rows, size / (size + 2), 1 / (size + 2), piece.tile->y / n, (piece.tile->y + 1) / n);
    set4(fs.u_values, vertical * n * size / kEarthCircumference, 1.2 * exaggeration, piece.opacity, standard ? 1 : 0);
    const double sz = std::max(z, displayZ);
    const double factor = sz < 2 ? 0.4 : sz < 4.5 ? 0.35 : 0.3;
    const double boost = sz < 15 ? std::exp2((15 - sz) * factor) : 1;
    set4(fs.u_values2, size * std::exp2(sz) / kEarthCircumference * boost, azimuth, exaggeration);
    sg_apply_uniforms(UB_mk_hillshade_params, SG_RANGE(fs));
    sg_draw(0, 6, 1);
    stats_.drawCalls++;
    stats_.pieces++;
  }
}

Painter::SlotDem Painter::slotDem(const FrameInput& frame, size_t index) {
  SlotDem out;
  Gpu& gpu = Gpu::get();
  out.dem = flatDem_.view;
  out.gradient = gpu.blankTexture().view;
  Terrain* terrain = frame.terrain;
  if (!terrain || index >= terrain->slots.size()) return out;
  const TerrainSlot& slot = terrain->slots[index];
  if (!slot.dem || !slot.dem->data || !terrain->source) return out;
  DemTileData& data = *slot.dem->data;
  const GpuTexture* elevation = terrain->source->elevationTexture(data);
  if (!elevation || !elevation->valid()) return out;
  out.dem = elevation->view;
  if (const GpuTexture* g = terrain->source->gradientTexture(data)) out.gradient = g->view;
  out.rect = slot.rect;
  out.size = data.grid.size;
  out.z = data.z;
  out.range = data.grid.max - data.grid.min;
  out.has = true;
  return out;
}

void Painter::drawTerrain(const FrameInput& frame, const std::vector<CoveringTile>& slots) {
  Transform& t = *frame.transform;
  Terrain& terrain = *frame.terrain;
  Gpu& gpu = Gpu::get();
  const bool flip = gpu.backend() == SG_BACKEND_GLCORE || gpu.backend() == SG_BACKEND_GLES3;
  sg_apply_pipeline(pipeline(Program::Terrain, SG_PRIMITIVETYPE_TRIANGLES, StencilMode::Off, DepthMode::TestWrite));
  const double ppm = t.pixelsPerMeter() * terrain.exaggeration;
  const double relief = terrain.relief;
  const double azimuth = (315 + t.bearing()) * kDeg;
  for (size_t i = 0; i < slots.size(); i++) {
    const CoveringTile& slot = slots[i];
    auto [entry, rect] = slotDrape(slot);
    if (!entry) continue;
    const SlotDem dem = slotDem(frame, i);
    const double n = std::exp2(slot.z);
    sg_bindings b{};
    b.vertex_buffers[0] = meshVertices_.buffer;
    b.index_buffer = meshIndices_.buffer;
    b.views[VIEW_mk_drape_tex] = entry->texture;
    b.samplers[SMP_mk_drape_smp] = gpu.sampler(true);
    b.views[VIEW_mk_dem_tex] = dem.dem;
    b.samplers[SMP_mk_dem_smp] = gpu.sampler(false);
    b.views[VIEW_mk_gradient_tex] = dem.gradient;
    b.samplers[SMP_mk_gradient_smp] = gpu.sampler(true);
    sg_apply_bindings(&b);
    mk_terrain_vs_params_t vs{};
    copyMat(vs.u_matrix, t.tileMatrix(slot.z, slot.x, slot.y, slot.wrap, 1));
    set4(vs.u_dem_rect, dem.rect[0], dem.rect[1], dem.rect[2], dem.rect[3]);
    set4(vs.u_dem_info, dem.size, dem.has ? 1 : 0, terrain.exaggeration, std::max(30.0, dem.range * 0.2) * ppm);
    set4(vs.u_rows, slot.y / n, (slot.y + 1) / n, t.worldSize());
    sg_apply_uniforms(UB_mk_terrain_vs_params, SG_RANGE(vs));
    mk_terrain_fs_params_t fs{};
    set4(fs.u_drape_rect, rect[0], rect[1], rect[2], rect[3]);
    if (relief > 0) set4(fs.u_sun, std::sin(azimuth) * std::cos(kPi / 4), -std::cos(azimuth) * std::cos(kPi / 4), std::sin(kPi / 4), relief);
    const double cells = dem.size;
    set4(fs.u_grad, cells / (cells + 2), 1 / (cells + 2), dem.has ? std::exp2(dem.z) * cells / kEarthCircumference : 0, flip ? 1 : 0);
    set4(fs.u_rows2, slot.y / n, (slot.y + 1) / n, terrain.exaggeration, dem.has ? 1 : 0);
    set4(fs.u_dem_rect2, dem.rect[0], dem.rect[1], dem.rect[2], dem.rect[3]);
    sg_apply_uniforms(UB_mk_terrain_fs_params, SG_RANGE(fs));
    sg_draw(0, static_cast<int>(meshCount_), 1);
    stats_.drawCalls++;
    stats_.pieces++;
  }
}

bool Painter::clipped(const RenderPiece<VectorTileData>& piece) {
  const Rect& c = piece.clip;
  return !(c.x0 <= 0 && c.y0 <= 0 && c.x1 >= 1 && c.y1 >= 1);
}

void Painter::applyClip(const RenderPiece<VectorTileData>& piece, double extent) {
  if (!clipped(piece)) return;
  mk_clip_params_t c{};
  set4(c.u_clip, piece.clip.x0 * extent, piece.clip.y0 * extent, piece.clip.x1 * extent, piece.clip.y1 * extent);
  sg_apply_uniforms(UB_mk_clip_params, SG_RANGE(c));
}

void Painter::drawVector(const FrameInput& frame, const StyleLayer& layer, VectorSource& source) {
  if (!source.manager) return;
  const auto& pieces = source.manager->pieces;
  bool any = false;
  for (const auto& piece : pieces)
    if (piece.tile->data && piece.tile->data->buckets.contains(layer.id)) {
      any = true;
      break;
    }
  if (!any) return;
  const double margin = layer.type == LayerType::Circle ? 0.125 : 0;
  for (const auto& piece : pieces) {
    auto& data = piece.tile->data;
    if (!data || !data->uploaded) continue;
    if (!frame.view->overlaps(piece.tile->z, piece.tile->x, piece.tile->y, piece.wrap, margin)) continue;
    auto it = data->buckets.find(layer.id);
    if (it == data->buckets.end()) continue;
    VectorBucket& bucket = *it->second;
    switch (layer.type) {
      case LayerType::Fill: drawFill(frame, layer, bucket, piece); break;
      case LayerType::Line: drawLine(frame, layer, bucket, piece); break;
      case LayerType::Circle: drawCircle(frame, layer, bucket, piece); break;
      case LayerType::Extrusion: drawExtrusion(frame, layer, bucket, piece); break;
      default: break;
    }
    stats_.pieces++;
  }
}

void Painter::drawFill(const FrameInput& frame, const StyleLayer& layer, VectorBucket& bucket, const RenderPiece<VectorTileData>& piece) {
  if (!bucket.main) return;
  const RenderView& t = *frame.view;
  const double zoom = t.zoom;
  static const std::vector<std::string> names{"color", "opacity", "outlineColor"};
  bucket.paint.update(layer, bucket.features, names, zoom, frame.states, frame.states ? frame.states->version() : 0);
  EvalContext ctx;
  ctx.zoom = zoom;
  const Color color = layer.paintColor("color", ctx);
  const double opacity = layer.paintNumber("opacity", ctx);
  const auto translate = screenOffset(layer, t.bearing, zoom, "translate", "translateAnchor");
  const double units = bucket.extent / (512 * std::exp2(t.scaleZoom() - piece.tile->z));
  mk_fill_params_t p{};
  copyMat(p.u_matrix, t.tileMatrix(piece.tile->z, piece.tile->x, piece.tile->y, piece.wrap, bucket.extent));
  const PaintSlot cs = bucket.paint.slot("color"), os = bucket.paint.slot("opacity");
  setColor(p.u_color, color);
  set4(p.u_slots, cs.slot, cs.composite, os.slot, os.composite);
  set4(p.u_misc, opacity * piece.opacity, translate[0] * units, translate[1] * units);
  set4(p.u_paint, bucket.paint.width(), bucket.paint.stride(), zoom - std::floor(zoom));
  sg_bindings b{};
  b.vertex_buffers[0] = bucket.main->vertices.buffer;
  b.index_buffer = bucket.main->indices.buffer;
  bindPaint(b, bucket.paint);
  if (!layer.paintValue("pattern", ctx).stringOr("").empty()) return;
  const bool antialias = layer.paintValue("antialias", ctx).boolOr(true);
  const bool hasOutline = layer.spec["paint"].has("outlineColor");
  if (color[3] > 0 || bucket.paint.slot("color").slot >= 0) {
    sg_apply_pipeline(pipeline(clipped(piece) ? Program::FillClip : Program::Fill));
    sg_apply_bindings(&b);
    sg_apply_uniforms(UB_mk_fill_params, SG_RANGE(p));
    applyClip(piece, bucket.extent);
    sg_draw(0, static_cast<int>(bucket.main->indexCount), 1);
    stats_.drawCalls++;
  }
  if (bucket.outline && (hasOutline || antialias)) {
    const PaintSlot ol = bucket.paint.slot("outlineColor");
    if (hasOutline) {
      setColor(p.u_color, layer.paintColor("outlineColor", ctx));
      set4(p.u_slots, ol.slot, ol.composite, os.slot, os.composite);
    }
    b.index_buffer = bucket.outline->indices.buffer;
    sg_apply_pipeline(pipeline(clipped(piece) ? Program::FillClip : Program::Fill, SG_PRIMITIVETYPE_LINES));
    sg_apply_bindings(&b);
    sg_apply_uniforms(UB_mk_fill_params, SG_RANGE(p));
    applyClip(piece, bucket.extent);
    sg_draw(0, static_cast<int>(bucket.outline->indexCount), 1);
    stats_.drawCalls++;
  }
}

void Painter::drawLine(const FrameInput& frame, const StyleLayer& layer, VectorBucket& bucket, const RenderPiece<VectorTileData>& piece) {
  if (!bucket.main) return;
  const RenderView& t = *frame.view;
  const double zoom = t.zoom;
  static const std::vector<std::string> names{"color", "width", "opacity", "blur", "offset", "gapWidth"};
  bucket.paint.update(layer, bucket.features, names, zoom, frame.states, frame.states ? frame.states->version() : 0);
  EvalContext ctx;
  ctx.zoom = zoom;
  const double units = bucket.extent / (512 * std::exp2(t.scaleZoom() - piece.tile->z));
  mk_line_params_t p{};
  copyMat(p.u_matrix, t.tileMatrix(piece.tile->z, piece.tile->x, piece.tile->y, piece.wrap, bucket.extent));
  setColor(p.u_color, layer.paintColor("color", ctx));
  set4(p.u_values, layer.paintNumber("opacity", ctx) * piece.opacity, layer.paintNumber("width", ctx), layer.paintNumber("blur", ctx),
       layer.paintNumber("offset", ctx));
  set4(p.u_values2, layer.paintNumber("gapWidth", ctx), 0, 0);
  const PaintSlot c = bucket.paint.slot("color"), o = bucket.paint.slot("opacity"), w = bucket.paint.slot("width"), bl = bucket.paint.slot("blur"),
                  of = bucket.paint.slot("offset"), g = bucket.paint.slot("gapWidth");
  set4(p.u_slots, c.slot, c.composite, o.slot, o.composite);
  set4(p.u_slots2, w.slot, w.composite, bl.slot, bl.composite);
  set4(p.u_slots3, of.slot, of.composite, g.slot, g.composite);
  set4(p.u_misc, units, 0, frame.pixelRatio, 0);
  set4(p.u_paint, bucket.paint.width(), bucket.paint.stride(), zoom - std::floor(zoom));
  mk_line_dash_t dash{};
  const Value raw = layer.paintValue("dash", ctx);
  std::vector<double> pattern;
  if (raw.isArray()) {
    for (const auto& v : raw.asArray()) {
      if (!v.isNumber() || v.asNumber() < 0) {
        pattern.clear();
        break;
      }
      pattern.push_back(v.asNumber());
    }
  }
  if (!pattern.empty()) {
    if (pattern.size() % 2 == 1) {
      const auto copy = pattern;
      pattern.insert(pattern.end(), copy.begin(), copy.end());
    }
    const size_t n = std::min<size_t>(8, pattern.size());
    double total = 0;
    for (size_t i = 0; i < n; i++) {
      dash.u_dash[i / 4][i % 4] = static_cast<float>(pattern[i]);
      total += pattern[i];
    }
    set4(dash.u_dash_info, total > 0 ? static_cast<double>(n) : 0, total);
  }
  sg_bindings b{};
  b.vertex_buffers[0] = bucket.main->vertices.buffer;
  b.index_buffer = bucket.main->indices.buffer;
  bindPaint(b, bucket.paint);
  sg_apply_pipeline(pipeline(clipped(piece) ? Program::LineClip : Program::Line));
  sg_apply_bindings(&b);
  sg_apply_uniforms(UB_mk_line_params, SG_RANGE(p));
  sg_apply_uniforms(UB_mk_line_dash, SG_RANGE(dash));
  applyClip(piece, bucket.extent);
  sg_draw(0, static_cast<int>(bucket.main->indexCount), 1);
  stats_.drawCalls++;
}

void Painter::drawCircle(const FrameInput& frame, const StyleLayer& layer, VectorBucket& bucket, const RenderPiece<VectorTileData>& piece) {
  if (!bucket.main) return;
  const RenderView& t = *frame.view;
  const double zoom = t.zoom;
  static const std::vector<std::string> names{"radius", "color", "opacity", "strokeColor", "strokeWidth", "strokeOpacity", "blur"};
  bucket.paint.update(layer, bucket.features, names, zoom, frame.states, frame.states ? frame.states->version() : 0);
  EvalContext ctx;
  ctx.zoom = zoom;
  const double units = bucket.extent / (512 * std::exp2(t.scaleZoom() - piece.tile->z));
  const auto translate = screenOffset(layer, t.bearing, zoom, "translate", "translateAnchor");
  mk_circle_params_t p{};
  copyMat(p.u_matrix, t.tileMatrix(piece.tile->z, piece.tile->x, piece.tile->y, piece.wrap, bucket.extent));
  setColor(p.u_color, layer.paintColor("color", ctx));
  setColor(p.u_stroke_color, layer.paintColor("strokeColor", ctx));
  set4(p.u_values, layer.paintNumber("opacity", ctx) * piece.opacity, layer.paintNumber("radius", ctx), layer.paintNumber("strokeWidth", ctx),
       layer.paintNumber("blur", ctx));
  set4(p.u_values2, layer.paintNumber("strokeOpacity", ctx) * piece.opacity, translate[0] * units, translate[1] * units);
  const PaintSlot c = bucket.paint.slot("color"), s = bucket.paint.slot("strokeColor"), o = bucket.paint.slot("opacity"),
                  r = bucket.paint.slot("radius"), sw = bucket.paint.slot("strokeWidth"), bl = bucket.paint.slot("blur"),
                  so = bucket.paint.slot("strokeOpacity");
  set4(p.u_slots, c.slot, c.composite, s.slot, s.composite);
  set4(p.u_slots2, o.slot, o.composite, r.slot, r.composite);
  set4(p.u_slots3, sw.slot, sw.composite, bl.slot, bl.composite);
  set4(p.u_slots4, so.slot, so.composite);
  set4(p.u_misc, t.width, t.height, frame.pixelRatio, units);
  set4(p.u_misc2, layer.fixed("pitchAlignment").stringOr("viewport") == "map" ? 1 : 0, layer.fixed("pitchScale").stringOr("viewport") == "map" ? 1 : 0,
       frame.view->region ? 1.0 : frame.transform->cameraToCenterDistance());
  set4(p.u_paint, bucket.paint.width(), bucket.paint.stride(), zoom - std::floor(zoom));
  sg_bindings b{};
  b.vertex_buffers[0] = bucket.main->vertices.buffer;
  b.index_buffer = bucket.main->indices.buffer;
  bindPaint(b, bucket.paint);
  sg_apply_pipeline(pipeline(clipped(piece) ? Program::CircleClip : Program::Circle));
  sg_apply_bindings(&b);
  sg_apply_uniforms(UB_mk_circle_params, SG_RANGE(p));
  applyClip(piece, bucket.extent);
  sg_draw(0, static_cast<int>(bucket.main->indexCount), 1);
  stats_.drawCalls++;
}

void Painter::drawExtrusion(const FrameInput& frame, const StyleLayer& layer, VectorBucket& bucket, const RenderPiece<VectorTileData>& piece) {
  if (!bucket.main) return;
  Transform& t = *frame.transform;
  const double zoom = t.zoom();
  EvalContext ctx;
  ctx.zoom = zoom;
  const double opacity = layer.paintNumber("opacity", ctx);
  if (!(opacity > 0)) return;
  static const std::vector<std::string> names{"color", "height", "base"};
  bucket.paint.update(layer, bucket.features, names, zoom, frame.states, frame.states ? frame.states->version() : 0);
  const double b = t.bearing() * kDeg;
  mk_extrusion_params_t p{};
  copyMat(p.u_matrix, t.tileMatrix(piece.tile->z, piece.tile->x, piece.tile->y, piece.wrap, bucket.extent));
  setColor(p.u_color, layer.paintColor("color", ctx));
  const double lat = latFromMercatorY((piece.tile->y + 0.5) / std::exp2(piece.tile->z));
  set4(p.u_values, layer.paintNumber("height", ctx), layer.paintNumber("base", ctx), std::min(1.0, opacity), 1 / metersPerPixel(lat, zoom));
  const PaintSlot c = bucket.paint.slot("color"), h = bucket.paint.slot("height"), bs = bucket.paint.slot("base");
  set4(p.u_slots, c.slot, c.composite, h.slot, h.composite);
  set4(p.u_slots2, bs.slot, bs.composite);
  set4(p.u_light, kLight[0] * std::cos(b) - kLight[1] * std::sin(b), kLight[0] * std::sin(b) + kLight[1] * std::cos(b), kLight[2]);
  set4(p.u_paint, bucket.paint.width(), bucket.paint.stride(), zoom - std::floor(zoom));
  sg_bindings bind{};
  bind.vertex_buffers[0] = bucket.main->vertices.buffer;
  bind.index_buffer = bucket.main->indices.buffer;
  bindPaint(bind, bucket.paint);
  mk_extrusion_dem_t dem{};
  bind.views[VIEW_mk_dem_tex] = flatDem_.view;
  bind.samplers[SMP_mk_dem_smp] = Gpu::get().sampler(false);
  if (frame.terrain && frame.terrain->source && t.globeAmount() <= 0) {
    auto [tile, rect] = frame.terrain->demFor(piece.tile->z, piece.tile->x, piece.tile->y);
    if (tile && tile->data)
      if (const GpuTexture* e = frame.terrain->source->elevationTexture(*tile->data); e && e->valid()) {
        bind.views[VIEW_mk_dem_tex] = e->view;
        const double n = std::exp2(piece.tile->z);
        set4(dem.u_dem_rect, rect[0], rect[1], rect[2], rect[3]);
        set4(dem.u_dem_info, tile->data->grid.size, 1, frame.terrain->exaggeration, bucket.extent);
        set4(dem.u_rows, piece.tile->y / n, (piece.tile->y + 1) / n, t.worldSize());
      }
  }
  sg_apply_pipeline(pipeline(clipped(piece) ? Program::ExtrusionClip : Program::Extrusion, SG_PRIMITIVETYPE_TRIANGLES, StencilMode::Off,
                             DepthMode::TestWrite));
  sg_apply_bindings(&bind);
  sg_apply_uniforms(UB_mk_extrusion_params, SG_RANGE(p));
  sg_apply_uniforms(UB_mk_extrusion_dem, SG_RANGE(dem));
  applyClip(piece, bucket.extent);
  sg_draw(0, static_cast<int>(bucket.main->indexCount), 1);
  stats_.drawCalls++;
}

}
