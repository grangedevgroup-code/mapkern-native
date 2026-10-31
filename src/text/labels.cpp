#include "text/labels.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/utf8.hpp"
#include "render/shaders/map.glsl.h"
#include "style/color.hpp"
#include "style/import.hpp"

namespace mapkern {

namespace {

constexpr double kFadeMs = 180;
constexpr double kPlaceInterval = 120;
constexpr size_t kStride = 40;
constexpr double kDeg = kPi / 180;

struct Shift {
  double x, y;
};

Shift anchorShift(const std::string& a) {
  if (a == "top") return {-0.5, 0};
  if (a == "bottom") return {-0.5, -1};
  if (a == "left") return {0, -0.5};
  if (a == "right") return {-1, -0.5};
  if (a == "top-left") return {0, 0};
  if (a == "top-right") return {-1, 0};
  if (a == "bottom-left") return {0, -1};
  if (a == "bottom-right") return {-1, -1};
  return {-0.5, -0.5};
}

bool isAnchor(const std::string& a) {
  static const char* all[] = {"center", "top", "bottom", "left", "right", "top-left", "top-right", "bottom-left", "bottom-right"};
  for (const char* x : all)
    if (a == x) return true;
  return false;
}

Shift radialOffset(double n, const std::string& a) {
  const double d = n / std::sqrt(2.0);
  if (a == "top") return {0, n};
  if (a == "bottom") return {0, -n};
  if (a == "left") return {n, 0};
  if (a == "right") return {-n, 0};
  if (a == "top-left") return {d, d};
  if (a == "top-right") return {-d, d};
  if (a == "bottom-left") return {d, -d};
  if (a == "bottom-right") return {-d, -d};
  return {0, 0};
}

double numberOr(const Value& v, double fallback) { return v.isNumber() && std::isfinite(v.asNumber()) ? v.asNumber() : fallback; }

std::array<double, 2> pairOr(const Value& v) {
  if (v.isArray() && v.size() == 2 && v[0].isNumber() && v[1].isNumber()) return {v[0].asNumber(), v[1].asNumber()};
  return {0, 0};
}

std::array<double, 2> screenShift(std::array<double, 2> t, const std::string& anchor, double bearing) {
  if ((t[0] == 0 && t[1] == 0) || anchor == "viewport" || bearing == 0) return t;
  const double b = -bearing * kDeg;
  return {t[0] * std::cos(b) - t[1] * std::sin(b), t[0] * std::sin(b) + t[1] * std::cos(b)};
}

Color withOpacity(const Color& c, double opacity) { return {c[0], c[1], c[2], c[3] * std::clamp(opacity, 0.0, 1.0)}; }

class CollisionGrid {
 public:
  CollisionGrid(double width, double height)
      : cols_(std::max(1, static_cast<int>(std::ceil(width / kCell)))), rows_(std::max(1, static_cast<int>(std::ceil(height / kCell)))),
        cells_(static_cast<size_t>(cols_) * rows_) {}
  bool hits(double x0, double y0, double x1, double y1) const {
    const int c0 = std::max(0, static_cast<int>(std::floor(x0 / kCell))), c1 = std::min(cols_ - 1, static_cast<int>(std::floor(x1 / kCell)));
    const int r0 = std::max(0, static_cast<int>(std::floor(y0 / kCell))), r1 = std::min(rows_ - 1, static_cast<int>(std::floor(y1 / kCell)));
    for (int r = r0; r <= r1; r++)
      for (int c = c0; c <= c1; c++)
        for (size_t i : cells_[static_cast<size_t>(r) * cols_ + c])
          if (x0 < boxes_[i + 2] && x1 > boxes_[i] && y0 < boxes_[i + 3] && y1 > boxes_[i + 1]) return true;
    return false;
  }
  void insert(double x0, double y0, double x1, double y1) {
    const size_t i = boxes_.size();
    boxes_.insert(boxes_.end(), {x0, y0, x1, y1});
    const int c0 = std::max(0, static_cast<int>(std::floor(x0 / kCell))), c1 = std::min(cols_ - 1, static_cast<int>(std::floor(x1 / kCell)));
    const int r0 = std::max(0, static_cast<int>(std::floor(y0 / kCell))), r1 = std::min(rows_ - 1, static_cast<int>(std::floor(y1 / kCell)));
    for (int r = r0; r <= r1; r++)
      for (int c = c0; c <= c1; c++) cells_[static_cast<size_t>(r) * cols_ + c].push_back(i);
  }
  bool hitsAny(const std::vector<double>& b) const {
    for (size_t i = 0; i + 3 < b.size(); i += 4)
      if (hits(b[i], b[i + 1], b[i + 2], b[i + 3])) return true;
    return false;
  }

