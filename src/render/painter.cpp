#include "render/painter.hpp"

#include <chrono>
#include <cmath>
#include <cstring>

#include "geo/mercator.hpp"
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

std::array<float, 16> clipQuadMatrix(Transform& t, const Tile<VectorTileData>& tile, int wrap, const Rect& clip) {
  mat4::Mat4 m = t.tileMatrixDouble(tile.z, tile.x, tile.y, wrap, 1);
  mat4::translate(m, clip.x0, clip.y0, 0);
  mat4::scale(m, clip.x1 - clip.x0, clip.y1 - clip.y0, 1);
  return mat4::toFloat(m);
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

bool Painter::render(const FrameInput& frame, const FrameTarget& target) {
  const auto started = std::chrono::steady_clock::now();
  stats_ = {};
  format_ = {target.swapchain.color_format, target.swapchain.depth_format, target.swapchain.sample_count};
  maskSource_.clear();
  maskRefs_.clear();
  maskOffset_ = 0;
  sg_pass pass{};
  pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
  pass.action.colors[0].clear_value = {static_cast<float>(frame.clearColor[0]), static_cast<float>(frame.clearColor[1]),
                                       static_cast<float>(frame.clearColor[2]), static_cast<float>(frame.clearColor[3])};
  pass.action.depth.load_action = SG_LOADACTION_CLEAR;
  pass.action.depth.clear_value = 1.0f;
  pass.action.stencil.load_action = SG_LOADACTION_CLEAR;
  pass.action.stencil.clear_value = 0;
  pass.swapchain = target.swapchain;
  sg_begin_pass(&pass);
  const double zoom = frame.transform->zoom();
  for (const auto& layer : *frame.layers) {
    if (!layer->isVisible(zoom)) continue;
    drawLayer(frame, *layer);
    stats_.layers++;
  }
  sg_end_pass();
  sg_commit();
  stats_.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return true;
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
  if (auto* vector = dynamic_cast<VectorSource*>(source)) drawVector(frame, layer, *vector);
}

void Painter::drawBackground(const FrameInput& frame, const StyleLayer& layer) {
  EvalContext ctx;
  ctx.zoom = frame.transform->zoom();
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
  Transform& t = *frame.transform;
  EvalContext ctx;
  ctx.zoom = t.zoom();
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

void Painter::drawMasks(const FrameInput& frame, const std::string& sourceId, const std::vector<RenderPiece<VectorTileData>>& pieces) {
  if (maskSource_ == sourceId && !maskRefs_.empty()) return;
  maskSource_ = sourceId;
  maskRefs_.clear();
  Transform& t = *frame.transform;
  sg_bindings b{};
  b.vertex_buffers[0] = Gpu::get().quad();
  sg_apply_pipeline(pipeline(Program::Solid, SG_PRIMITIVETYPE_TRIANGLES, StencilMode::Write, DepthMode::Off, false));
  sg_apply_bindings(&b);
  sg_apply_stencil_ref(0);
  mk_solid_params_t clear{};
  const float m[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1};
  std::memcpy(clear.u_matrix, m, sizeof m);
  sg_apply_uniforms(UB_mk_solid_params, SG_RANGE(clear));
  sg_draw(0, 6, 1);
  int ref = 1;
  for (const auto& piece : pieces) {
    if (ref > 255) break;
    sg_apply_stencil_ref(static_cast<uint8_t>(ref));
    mk_solid_params_t p{};
    copyMat(p.u_matrix, clipQuadMatrix(t, *piece.tile, piece.wrap, piece.clip));
    sg_apply_uniforms(UB_mk_solid_params, SG_RANGE(p));
    sg_draw(0, 6, 1);
    maskRefs_[&piece] = ref++;
    stats_.drawCalls++;
  }
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
  drawMasks(frame, source.id, pieces);
  for (const auto& piece : pieces) {
    auto& data = piece.tile->data;
    if (!data || !data->uploaded) continue;
    auto it = data->buckets.find(layer.id);
    if (it == data->buckets.end()) continue;
    VectorBucket& bucket = *it->second;
    auto ref = maskRefs_.find(&piece);
    sg_apply_stencil_ref(ref == maskRefs_.end() ? 0 : static_cast<uint8_t>(ref->second));
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
  Transform& t = *frame.transform;
  const double zoom = t.zoom();
  static const std::vector<std::string> names{"color", "opacity", "outlineColor"};
  bucket.paint.update(layer, bucket.features, names, zoom, frame.states, frame.states ? frame.states->version() : 0);
  EvalContext ctx;
  ctx.zoom = zoom;
  const Color color = layer.paintColor("color", ctx);
  const double opacity = layer.paintNumber("opacity", ctx);
  const auto translate = screenOffset(layer, t.bearing(), zoom, "translate", "translateAnchor");
  const double units = bucket.extent / (512 * std::exp2(zoom - piece.tile->z));
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
  const bool antialias = layer.paintValue("antialias", ctx).boolOr(true);
  const bool hasOutline = layer.spec["paint"].has("outlineColor");
  if (color[3] > 0 || bucket.paint.slot("color").slot >= 0) {
    sg_apply_pipeline(pipeline(Program::Fill));
    sg_apply_bindings(&b);
    sg_apply_uniforms(UB_mk_fill_params, SG_RANGE(p));
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
    sg_apply_pipeline(pipeline(Program::Fill, SG_PRIMITIVETYPE_LINES));
    sg_apply_bindings(&b);
    sg_apply_uniforms(UB_mk_fill_params, SG_RANGE(p));
    sg_draw(0, static_cast<int>(bucket.outline->indexCount), 1);
    stats_.drawCalls++;
  }
}

void Painter::drawLine(const FrameInput& frame, const StyleLayer& layer, VectorBucket& bucket, const RenderPiece<VectorTileData>& piece) {
  if (!bucket.main) return;
  Transform& t = *frame.transform;
  const double zoom = t.zoom();
  static const std::vector<std::string> names{"color", "width", "opacity", "blur", "offset", "gapWidth"};
  bucket.paint.update(layer, bucket.features, names, zoom, frame.states, frame.states ? frame.states->version() : 0);
  EvalContext ctx;
  ctx.zoom = zoom;
  const double units = bucket.extent / (512 * std::exp2(zoom - piece.tile->z));
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
  sg_apply_pipeline(pipeline(Program::Line));
  sg_apply_bindings(&b);
  sg_apply_uniforms(UB_mk_line_params, SG_RANGE(p));
  sg_apply_uniforms(UB_mk_line_dash, SG_RANGE(dash));
  sg_draw(0, static_cast<int>(bucket.main->indexCount), 1);
  stats_.drawCalls++;
}

void Painter::drawCircle(const FrameInput& frame, const StyleLayer& layer, VectorBucket& bucket, const RenderPiece<VectorTileData>& piece) {
  if (!bucket.main) return;
  Transform& t = *frame.transform;
  const double zoom = t.zoom();
  static const std::vector<std::string> names{"radius", "color", "opacity", "strokeColor", "strokeWidth", "strokeOpacity", "blur"};
  bucket.paint.update(layer, bucket.features, names, zoom, frame.states, frame.states ? frame.states->version() : 0);
  EvalContext ctx;
  ctx.zoom = zoom;
  const double units = bucket.extent / (512 * std::exp2(zoom - piece.tile->z));
  const auto translate = screenOffset(layer, t.bearing(), zoom, "translate", "translateAnchor");
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
       t.cameraToCenterDistance());
  set4(p.u_paint, bucket.paint.width(), bucket.paint.stride(), zoom - std::floor(zoom));
  sg_bindings b{};
  b.vertex_buffers[0] = bucket.main->vertices.buffer;
  b.index_buffer = bucket.main->indices.buffer;
  bindPaint(b, bucket.paint);
  sg_apply_pipeline(pipeline(Program::Circle));
  sg_apply_bindings(&b);
  sg_apply_uniforms(UB_mk_circle_params, SG_RANGE(p));
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
  sg_apply_pipeline(pipeline(Program::Extrusion, SG_PRIMITIVETYPE_TRIANGLES, StencilMode::Equal, DepthMode::TestWrite));
  sg_apply_bindings(&bind);
  sg_apply_uniforms(UB_mk_extrusion_params, SG_RANGE(p));
  sg_draw(0, static_cast<int>(bucket.main->indexCount), 1);
  stats_.drawCalls++;
}

}