 private:
  static constexpr double kCell = 32;
  int cols_, rows_;
  std::vector<std::vector<size_t>> cells_;
  std::vector<double> boxes_;
};

struct PathGlyph {
  const LaidGlyph* glyph;
  double cx, cy, angle, advance;
};

std::optional<std::vector<PathGlyph>> layoutAlongPath(Transform& t, const std::vector<double>& path, double anchorDist, const Laid& laid,
                                                      double scale, double maxAngle = kPi / 4, bool keepUpright = true) {
  const size_t n = path.size() / 2;
  if (n < 2) return std::nullopt;
  double target = anchorDist;
  size_t anchorSeg = 0;
  double anchorT = 0;
  for (size_t i = 0; i + 1 < n; i++) {
    const double len = std::hypot(path[i * 2 + 2] - path[i * 2], path[i * 2 + 3] - path[i * 2 + 1]);
    if (target <= len || i == n - 2) {
      anchorSeg = i;
      anchorT = len > 0 ? std::min(1.0, target / len) : 0;
      break;
    }
    target -= len;
  }
  const double width = laid.width * scale;
  auto project = [&](size_t i) -> std::optional<std::array<double, 2>> {
    const auto p = t.project(path[i * 2], path[i * 2 + 1], 0);
    if (p.w <= 0) return std::nullopt;
    return std::array<double, 2>{p.x, p.y};
  };
  const auto first = project(anchorSeg);
  const auto second = project(anchorSeg + 1);
  if (!first || !second) return std::nullopt;
  std::vector<std::array<double, 2>> before{*first}, after{*second};
  const double span = std::hypot((*second)[0] - (*first)[0], (*second)[1] - (*first)[1]);
  double reach = span * anchorT;
  for (int64_t i = static_cast<int64_t>(anchorSeg) - 1; i >= 0 && reach < width; i--) {
    const auto p = project(static_cast<size_t>(i));
    if (!p) break;
    const auto& q = before.back();
    reach += std::hypot((*p)[0] - q[0], (*p)[1] - q[1]);
    before.push_back(*p);
  }
  reach = span * (1 - anchorT);
  for (size_t i = anchorSeg + 2; i < n && reach < width; i++) {
    const auto p = project(i);
    if (!p) break;
    const auto& q = after.back();
    reach += std::hypot((*p)[0] - q[0], (*p)[1] - q[1]);
    after.push_back(*p);
  }
  std::vector<double> screen;
  for (auto it = before.rbegin(); it != before.rend(); ++it) screen.insert(screen.end(), {(*it)[0], (*it)[1]});
  for (const auto& p : after) screen.insert(screen.end(), {p[0], p[1]});
  const size_t m = screen.size() / 2;
  const size_t local = before.size() - 1;
  std::vector<double> cum{0};
  for (size_t i = 1; i < m; i++) cum.push_back(cum[i - 1] + std::hypot(screen[i * 2] - screen[i * 2 - 2], screen[i * 2 + 1] - screen[i * 2 - 1]));
  const double total = cum[m - 1];
  const double anchorAt = cum[local] + (cum[local + 1] - cum[local]) * anchorT;
  if (width > total) return std::nullopt;
  double start = std::max(0.0, anchorAt - width / 2);
  if (start + width > total) start = total - width;
  auto pointAt = [&](double d) {
    size_t i = 0;
    while (i < m - 2 && cum[i + 1] < d) i++;
    const double seg = cum[i + 1] - cum[i];
    const double tt = seg > 0 ? (d - cum[i]) / seg : 0;
    const double ax = screen[i * 2], ay = screen[i * 2 + 1], bx = screen[i * 2 + 2], by = screen[i * 2 + 3];
    return std::array<double, 3>{ax + (bx - ax) * tt, ay + (by - ay) * tt, std::atan2(by - ay, bx - ax)};
  };
  const auto mid = pointAt(start + width / 2);
  const bool flip = keepUpright && std::cos(mid[2]) < 0;
  std::vector<PathGlyph> out;
  double lastAngle = NAN;
  for (const auto& g : laid.glyphs) {
    const double along = (g.x + g.m.advance / 2) * scale;
    const double d = flip ? start + width - along : start + along;
    const auto p = pointAt(d);
    const double angle = flip ? p[2] + kPi : p[2];
    if (!std::isnan(lastAngle)) {
      double diff = std::abs(angle - lastAngle);
      if (diff > kPi) diff = 2 * kPi - diff;
      if (diff > maxAngle) return std::nullopt;
    }
    lastAngle = angle;
    out.push_back({&g, p[0], p[1], angle, g.m.advance});
  }
  return out;
}

template <class Emit>
bool layoutOnGround(Transform& t, const std::vector<double>& path, double anchorDist, const Laid& laid, double size, double maxAngle, bool keepUpright,
                    Emit&& emit) {
  const size_t n = path.size() / 2;
  if (n < 2) return false;
  const double ws = t.worldSize();
  const double unit = size / kGlyphSize / ws;
  const double width = laid.width * unit;
  std::vector<double> cum(n, 0);
  for (size_t i = 1; i < n; i++) cum[i] = cum[i - 1] + std::hypot(path[i * 2] - path[i * 2 - 2], path[i * 2 + 1] - path[i * 2 - 1]);
  const double total = cum[n - 1];
  if (width > total) return false;
  double start = std::max(0.0, anchorDist - width / 2);
  if (start + width > total) start = total - width;
  size_t seg = 0;
  auto locate = [&](double d) {
    while (seg < n - 2 && cum[seg + 1] < d) seg++;
    while (seg > 0 && cum[seg] > d) seg--;
  };
  locate(start + width / 2);
  const double mx = path[seg * 2], my = path[seg * 2 + 1];
  const double ma = std::atan2(path[seg * 2 + 3] - my, path[seg * 2 + 2] - mx);
  double gp[2];
  if (t.projectTo(gp, mx, my, 0) <= 0) return false;
  const double x0s = gp[0];
  if (t.projectTo(gp, mx + std::cos(ma) * unit * 8, my + std::sin(ma) * unit * 8, 0) <= 0) return false;
  const bool flip = keepUpright && gp[0] < x0s;
  std::vector<double> placed(laid.glyphs.size() * 3);
  double lastAngle = NAN;
  for (size_t k = 0; k < laid.glyphs.size(); k++) {
    const auto& g = laid.glyphs[k];
    const double along = (g.x + g.m.advance / 2) * unit;
    const double d = flip ? start + width - along : start + along;
    locate(d);
    const double segLength = cum[seg + 1] - cum[seg];
    const double tt = segLength > 0 ? (d - cum[seg]) / segLength : 0;
    const double ax = path[seg * 2], ay = path[seg * 2 + 1], bx = path[seg * 2 + 2], by = path[seg * 2 + 3];
    const double a = std::atan2(by - ay, bx - ax);
    const double angle = flip ? a + kPi : a;
    if (!std::isnan(lastAngle)) {
      double diff = std::abs(angle - lastAngle);
      if (diff > kPi) diff = 2 * kPi - diff;
      if (diff > maxAngle) return false;
    }
    lastAngle = angle;
    placed[k * 3] = ax + (bx - ax) * tt;
    placed[k * 3 + 1] = ay + (by - ay) * tt;
    placed[k * 3 + 2] = angle;
  }
  double frame[6];
  double corners[8];
  for (size_t k = 0; k < laid.glyphs.size(); k++) {
    const auto& g = laid.glyphs[k];
    const auto& m = g.m;
    const double cs = std::cos(placed[k * 3 + 2]), sn = std::sin(placed[k * 3 + 2]);
    if (t.groundFrame(placed[k * 3], placed[k * 3 + 1], 0, frame) <= 0) return false;
    const double x0 = (m.left - m.advance / 2) * unit, x1 = x0 + m.width * unit;
    const double y0 = (m.top + kGlyphSize * 0.32) * unit, y1 = y0 + m.height * unit;
    for (int c = 0; c < 4; c++) {
      const double dx = c == 0 || c == 3 ? x0 : x1;
      const double dy = c < 2 ? y0 : y1;
      const double ox = dx * cs - dy * sn, oy = dx * sn + dy * cs;
      corners[c * 2] = frame[0] + frame[2] * ox + frame[3] * oy;
      corners[c * 2 + 1] = frame[1] + frame[4] * ox + frame[5] * oy;
    }
    const double tall = std::hypot(corners[6] - corners[0], corners[7] - corners[1]);
    emit(g, corners, m.height > 0 ? tall / m.height : size / kGlyphSize);
  }
  return true;
}

void putVertex(std::vector<float>& out, double x, double y, double u, double v, const Color& c, const Color& h, double opacity, double scale,
               double haloWidth, double param) {
  const size_t at = out.size();
  out.resize(at + kStride / 4);
  float* f = &out[at];
  f[0] = static_cast<float>(x);
  f[1] = static_cast<float>(y);
  f[2] = static_cast<float>(u);
  f[3] = static_cast<float>(v);
  uint8_t* b = reinterpret_cast<uint8_t*>(&f[4]);
  for (int i = 0; i < 3; i++) b[i] = static_cast<uint8_t>(std::clamp(c[i] * c[3] * 255, 0.0, 255.0));
  b[3] = static_cast<uint8_t>(std::clamp(c[3] * 255, 0.0, 255.0));
  for (int i = 0; i < 3; i++) b[4 + i] = static_cast<uint8_t>(std::clamp(h[i] * h[3] * 255, 0.0, 255.0));
  b[7] = static_cast<uint8_t>(std::clamp(h[3] * 255, 0.0, 255.0));
  f[6] = static_cast<float>(opacity);
  f[7] = static_cast<float>(scale);
  f[8] = static_cast<float>(haloWidth);
  f[9] = static_cast<float>(param);
}

}

ImageAtlas::ImageAtlas() { data.assign(static_cast<size_t>(width) * height * 4, 0); }

const StyleImage* ImageAtlas::get(const std::string& id) const {
  auto it = images_.find(id);
  return it == images_.end() ? nullptr : &it->second;
}

bool ImageAtlas::place(StyleImage& image) {
  const int w = image.width + 2, h = image.height + 2;
  for (auto& s : shelves_)
    if (h <= s.height && s.x + w <= width) {
      image.x = s.x + 1;
      image.y = s.y + 1;
      s.x += w;
      return true;
    }
  while (bottom_ + h > height || w > width) {
    if (height >= 8192) return false;
    const int nw = w > width ? std::max(width * 2, w) : width;
    std::vector<uint8_t> next(static_cast<size_t>(nw) * height * 2 * 4, 0);
    for (int y = 0; y < height; y++) std::memcpy(&next[static_cast<size_t>(y) * nw * 4], &data[static_cast<size_t>(y) * width * 4], static_cast<size_t>(width) * 4);
    data = std::move(next);
    width = nw;
    height *= 2;
    generation++;
  }
  shelves_.push_back({bottom_, h, w});
  image.x = 1;
  image.y = bottom_ + 1;
  bottom_ += h;
  return true;
}

void ImageAtlas::add(const std::string& id, StyleImage image) {
  if (image.width <= 0 || image.height <= 0) return;
  if (!place(image)) return;
  for (int y = 0; y < image.height; y++)
    std::memcpy(&data[(static_cast<size_t>(image.y + y) * width + image.x) * 4], &image.rgba[static_cast<size_t>(y) * image.width * 4],
                static_cast<size_t>(image.width) * 4);
  image.rgba.clear();
  images_[id] = std::move(image);
  dirty = true;
  version++;
}

void ImageAtlas::clear() {
  images_.clear();
  shelves_.clear();
  bottom_ = 0;
  width = height = 256;
  data.assign(static_cast<size_t>(width) * height * 4, 0);
  dirty = true;
  generation++;
  version++;
}

void LabelRenderer::setGlyphs(const std::string& tmpl, std::shared_ptr<ResponseStore> store, Mailbox* mailbox, std::function<void()> arrived) {
  if (server_ && server_->tmpl == tmpl) return;
  server_.reset();
  if (!tmpl.empty())
    server_ = std::make_unique<GlyphServer>(tmpl, std::move(store), mailbox, [this, arrived] {
      reflow_ = true;
      if (arrived) arrived();
    });
  layoutCache_.clear();
  reflow_ = true;
}

void LabelRenderer::resetGpu() {
  glyphTexture_ = {};
  iconTexture_ = {};
  textBuffer_ = {};
  iconBuffer_ = {};
  indexBuffer_ = {};
  indexQuads_ = 0;
  glyphGeneration_ = iconGeneration_ = UINT64_MAX;
}

void LabelRenderer::clear() {
  labels_.clear();
  layoutCache_.clear();
  placedStructure_.clear();
}

FontSpec LabelRenderer::font(const StyleLayer& layer, const EvalContext& ctx) {
  const Value value = layer.layoutValue("font", ctx);
  std::vector<std::string> names;
  if (value.isArray())
    for (const auto& v : value.asArray())
      if (v.isString()) names.push_back(v.asString());
  if (names.empty()) names.push_back("sans-serif");
  FontSpec f;
  const Value& raw = layer.spec["layout"];
  if (!server_) {
    f.families = names;
    f.bold = layer.layoutValue("weight", ctx).stringOr("normal") == "bold";
    f.italic = layer.layoutValue("italic", ctx).boolOr(false);
    return f;
  }
  const NamedFont derived = fontFromNames(names);
  f.families = derived.families;
  f.bold = raw.has("weight") ? layer.layoutValue("weight", ctx).stringOr("normal") == "bold" : derived.bold;
  f.italic = raw.has("italic") ? layer.layoutValue("italic", ctx).boolOr(false) : derived.italic;
  for (size_t i = 0; i < names.size(); i++) f.stack += (i ? "," : "") + names[i];
  return f;
}

std::shared_ptr<const Laid> LabelRenderer::layout(const StyleLayer& layer, const std::string& text, bool line, int& budget, const TextShape& shape) {
  if (layoutGeneration_ != atlas.generation || layoutCache_.size() > 6000) {
    layoutCache_.clear();
    layoutGeneration_ = atlas.generation;
  }
  const std::string key = layer.id + "#" + std::to_string(layer.revision) + "#" + (line ? "1" : "0") + "#" + formatNumber(shape.letterSpacing) + "#" +
                          formatNumber(shape.lineHeight) + "#" + formatNumber(shape.maxWidth) + "#" + shape.font.key() + "#" + shape.font.stack +
                          "#" + text;
  if (auto it = layoutCache_.find(key); it != layoutCache_.end()) return it->second;
  std::string source = text;
  if (line) std::replace(source.begin(), source.end(), '\n', ' ');
  auto laid = computeLayout(source, line, budget, shape);
  if (laid) layoutCache_[key] = laid;
  return laid;
}

std::shared_ptr<const Laid> LabelRenderer::computeLayout(const std::string& text, bool line, int& budget, const TextShape& shape) {
  const auto units = utf8::decode(text);
  const double spacing = shape.letterSpacing * kGlyphSize;
  std::vector<std::optional<GlyphMetrics>> metrics;
  metrics.reserve(units.size());
  for (uint32_t u : units) {
    if (u == '\n') {
      metrics.emplace_back(std::nullopt);
      continue;
    }
    if (!shape.font.stack.empty() && server_ && fromServer(u)) {
      bool pending = false;
      const ServerGlyph* g = server_->glyph(shape.font.stack, u, pending);
      if (pending) {
        pendingGlyphs_ = true;
        return nullptr;
      }
      if (g) {
        metrics.push_back(atlas.server(shape.font.stack, u, *g));
        continue;
      }
    }
    if (const GlyphMetrics* m = atlas.get(shape.font, u)) {
      metrics.push_back(*m);
      continue;
    }
    if (budget <= 0) {
      pendingGlyphs_ = true;
      return nullptr;
    }
    budget--;
    metrics.push_back(atlas.rasterize(shape.font, u));
  }
  const double lineHeight = kGlyphSize * shape.lineHeight;
  const double maxWidth = line ? INFINITY : shape.maxWidth * kGlyphSize;
  struct Line {
    size_t start, end;
    double width;
  };
  std::vector<Line> lines;
  size_t start = 0;
  double x = 0, widthAtBreak = 0;
  int64_t lastBreak = -1;
  for (size_t i = 0; i < units.size(); i++) {
    if (units[i] == '\n') {
      lines.push_back({start, i, x});
      start = i + 1;
      x = 0;
      lastBreak = -1;
      continue;
    }
    const double advance = (metrics[i] ? metrics[i]->advance : 0) + spacing;
    if (units[i] == ' ') {
      lastBreak = static_cast<int64_t>(i);
      widthAtBreak = x;
    }
    if (x + advance > maxWidth && lastBreak > static_cast<int64_t>(start)) {
      lines.push_back({start, static_cast<size_t>(lastBreak), widthAtBreak});
      start = static_cast<size_t>(lastBreak) + 1;
      x = 0;
      for (size_t k = start; k <= i; k++) x += (metrics[k] ? metrics[k]->advance : 0) + spacing;
      lastBreak = -1;
      continue;
    }
    x += advance;
  }
  lines.push_back({start, units.size(), x});
  auto laid = std::make_shared<Laid>();
  for (const auto& l : lines) laid->width = std::max(laid->width, l.width);
  laid->height = lines.size() * lineHeight;
  for (size_t row = 0; row < lines.size(); row++) {
    const auto& l = lines[row];
    double pen = 0;
    const double baseline = row * lineHeight + kGlyphSize * 0.9 + (lineHeight - kGlyphSize * 1.2) / 2;
    for (size_t i = l.start; i < l.end; i++) {
      const auto& m = metrics[i];
      if (m && m->width > 0) laid->glyphs.push_back({*m, pen, baseline, static_cast<int>(row)});
      pen += (m ? m->advance : 0) + spacing;
    }
    laid->lines.push_back(l.width);
  }
  return laid;
}

bool LabelRenderer::update(Transform& transform, const std::vector<SymbolLayerInput>& inputs, double now, uint64_t cameraVersion, double pixelRatio) {
  std::string structure = "images:" + std::to_string(images.version);
  for (const auto& in : inputs) {
    structure += "," + in.layer->id + ":" + std::to_string(in.layer->revision) + ":" + std::to_string(in.layer->motion);
    for (const auto& piece : in.manager->pieces)
      if (piece.tile->data && piece.tile->data->buckets.contains(in.layer->id))
        structure += "," + std::to_string(piece.tile->key) + "/" + std::to_string(piece.tile->revision) + "/" + std::to_string(piece.wrap);
  }
  if (structure != placedStructure_ || pendingGlyphs_ || reflow_ ||
      (cameraVersion != placedCamera_ && now - lastPlacement_ >= kPlaceInterval)) {
    reflow_ = false;
    placement(transform, inputs, now);
    placedStructure_ = structure;
    placedCamera_ = cameraVersion;
    lastPlacement_ = now;
  }
  bool animating = cameraVersion != placedCamera_ || pendingGlyphs_;
  const double dt = std::clamp(now - (lastFrame_ == 0 ? now : lastFrame_), 0.0, 100.0);
  lastFrame_ = now;
  for (auto it = labels_.begin(); it != labels_.end();) {
    Label& l = it->second;
    if (l.opacity != l.target) {
      const double step = dt / kFadeMs;
      l.opacity = l.target > l.opacity ? std::min(l.target, l.opacity + step) : std::max(l.target, l.opacity - step);
      animating = true;
    }
    if (l.target == 0 && l.opacity <= 0) it = labels_.erase(it);
    else ++it;
  }
  build(transform, pixelRatio);
  return animating;
}

void LabelRenderer::placement(Transform& transform, const std::vector<SymbolLayerInput>& inputs, double now) {
  CollisionGrid grid(transform.width, transform.height);
  std::unordered_map<std::string, std::vector<std::array<double, 2>>> placed;
  std::unordered_set<std::string> visible;
  const double zoom = transform.zoom();
  int budget = 400;
  pendingGlyphs_ = false;
  const double ws = transform.worldSize();
  const double margin = 64;
  std::unordered_set<std::string> shown;
  for (const auto& [k, l] : labels_)
    if (l.target == 1) shown.insert(l.dedupe);
  std::function<bool(const std::string&)> hasImage = [this](const std::string& id) { return images.has(id); };
  for (int64_t li = static_cast<int64_t>(inputs.size()) - 1; li >= 0; li--) {
    const StyleLayer& layer = *inputs[li].layer;
    TileManager<VectorTileData>& manager = *inputs[li].manager;
    if (!layer.isVisible(zoom)) continue;
    EvalContext at;
    at.zoom = zoom;
    const bool allowOverlap = layer.layoutValue("allowOverlap", at).boolOr(false);
    const double padding = numberOr(layer.layoutValue("padding", at), 2);
    const double spacing = numberOr(layer.layoutValue("spacing", at), 250);
    const bool textOptional = layer.layoutValue("optional", at).boolOr(false);
    const double iconPadding = numberOr(layer.layoutValue("iconPadding", at), 2);
    const bool iconAllowOverlap = layer.layoutValue("iconAllowOverlap", at).boolOr(false);
    const bool iconOptional = layer.layoutValue("iconOptional", at).boolOr(false);
    const std::string alignment = layer.layoutValue("iconRotationAlignment", at).stringOr("auto");
    const std::string textAlignment = layer.layoutValue("rotationAlignment", at).stringOr("auto");
    const std::string pitchAlignment = layer.layoutValue("pitchAlignment", at).stringOr("auto");
    const bool ignoreText = layer.layoutValue("ignorePlacement", at).boolOr(false);
    const bool ignoreIcon = layer.layoutValue("iconIgnorePlacement", at).boolOr(false);
    const double maxAngle = numberOr(layer.layoutValue("maxAngle", at), 45) * kDeg;
    const bool keepUpright = layer.layoutValue("keepUpright", at).boolOr(true);
    const double lineHeight = numberOr(layer.layoutValue("lineHeight", at), 1.2);
    std::vector<std::string> variable;
    if (layer.fixed("variableAnchor").isArray())
      for (const auto& a : layer.fixed("variableAnchor").asArray())
        if (a.isString()) variable.push_back(a.asString());
    const auto textShift = screenShift(pairOr(layer.paintValue("translate", at)), layer.fixed("translateAnchor").stringOr("map"), transform.bearing());
    const auto iconShift = screenShift(pairOr(layer.paintValue("iconTranslate", at)), layer.fixed("iconTranslateAnchor").stringOr("map"), transform.bearing());
    struct Item {
      const RenderPiece<VectorTileData>* piece;
      VectorBucket* bucket;
      PackedLabels* labels;
      size_t i;
      double priority;
      int was;
      size_t order;
    };
    std::vector<Item> items;
    size_t order = 0;
    for (const auto& piece : manager.pieces) {
      if (!piece.tile->data) continue;
      auto it = piece.tile->data->buckets.find(layer.id);
      if (it == piece.tile->data->buckets.end() || !it->second->labels) continue;
      auto* labels = static_cast<PackedLabels*>(it->second->labels.get());
      for (size_t i = 0; i < labels->count(); i++) {
        const int was = shown.contains(layer.id + "|" + labels->texts[i] + "|" + labels->icons[i]) ? 0 : 1;
        items.push_back({&piece, it->second.get(), labels, i, labels->priority[i], was, order++});
      }
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
      if (a.priority != b.priority) return a.priority > b.priority;
      if (a.was != b.was) return a.was < b.was;
      return a.order < b.order;
    });
    for (const Item& item : items) {
      const auto& piece = *item.piece;
      const PackedLabels& labels = *item.labels;
      VectorBucket& bucket = *item.bucket;
      const auto& tile = *piece.tile;
      const double extent = bucket.extent;
      const double n = std::exp2(tile.z);
      const Rect& clip = piece.clip;
      const size_t i = item.i;
      const std::string& text = labels.texts[i];
      const bool isLine = labels.line[i] == 1;
      const uint32_t fi = labels.feature[i];
      const FeatureRecord& rec = bucket.features.features[fi];
      const std::string dedupeKey = layer.id + "|" + text + "|" + labels.icons[i];
      const bool alignMap = alignment == "map" || (alignment != "viewport" && isLine);
      const bool followLine = isLine && textAlignment != "viewport";
      const bool ground = followLine && pitchAlignment != "viewport";
      FeatureContext feature;
      feature.properties = &rec.properties;
      feature.id = !rec.textId.empty() ? Value(rec.textId) : rec.id ? Value(*rec.id) : Value();
      feature.geometryType = rec.type == 1 ? GeometryType::Point : rec.type == 2 ? GeometryType::LineString : rec.type == 3 ? GeometryType::Polygon : GeometryType::Unknown;
      EvalContext ctx;
      ctx.zoom = zoom;
      ctx.feature = &feature;
      ctx.images = &hasImage;
      bool ready = false;
      std::string fixedAnchor = "center", iconAnchor = "center", justifyMode = "center";
      std::array<double, 2> iconOffset{0, 0}, textOffset{0, 0};
      std::optional<double> radial;
      TextShape shape;
      shape.lineHeight = lineHeight;
      std::string iconId;
      for (size_t a = labels.anchorStart[i]; a + 2 < labels.anchorStart[i + 1]; a += 3) {
        const double tx = labels.anchors[a] / extent, ty = labels.anchors[a + 1] / extent;
        if (tx < clip.x0 || tx >= clip.x1 || ty < clip.y0 || ty >= clip.y1) continue;
        const double wx = piece.wrap + (tile.x + tx) / n, wy = (tile.y + ty) / n;
        const auto p = transform.project(wx, wy, 0);
        const double depth = p.w / transform.cameraToCenterDistance();
        if (p.w <= 0 || depth > 2.2) continue;
        if (p.x < -margin || p.y < -margin || p.x > transform.width + margin || p.y > transform.height + margin) continue;
        if (!ready) {
          ready = true;
          const std::string fa = layer.layoutValue("anchor", ctx).stringOr("center");
          fixedAnchor = isAnchor(fa) ? fa : "center";
          const std::string ia = layer.layoutValue("iconAnchor", ctx).stringOr("center");
          iconAnchor = isAnchor(ia) ? ia : "center";
          iconOffset = pairOr(layer.layoutValue("iconOffset", ctx));
          const Value r = layer.layoutValue("radialOffset", ctx);
          if (r.isNumber() && std::isfinite(r.asNumber())) radial = r.asNumber();
          textOffset = pairOr(layer.layoutValue("offset", ctx));
          justifyMode = layer.layoutValue("justify", ctx).stringOr("center");
          shape.letterSpacing = numberOr(layer.layoutValue("letterSpacing", ctx), 0);
          shape.maxWidth = numberOr(layer.layoutValue("maxWidth", ctx), 10);
          shape.font = font(layer, ctx);
          iconId = layer.layoutValue("icon", ctx).stringOr("");
        }
        std::shared_ptr<const Laid> laid = text.empty() ? std::make_shared<Laid>() : layout(layer, text, followLine, budget, shape);
        if (!laid) continue;
        const double perspective = std::clamp(1.3 - 0.3 * depth, 0.7, 1.0);
        const double anchorDist = labels.anchors[a + 2] / extent / n;
        std::optional<IconPart> icon;
        if (!iconId.empty()) {
          if (const StyleImage* img = images.get(iconId)) {
            IconPart part;
            part.id = iconId;
            part.width = img->width / img->pixelRatio;
            part.height = img->height / img->pixelRatio;
            const Shift s = anchorShift(iconAnchor);
            part.left = s.x * part.width + iconOffset[0];
            part.top = s.y * part.height + iconOffset[1];
            part.size = std::max(0.0, layer.layoutNumber("iconSize", ctx)) * perspective;
            part.rotate = layer.layoutNumber("iconRotate", ctx) * kDeg;
            part.alignMap = alignMap;
            part.alongLine = isLine;
            part.color = layer.paintColor("iconColor", ctx);
            part.halo = layer.paintColor("iconHaloColor", ctx);
            part.haloWidth = layer.paintNumber("iconHaloWidth", ctx);
            part.opacity = layer.paintNumber("iconOpacity", ctx);
            part.sdf = img->sdf;
            part.pixelRatio = img->pixelRatio;
            icon = part;
          }
        }
        if (text.empty() && !icon) continue;
        const double size = layer.layoutNumber("size", ctx);
        const double scale = size / kGlyphSize * perspective;
        const double w = laid->width * scale;
        const double rotate = followLine ? 0 : layer.layoutNumber("rotate", ctx) * kDeg;
        const double minDist = isLine ? std::max(spacing, w * 1.5) : std::max(80.0, w * 1.2);
        const std::string key = dedupeKey + "|" + (rec.id ? formatNumber(*rec.id) : std::string()) + "|" +
                                std::to_string(static_cast<int64_t>(std::llround(wx * ws / 4))) + "|" +
                                std::to_string(static_cast<int64_t>(std::llround(wy * ws / 4)));
        auto existing = labels_.find(key);
        auto prior = text.empty() ? placed.end() : placed.find(dedupeKey);
        if (prior != placed.end()) {
          bool near = false;
          for (const auto& q : prior->second) near = near || std::hypot(q[0] - p.x, q[1] - p.y) < minDist;
          if (near) continue;
        }
        std::vector<double> path, boxes;
        const size_t ps = labels.pathStart[i], pe = labels.pathStart[i + 1];
        if (isLine && pe - ps >= 4) {
          path.resize(pe - ps);
          for (size_t k = 0; k < path.size(); k += 2) {
            path[k] = piece.wrap + (tile.x + labels.paths[ps + k] / extent) / n;
            path[k + 1] = (tile.y + labels.paths[ps + k + 1] / extent) / n;
          }
          if (!text.empty() && followLine && ground) {
            const bool fits = layoutOnGround(transform, path, anchorDist, *laid, size, maxAngle, keepUpright, [&](const LaidGlyph&, const double* c, double) {
              boxes.insert(boxes.end(), {std::min({c[0], c[2], c[4], c[6]}) + textShift[0] - padding, std::min({c[1], c[3], c[5], c[7]}) + textShift[1] - padding,
                                         std::max({c[0], c[2], c[4], c[6]}) + textShift[0] + padding, std::max({c[1], c[3], c[5], c[7]}) + textShift[1] + padding});
            });
            if (!fits) continue;
          } else if (!text.empty() && followLine) {
            const auto quads = layoutAlongPath(transform, path, anchorDist, *laid, scale, maxAngle, keepUpright);
            if (!quads) continue;
            for (const auto& q : *quads) {
              const double half = size * 0.6 / 2 + padding;
              boxes.insert(boxes.end(), {q.cx + textShift[0] - half, q.cy + textShift[1] - half, q.cx + textShift[0] + half, q.cy + textShift[1] + half});
            }
          }
        }
        std::string chosenAnchor = fixedAnchor;
        double offX = 0, offY = 0, justify = 0.5;
        bool textHit = false;
        auto textPlacement = [&](const std::string& anchor, std::vector<double>& outBoxes) {
          double dx = textOffset[0], dy = textOffset[1];
          if (radial) {
            const Shift r = radialOffset(*radial, anchor);
            dx = r.x;
            dy = r.y;
          } else if (!variable.empty()) {
            const Shift s = anchorShift(anchor);
            dx = s.x == 0 ? std::abs(textOffset[0]) : s.x == -1 ? -std::abs(textOffset[0]) : 0;
            dy = s.y == 0 ? std::abs(textOffset[1]) : s.y == -1 ? -std::abs(textOffset[1]) : 0;
          }
          const Shift s = anchorShift(anchor);
          offX = s.x * laid->width + dx * kGlyphSize;
          offY = s.y * laid->height + dy * kGlyphSize;
          justify = justifyMode == "auto" ? (s.x == 0 ? 0 : s.x == -1 ? 1 : 0.5) : justifyMode == "left" ? 0 : justifyMode == "right" ? 1 : 0.5;
          const double x0 = offX * scale, y0 = offY * scale, x1 = x0 + laid->width * scale, y1 = y0 + laid->height * scale;
          double bx0 = x0, by0 = y0, bx1 = x1, by1 = y1;
          if (rotate != 0) {
            const double cs = std::cos(rotate), sn = std::sin(rotate);
            bx0 = by0 = INFINITY;
            bx1 = by1 = -INFINITY;
            for (const auto& c : {std::array<double, 2>{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}) {
              const double rx = c[0] * cs - c[1] * sn, ry = c[0] * sn + c[1] * cs;
              bx0 = std::min(bx0, rx);
              by0 = std::min(by0, ry);
              bx1 = std::max(bx1, rx);
              by1 = std::max(by1, ry);
            }
          }
          const double ox = p.x + textShift[0], oy = p.y + textShift[1];
          outBoxes = {ox + bx0 - padding, oy + by0 - padding, ox + bx1 + padding, oy + by1 + padding};
        };
        if (!text.empty() && !followLine) {
          std::vector<std::string> tries;
          if (!variable.empty()) {
            if (existing != labels_.end() && std::find(variable.begin(), variable.end(), existing->second.anchor) != variable.end()) {
              tries.push_back(existing->second.anchor);
              for (const auto& a2 : variable)
                if (a2 != existing->second.anchor) tries.push_back(a2);
            } else {
              tries = variable;
            }
          } else {
            tries = {fixedAnchor};
          }
          bool found = false;
          for (const auto& anchor : tries) {
            std::vector<double> candidate;
            textPlacement(anchor, candidate);
            if (allowOverlap || !grid.hitsAny(candidate)) {
              boxes = candidate;
              chosenAnchor = anchor;
              found = true;
              break;
            }
          }
          if (!found) {
            textHit = true;
            textPlacement(tries[0], boxes);
            chosenAnchor = tries[0];
          }
        } else if (!text.empty()) {
          textHit = !allowOverlap && grid.hitsAny(boxes);
        }
        std::vector<double> iconBox;
        if (icon) {
          double angle = icon->rotate;
          if (icon->alignMap) angle -= transform.bearing() * kDeg;
          const double cs = std::cos(angle), sn = std::sin(angle);
          double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
          for (const auto& c : {std::array<double, 2>{icon->left, icon->top}, {icon->left + icon->width, icon->top}, {icon->left + icon->width, icon->top + icon->height},
                                {icon->left, icon->top + icon->height}}) {
            const double px = p.x + iconShift[0] + (c[0] * cs - c[1] * sn) * icon->size;
            const double py = p.y + iconShift[1] + (c[0] * sn + c[1] * cs) * icon->size;
            x0 = std::min(x0, px);
            y0 = std::min(y0, py);
            x1 = std::max(x1, px);
            y1 = std::max(y1, py);
          }
          iconBox = {x0 - iconPadding, y0 - iconPadding, x1 + iconPadding, y1 + iconPadding};
        }
        bool showText = !text.empty();
        bool showIcon = icon.has_value();
        const bool iconHit = !iconBox.empty() && !iconAllowOverlap && grid.hitsAny(iconBox);
        if (showText && showIcon) {
          if (textHit && iconHit) continue;
          if (textHit) {
            if (!textOptional) continue;
            showText = false;
          }
          if (iconHit) {
            if (!iconOptional) continue;
            showIcon = false;
          }
        } else if (textHit || iconHit) {
          continue;
        }
        if (!showText) boxes.clear();
        if (showText && !ignoreText)
          for (size_t b = 0; b + 3 < boxes.size(); b += 4) grid.insert(boxes[b], boxes[b + 1], boxes[b + 2], boxes[b + 3]);
        if (showIcon && !iconBox.empty()) {
          if (!ignoreIcon) grid.insert(iconBox[0], iconBox[1], iconBox[2], iconBox[3]);
          boxes.insert(boxes.end(), iconBox.begin(), iconBox.end());
        }
        if (showText) placed[dedupeKey].push_back({p.x, p.y});
        visible.insert(key);
        const double opacity = layer.paintNumber("opacity", ctx);
        Label& l = existing != labels_.end() ? existing->second : labels_[key];
        if (existing == labels_.end()) {
          l.key = key;
          l.opacity = 0;
        }
        l.dedupe = dedupeKey;
        l.layer = &layer;
        l.text = text;
        l.line = followLine;
        l.wx = wx;
        l.wy = wy;
        l.wz = 0;
        l.path = std::move(path);
        l.anchorDist = anchorDist;
        l.laid = laid;
        l.scale = scale;
        l.ground = ground;
        l.size = size;
        l.color = withOpacity(layer.paintColor("color", ctx), opacity);
        l.halo = withOpacity(layer.paintColor("haloColor", ctx), opacity);
        l.haloWidth = layer.paintNumber("haloWidth", ctx);
        l.haloBlur = std::max(0.0, layer.paintNumber("haloBlur", ctx));
        l.target = 1;
        l.boxes = std::move(boxes);
        l.showText = showText;
        l.icon = showIcon ? icon : std::nullopt;
        l.anchor = chosenAnchor;
        l.offsetX = offX;
        l.offsetY = offY;
        l.justify = justify;
        l.rotate = rotate;
        l.tileKey = tile.key;
        if (!isLine) break;
      }
    }
  }
  for (auto& [k, l] : labels_)
    if (!visible.contains(k)) l.target = 0;
  (void)now;
}

void LabelRenderer::build(Transform& transform, double pixelRatio) {
  textVertices_.clear();
  iconVertices_.clear();
  size_t visible = 0;
  for (auto& [key, label] : labels_) {
    if (label.opacity <= 0) continue;
    const auto p = transform.project(label.wx, label.wy, 0);
    if (label.icon && p.w > 0) {
      const IconPart& icon = *label.icon;
      if (const StyleImage* img = images.get(icon.id)) {
        double angle = icon.rotate;
        if (icon.alongLine && icon.alignMap && label.path.size() >= 4) {
          const auto a = transform.project(label.path[0], label.path[1], 0);
          const auto b = transform.project(label.path[2], label.path[3], 0);
          if (a.w > 0 && b.w > 0) angle += std::atan2(b.y - a.y, b.x - a.x);
        } else if (icon.alignMap) {
          angle -= transform.bearing() * kDeg;
        }
        const auto shift = screenShift(pairOr(label.layer->paintValue("iconTranslate", EvalContext{transform.zoom()})),
                                       label.layer->fixed("iconTranslateAnchor").stringOr("map"), transform.bearing());
        const double ox = p.x + shift[0], oy = p.y + shift[1];
        const double cs = std::cos(angle), sn = std::sin(angle), s = icon.size;
        const double u0 = img->x, v0 = img->y, u1 = img->x + img->width, v1 = img->y + img->height;
        const std::array<std::array<double, 4>, 4> corners{{{icon.left, icon.top, u0, v0},
                                                            {icon.left + icon.width, icon.top, u1, v0},
                                                            {icon.left + icon.width, icon.top + icon.height, u1, v1},
                                                            {icon.left, icon.top + icon.height, u0, v1}}};
        for (const auto& c : corners) {
          double x = ox + (c[0] * cs - c[1] * sn) * s;
          double y = oy + (c[0] * sn + c[1] * cs) * s;
          if (angle == 0) {
            x = std::round(x * pixelRatio) / pixelRatio;
            y = std::round(y * pixelRatio) / pixelRatio;
          }
          putVertex(iconVertices_, x, y, c[2], c[3], icon.color, icon.halo, icon.opacity * label.opacity, s * icon.pixelRatio, icon.haloWidth,
                    icon.sdf ? 1 : 0);
        }
      }
    }
    if (!label.showText || !label.laid) continue;
    const double s = label.scale;
    const Laid& laid = *label.laid;
    auto put = [&](double x, double y, double u, double v, double scale) {
      putVertex(textVertices_, x, y, u, v, label.color, label.halo, label.opacity, scale, label.haloWidth, label.haloBlur);
    };
    if (label.line && !label.path.empty() && label.ground) {
      const size_t mark = textVertices_.size();
      const bool fits = layoutOnGround(transform, label.path, label.anchorDist, laid, label.size, kPi / 4, true, [&](const LaidGlyph& g, const double* c, double scale) {
        const auto& m = g.m;
        put(c[0], c[1], m.x, m.y, scale);
        put(c[2], c[3], m.x + m.width, m.y, scale);
        put(c[4], c[5], m.x + m.width, m.y + m.height, scale);
        put(c[6], c[7], m.x, m.y + m.height, scale);
      });
      if (fits) visible++;
      else textVertices_.resize(mark);
      continue;
    }
    if (label.line && !label.path.empty()) {
      const auto quads = layoutAlongPath(transform, label.path, label.anchorDist, laid, s);
      if (!quads) continue;
      visible++;
      for (const auto& q : *quads) {
        const auto& m = q.glyph->m;
        const double cs = std::cos(q.angle), sn = std::sin(q.angle);
        const double x0 = (m.left - q.advance / 2) * s, x1 = x0 + m.width * s;
        const double y0 = (m.top + kGlyphSize * 0.32) * s, y1 = y0 + m.height * s;
        const std::array<std::array<double, 4>, 4> corners{{{x0, y0, static_cast<double>(m.x), static_cast<double>(m.y)},
                                                            {x1, y0, static_cast<double>(m.x + m.width), static_cast<double>(m.y)},
                                                            {x1, y1, static_cast<double>(m.x + m.width), static_cast<double>(m.y + m.height)},
                                                            {x0, y1, static_cast<double>(m.x), static_cast<double>(m.y + m.height)}}};
        for (const auto& c : corners) put(q.cx + c[0] * cs - c[1] * sn, q.cy + c[0] * sn + c[1] * cs, c[2], c[3], s);
      }
      continue;
    }
    if (p.w <= 0) continue;
    visible++;
    const auto shift = screenShift(pairOr(label.layer->paintValue("translate", EvalContext{transform.zoom()})),
                                   label.layer->fixed("translateAnchor").stringOr("map"), transform.bearing());
    const double originX = p.x + shift[0], originY = p.y + shift[1];
    const double rotate = label.rotate;
    const double cs = std::cos(rotate), sn = std::sin(rotate);
    const double ax = rotate == 0 ? std::round((originX + label.offsetX * s) * pixelRatio) / pixelRatio : 0;
    const double ay = rotate == 0 ? std::round((originY + label.offsetY * s) * pixelRatio) / pixelRatio : 0;
    for (const auto& g : laid.glyphs) {
      const auto& m = g.m;
      const double lineWidth = g.line < static_cast<int>(laid.lines.size()) ? laid.lines[g.line] : laid.width;
      const double gx = g.x + (laid.width - lineWidth) * label.justify;
      const double lx0 = (gx + m.left) * s, ly0 = (g.y + m.top) * s;
      const double lx1 = lx0 + m.width * s, ly1 = ly0 + m.height * s;
      if (rotate == 0) {
        put(ax + lx0, ay + ly0, m.x, m.y, s);
        put(ax + lx1, ay + ly0, m.x + m.width, m.y, s);
        put(ax + lx1, ay + ly1, m.x + m.width, m.y + m.height, s);
        put(ax + lx0, ay + ly1, m.x, m.y + m.height, s);
        continue;
      }
      const double bx = label.offsetX * s, by = label.offsetY * s;
      const std::array<std::array<double, 4>, 4> corners{{{lx0, ly0, static_cast<double>(m.x), static_cast<double>(m.y)},
                                                          {lx1, ly0, static_cast<double>(m.x + m.width), static_cast<double>(m.y)},
                                                          {lx1, ly1, static_cast<double>(m.x + m.width), static_cast<double>(m.y + m.height)},
                                                          {lx0, ly1, static_cast<double>(m.x), static_cast<double>(m.y + m.height)}}};
      for (const auto& c : corners) {
        const double x = bx + c[0], y = by + c[1];
        put(originX + x * cs - y * sn, originY + x * sn + y * cs, c[2], c[3], s);
      }
    }
  }
  visible_ = visible;
  glyphCount_ = textVertices_.size() * 4 / kStride / 4;
  iconCount_ = iconVertices_.size() * 4 / kStride / 4;
}

void LabelRenderer::syncTextures() {
  Gpu& gpu = Gpu::get();
  if (atlas.dirty || glyphGeneration_ != atlas.generation || !glyphTexture_.valid()) {
    glyphTexture_.release();
    glyphTexture_ = gpu.texture(atlas.width, atlas.height, SG_PIXELFORMAT_R8, atlas.data.data(), atlas.data.size());
    atlas.dirty = false;
    glyphGeneration_ = atlas.generation;
  }
  if (images.dirty || iconGeneration_ != images.generation || !iconTexture_.valid()) {
    iconTexture_.release();
    iconTexture_ = gpu.texture(images.width, images.height, SG_PIXELFORMAT_RGBA8, images.data.data(), images.data.size());
    images.dirty = false;
    iconGeneration_ = images.generation;
  }
}

int LabelRenderer::draw(Transform& transform, double pixelRatio, const PassFormat& format) {
  if (glyphCount_ == 0 && iconCount_ == 0) return 0;
  syncTextures();
  Gpu& gpu = Gpu::get();
  const size_t quads = std::max(glyphCount_, iconCount_);
  if (quads > indexQuads_) {
    indexBuffer_.release();
    indexQuads_ = std::max<size_t>(1024, quads * 2);
    std::vector<uint32_t> idx(indexQuads_ * 6);
    for (size_t q = 0; q < indexQuads_; q++) {
      const uint32_t b = static_cast<uint32_t>(q * 4);
      idx[q * 6] = b;
      idx[q * 6 + 1] = b + 1;
      idx[q * 6 + 2] = b + 2;
      idx[q * 6 + 3] = b;
      idx[q * 6 + 4] = b + 2;
      idx[q * 6 + 5] = b + 3;
    }
    indexBuffer_ = gpu.indexBuffer(idx.data(), idx.size() * 4);
  }
  int calls = 0;
  auto drawSet = [&](std::vector<float>& vertices, size_t count, GpuBuffer& buffer, Program program, const GpuTexture& texture, double atlasW,
                     double atlasH) {
    if (count == 0) return;
    buffer.release();
    buffer = gpu.vertexBuffer(vertices.data(), vertices.size() * 4);
    PipelineKey key;
    key.program = program;
    key.color = format.color;
    key.depthFormat = format.depth;
    key.samples = format.samples;
    sg_apply_pipeline(gpu.pipeline(key));
    sg_bindings b{};
    b.vertex_buffers[0] = buffer.buffer;
    b.index_buffer = indexBuffer_.buffer;
    b.views[VIEW_mk_atlas_tex] = texture.view;
    b.samplers[SMP_mk_atlas_smp] = gpu.sampler(true);
    sg_apply_bindings(&b);
    mk_label_vs_params_t vs{};
    vs.u_view[0] = static_cast<float>(transform.width);
    vs.u_view[1] = static_cast<float>(transform.height);
    vs.u_view[2] = static_cast<float>(atlasW);
    vs.u_view[3] = static_cast<float>(atlasH);
    sg_apply_uniforms(UB_mk_label_vs_params, SG_RANGE(vs));
    for (int pass = 0; pass < 2; pass++) {
      mk_label_fs_params_t fs{};
      fs.u_info[0] = static_cast<float>(pass);
      fs.u_info[1] = static_cast<float>(pixelRatio);
      fs.u_info[2] = static_cast<float>(kSdfRadius);
      sg_apply_uniforms(UB_mk_label_fs_params, SG_RANGE(fs));
      sg_draw(0, static_cast<int>(count * 6), 1);
      calls++;
    }
  };
  drawSet(iconVertices_, iconCount_, iconBuffer_, Program::Icon, iconTexture_, images.width, images.height);
  drawSet(textVertices_, glyphCount_, textBuffer_, Program::Text, glyphTexture_, atlas.width, atlas.height);
  return calls;
}

}
