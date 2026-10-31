#include "ui/overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/shaders/map.glsl.h"

namespace mapkern::ui {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFeetPerMeter = 3.28084;
constexpr double kFeetPerMile = 5280;

const std::vector<std::string> kPlus{"M12 6v12", "M6 12h12"};
const std::vector<std::string> kMinus{"M6 12h12"};
const std::vector<std::string> kLayers{"M12 3l9 5-9 5-9-5z", "M3 12.5l9 5 9-5", "M3 17l9 5 9-5"};
const std::vector<std::string> kGlobe{"M12 3a9 9 0 1 0 0 18a9 9 0 1 0 0-18z", "M12 3c-3.2 3-3.2 15 0 18", "M12 3c3.2 3 3.2 15 0 18", "M3.5 9h17",
                                      "M3.5 15h17"};
const std::vector<std::string> kInfo{"M12 3a9 9 0 1 0 0 18a9 9 0 1 0 0-18z", "M12 11v6", "M12 7.6v0.2"};
const std::string kNorth = "M12 3l4 9h-8z";
const std::string kSouth = "M12 21l-4-9h8z";

const Color kInk = rgba(16, 25, 29, 1);
const Color kWhite = rgba(255, 255, 255, 1);

const Shape& shape(const std::string& d) {
  static std::map<std::string, Shape> cache;
  auto it = cache.find(d);
  if (it == cache.end()) it = cache.emplace(d, parsePath(d)).first;
  return it->second;
}

Affine iconBox(double x, double y, double size) { return {size / 24, 0, 0, size / 24, x, y}; }

void strokeIcon(Canvas& c, const std::vector<std::string>& paths, const Affine& m, double width, const Color& color) {
  for (const auto& d : paths) c.stroke(shape(d), m, width, color);
}

void panel(Canvas& c, double x, double y, double w, double h, double r) {
  c.shadow(x, y, w, h, r, 10, 2, 0.08);
  c.shadow(x, y, w, h, r, 2, 1, 0.14);
  c.roundRect(x, y, w, h, r, kWhite);
}

double niceNumber(double n) {
  if (!(n > 0)) return 0;
  const double exponent = std::floor(std::log10(n));
  const double d = n / std::pow(10, exponent);
  const double nice = d >= 5 ? 5 : d >= 3 ? 3 : d >= 2 ? 2 : 1;
  return exponent >= 0 ? nice * std::pow(10, exponent) : nice / std::pow(10, -exponent);
}

std::string shortNumber(double n) {
  if (n >= 100) return std::to_string(static_cast<long long>(std::llround(n)));
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3g", n);
  return buf;
}

std::string formatValue(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

std::vector<std::string> wrapText(const std::string& text, double px, double width) {
  std::vector<std::string> lines;
  std::string current;
  size_t i = 0;
  while (i <= text.size()) {
    size_t j = text.find(' ', i);
    if (j == std::string::npos) j = text.size();
    const std::string word = text.substr(i, j - i);
    if (!word.empty()) {
      const std::string candidate = current.empty() ? word : current + " " + word;
      if (!current.empty() && Canvas::measure(candidate, px, false) > width) {
        lines.push_back(current);
        current = word;
      } else {
        current = candidate;
      }
    }
    i = j + 1;
  }
  if (!current.empty()) lines.push_back(current);
  return lines;
}

}

ScaleBar scaleBar(double meters, double maxWidth, bool imperial) {
  double span, size;
  std::string name;
  if (imperial) {
    const double feet = meters * kFeetPerMeter;
    if (feet > kFeetPerMile) {
      span = feet / kFeetPerMile;
      name = "mi";
      size = kFeetPerMile / kFeetPerMeter;
    } else {
      span = feet;
      name = "ft";
      size = 1 / kFeetPerMeter;
    }
  } else if (meters >= 1000) {
    span = meters / 1000;
    name = "km";
    size = 1000;
  } else {
    span = meters;
    name = "m";
    size = 1;
  }
  const double value = niceNumber(span);
  return {formatValue(value) + " " + name, value * size, std::round(maxWidth * value / span)};
}

std::string imperialLabel(double meters) {
  const double feet = meters * kFeetPerMeter;
  return feet < kFeetPerMile ? shortNumber(feet) + " ft" : shortNumber(feet / kFeetPerMile) + " mi";
}

std::string plainAttribution(const std::string& html) {
  std::string out;
  bool tag = false;
  size_t i = 0;
  while (i < html.size()) {
    const char ch = html[i++];
    if (ch == '<') tag = true;
    else if (ch == '>') tag = false;
    else if (!tag) {
      if (ch == '&') {
        const size_t end = html.find(';', i);
        if (end != std::string::npos && end - i <= 7) {
          const std::string ent = html.substr(i, end - i);
          if (ent == "copy") out += "\xC2\xA9";
          else if (ent == "amp") out += "&";
          else if (ent == "lt") out += "<";
          else if (ent == "gt") out += ">";
          else if (ent == "nbsp") out += " ";
          else if (ent == "quot") out += "\"";
          else out += html.substr(i - 1, end - i + 2);
          i = end + 1;
          continue;
        }
      }
      out += ch == '\n' ? ' ' : ch;
    }
  }
  std::string collapsed;
  for (char ch : out)
    if (!(ch == ' ' && (collapsed.empty() || collapsed.back() == ' '))) collapsed += ch;
  while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
  return collapsed;
}

void Overlay::update(const OverlayState& state) {
  state_ = state;
  if (!pending_.empty() && pending_ == state.view) pending_.clear();
  layout(state);
}

void Overlay::layout(const OverlayState& s) {
  elements_.clear();
  const double m = touch ? 16 : 10;
  const double b = touch ? 44 : 32;
  const double icon = touch ? 22 : 18;
  const double pad = 14;
  const double W = s.width, H = s.height;
  if (W < 4 * b || H < 4 * b) return;
  const double rx = W - m - b;
  double bottom = H - m;

  if (s.metersPerMaxWidth > 0) {
    const ScaleBar bar = scaleBar(s.metersPerMaxWidth, 100, false);
    const std::string label = bar.label + " \xC2\xB7 " + imperialLabel(bar.meters);
    const double px = touch ? 12 : 11;
    const double line = touch ? 18 : 16;
    const double tw = Canvas::measure(label, px, false);
    const double inner = std::max(tw, bar.width);
    const double w = inner + 12, h = 1 + line + 1 + 5 + 4;
    Element e;
    e.id = "scale";
    e.x = W - m - w;
    e.y = bottom - h;
    e.w = w;
    e.h = h;
    e.pad = 2;
    e.key = label + "|" + std::to_string(bar.width) + "|" + std::to_string(touch);
    const double barWidth = bar.width;
    e.paint = [=](Canvas& c, double ox, double oy) {
      c.roundRect(ox, oy, w, h, 4, rgba(255, 255, 255, 0.7));
      const double base = oy + 1 + (line + Canvas::ascent(px, false) * 0.72) / 2;
      c.text(ox + 6 + (inner - tw) / 2, base, label, px, false, kInk);
      const double ly = oy + 1 + line + 1;
      const double lx = ox + 6 + (inner - barWidth) / 2;
      const Color ink = rgba(16, 25, 29, 0.85);
      c.rect(lx, ly, 2, 5, ink);
      c.rect(lx + barWidth - 2, ly, 2, 5, ink);
      c.rect(lx, ly + 3, barWidth, 2, ink);
    };
    elements_.push_back(std::move(e));
    bottom -= h + m;
  }

  {
    const bool atMax = s.zoom >= s.maxZoom - 1e-9;
    const bool atMin = s.zoom <= s.minZoom + 1e-9;
    const double squash = std::min(1 / std::cos(s.pitch * kPi / 180), 3.0);
    const double bearing = std::round(s.bearing * 10) / 10;
    Element e;
    e.id = "navigation";
    e.x = rx;
    e.y = bottom - 3 * b;
    e.w = b;
    e.h = 3 * b;
    e.pad = pad;
    char key[96];
    std::snprintf(key, sizeof key, "%d%d|%.1f|%.2f|%d", atMax, atMin, bearing, squash, touch);
    e.key = key;
    e.paint = [=](Canvas& c, double ox, double oy) {
      panel(c, ox, oy, b, 3 * b, 8);
      c.rect(ox, oy + b, b, 1, rgba(16, 25, 29, 0.08));
      c.rect(ox, oy + 2 * b, b, 1, rgba(16, 25, 29, 0.08));
      const double io = (b - icon) / 2;
      strokeIcon(c, kPlus, iconBox(ox + io, oy + io, icon), 2 * icon / 24, rgba(16, 25, 29, atMax ? 0.35 : 1));
      strokeIcon(c, kMinus, iconBox(ox + io, oy + b + io, icon), 2 * icon / 24, rgba(16, 25, 29, atMin ? 0.35 : 1));
      const double cx = ox + b / 2, cy = oy + 2.5 * b;
      const double a = -bearing * kPi / 180, k = icon / 24;
      const double ca = std::cos(a), sa = std::sin(a);
      Affine n{ca * k, sa * k * squash, -sa * k, ca * k * squash, 0, 0};
      n.e = cx - (n.a * 12 + n.c * 12);
      n.f = cy - (n.b * 12 + n.d * 12);
      c.fill(shape(kNorth), n, rgba(217, 71, 63, 1));
      c.stroke(shape(kNorth), n, 2 * k, rgba(217, 71, 63, 1));
      c.stroke(shape(kSouth), n, 2 * k, kInk);
    };
    e.hits = {{rx, bottom - 3 * b, b, b, 0}, {rx, bottom - 2 * b, b, b, 1}, {rx, bottom - b, b, b, 2}};
    e.click = [this, atMax, atMin](size_t part) {
      if (part == 0 && !atMax && actions.zoomIn) actions.zoomIn();
      if (part == 1 && !atMin && actions.zoomOut) actions.zoomOut();
      if (part == 2 && actions.resetNorth) actions.resetNorth();
    };
    elements_.push_back(std::move(e));
    bottom -= 3 * b + m;
  }

  if (!views.empty()) {
    const std::string view = pending_.empty() ? s.view : pending_;
    Element e;
    e.id = "terrain";
    e.x = rx;
    e.y = bottom - b;
    e.w = b;
    e.h = b;
    e.pad = pad;
    e.key = view + std::to_string(touch);
    const double px = touch ? 14 : 12;
    e.paint = [=](Canvas& c, double ox, double oy) {
      panel(c, ox, oy, b, b, 8);
      if (view == "globe") {
        const double io = (b - icon) / 2;
        strokeIcon(c, kGlobe, iconBox(ox + io, oy + io, icon), 2 * icon / 24, kInk);
        return;
      }
      const std::string label = view == "tilted" ? "3D" : "2D";
      const double tw = Canvas::measure(label, px, true);
      c.text(ox + (b - tw) / 2, oy + b / 2 + Canvas::ascent(px, true) * 0.36, label, px, true, kInk);
    };
    e.hits = {{rx, bottom - b, b, b, 0}};
    e.click = [this](size_t) {
      const std::string now = pending_.empty() ? state_.view : pending_;
      auto it = std::find(views.begin(), views.end(), now);
      const size_t i = it == views.end() ? 0 : (static_cast<size_t>(it - views.begin()) + 1) % views.size();
      pending_ = views[i];
      if (actions.setView) actions.setView(views[i]);
    };
    elements_.push_back(std::move(e));
    bottom -= b + m;
  }

  if (basemaps.size() > 1) {
    Element e;
    e.id = "basemaps";
    e.x = rx;
    e.y = bottom - b;
    e.w = b;
    e.h = b;
    e.pad = pad;
    e.key = std::to_string(touch);
    e.paint = [=](Canvas& c, double ox, double oy) {
      panel(c, ox, oy, b, b, 8);
      const double io = (b - icon) / 2;
      strokeIcon(c, kLayers, iconBox(ox + io, oy + io, icon), 2 * icon / 24, kInk);
    };
    e.hits = {{rx, bottom - b, b, b, 0}};
    e.click = [this](size_t) { menu_ = !menu_; };
    const double buttonBottom = bottom;
    elements_.push_back(std::move(e));
    if (menu_) {
      const double px = touch ? 15 : 13;
      const double ih = touch ? 44 : 36;
      double tw = 0;
      for (const auto& bm : basemaps) tw = std::max(tw, Canvas::measure(bm.name, px, true));
      const double w = std::max(140.0, tw + 24 + 8), h = basemaps.size() * ih + 8;
      Element menu;
      menu.id = "menu";
      menu.x = rx - 8 - w;
      menu.y = buttonBottom - h;
      menu.w = w;
      menu.h = h;
      menu.pad = pad;
      menu.key = std::to_string(selected) + "|" + std::to_string(touch) + "|" + std::to_string(basemaps.size());
      const auto names = basemaps;
      const size_t chosen = selected;
      menu.paint = [=](Canvas& c, double ox, double oy) {
        panel(c, ox, oy, w, h, 8);
        for (size_t i = 0; i < names.size(); i++) {
          const double iy = oy + 4 + i * ih;
          const bool on = i == chosen;
          if (on) c.roundRect(ox + 4, iy, w - 8, ih, 6, rgba(47, 111, 219, 0.12));
          c.text(ox + 4 + 12, iy + ih / 2 + Canvas::ascent(px, on) * 0.36, names[i].name, px, on, kInk);
        }
      };
      for (size_t i = 0; i < basemaps.size(); i++) menu.hits.push_back({menu.x + 4, menu.y + 4 + i * ih, w - 8, ih, static_cast<double>(i)});
      menu.click = [this](size_t i) {
        menu_ = false;
        if (i < basemaps.size() && i != selected) {
          selected = i;
          if (actions.selectBasemap) actions.selectBasemap(i);
        }
      };
      elements_.push_back(std::move(menu));
    }
  }

  double left = H - m;
  if (!s.attribution.empty()) {
    const double size = touch ? 32 : 26;
    const double px = 11, line = 15;
    const bool open = credits_;
    std::vector<std::string> lines;
    double textWidth = 0;
    if (open) {
      lines = wrapText(s.attribution, px, std::max(60.0, W - 3 * m - b - size - 18));
      for (const auto& l : lines) textWidth = std::max(textWidth, Canvas::measure(l, px, false));
    }
    const double w = open ? size + 6 + textWidth + 12 : size;
    const double h = open ? std::max(size, line * static_cast<double>(lines.size()) + 12) : size;
    Element e;
    e.id = "attribution";
    e.x = m;
    e.y = left - h;
    e.w = w;
    e.h = h;
    e.pad = 2;
    std::string key = open ? "credits" : "info";
    for (const auto& l : lines) key += "|" + l;
    e.key = key + "|" + std::to_string(touch);
    e.paint = [=](Canvas& c, double ox, double oy) {
      c.roundRect(ox, oy, w, h, size / 2, rgba(255, 255, 255, open ? 0.92 : 0.82));
      const double icon = size * 0.66;
      strokeIcon(c, kInfo, iconBox(ox + (size - icon) / 2, oy + h - size + (size - icon) / 2, icon), 1.8 * icon / 24, kInk);
      for (size_t i = 0; i < lines.size(); i++)
        c.text(ox + size + 6, oy + 6 + line * static_cast<double>(i) + Canvas::ascent(px, false), lines[i], px, false, kInk);
    };
    e.hits.push_back({e.x, e.y, w, h, 0});
    e.click = [this](size_t) { credits_ = !credits_; };
    elements_.push_back(std::move(e));
  }
  if (!s.hud.empty()) {
    const double px = touch ? 12 : 11;
    const double line = px * 1.35;
    double widest = 0;
    std::string key = "hud";
    for (const auto& l : s.hud) {
      widest = std::max(widest, Canvas::measure(l, px, false));
      key += "|" + l;
    }
    const double w = std::min(widest + 16, W - 2 * m), h = line * static_cast<double>(s.hud.size()) + 10;
    const std::vector<std::string> lines = s.hud;
    Element e;
    e.id = "hud";
    e.x = m;
    e.y = m;
    e.w = w;
    e.h = h;
    e.pad = 2;
    e.key = key;
    e.paint = [=](Canvas& c, double ox, double oy) {
      c.roundRect(ox, oy, w, h, 6, rgba(10, 14, 18, 0.72));
      for (size_t i = 0; i < lines.size(); i++)
        c.text(ox + 8, oy + 5 + line * static_cast<double>(i) + Canvas::ascent(px, false), lines[i], px, false, rgba(240, 244, 248, 0.96));
    };
    elements_.push_back(std::move(e));
  }
}

const Overlay::Element* Overlay::hit(double x, double y, size_t* part) const {
  for (auto it = elements_.rbegin(); it != elements_.rend(); ++it)
    for (const auto& h : it->hits)
      if (x >= h[0] && y >= h[1] && x < h[0] + h[2] && y < h[1] + h[3]) {
        if (part) *part = static_cast<size_t>(h[4]);
        return &*it;
      }
  return nullptr;
}

bool Overlay::pointerDown(int id, double x, double y) {
  size_t part = 0;
  const Element* e = hit(x, y, &part);
  if (!e) {
    if (menu_) {
      menu_ = false;
      layout(state_);
    }
    return false;
  }
  pressed_[id] = {e->id, part};
  return true;
}

bool Overlay::pointerMove(int id, double, double) { return pressed_.contains(id); }

bool Overlay::pointerUp(int id, double x, double y) {
  auto it = pressed_.find(id);
  if (it == pressed_.end()) return false;
  const auto [elementId, part] = it->second;
  pressed_.erase(it);
  size_t now = 0;
  const Element* e = hit(x, y, &now);
  if (e && e->id == elementId && now == part && e->click) {
    auto click = e->click;
    click(part);
    layout(state_);
  }
  return true;
}

int Overlay::draw(const PassFormat& format, double width, double height) {
  if (width <= 0 || height <= 0) return 0;
  Gpu& gpu = Gpu::get();
  const double ratio = state_.pixelRatio;
  std::map<std::string, Cached> keep;
  int draws = 0;
  PipelineKey key;
  key.program = Program::Raster;
  key.color = format.color;
  key.depthFormat = format.depth;
  key.samples = format.samples;
  sg_apply_pipeline(gpu.pipeline(key));
  for (const auto& e : elements_) {
    const double ox = std::floor((e.x - e.pad) * ratio) / ratio, oy = std::floor((e.y - e.pad) * ratio) / ratio;
    const int pw = static_cast<int>(std::ceil((e.x + e.w + e.pad - ox) * ratio));
    const int ph = static_cast<int>(std::ceil((e.y + e.h + e.pad - oy) * ratio));
    const std::string full = e.key + "|" + std::to_string(pw) + "x" + std::to_string(ph) + "|" + std::to_string(ratio) + "|" +
                             std::to_string(e.x - ox) + "," + std::to_string(e.y - oy);
    auto found = cache_.find(e.id);
    Cached entry;
    if (found != cache_.end() && found->second.key == full && found->second.texture.valid()) {
      entry = std::move(found->second);
      cache_.erase(found);
    } else {
      if (found != cache_.end()) {
        found->second.texture.release();
        cache_.erase(found);
      }
      Canvas canvas(pw, ph, ratio);
      e.paint(canvas, e.x - ox, e.y - oy);
      const auto px = canvas.pixels();
      entry.texture = gpu.texture(pw, ph, SG_PIXELFORMAT_RGBA8, px.data(), px.size());
      entry.key = full;
      entry.pw = pw;
      entry.ph = ph;
    }
    sg_bindings b{};
    b.vertex_buffers[0] = gpu.quad();
    b.views[VIEW_mk_raster_tex] = entry.texture.view;
    b.samplers[SMP_mk_raster_smp] = gpu.sampler(false);
    sg_apply_bindings(&b);
    mk_raster_vs_params_t vs{};
    const double w = pw / ratio, h = ph / ratio;
    const float m[16] = {static_cast<float>(2 * w / width), 0, 0, 0, 0, static_cast<float>(-2 * h / height), 0, 0, 0, 0, 1, 0,
                         static_cast<float>(2 * ox / width - 1), static_cast<float>(1 - 2 * oy / height), 0, 1};
    std::memcpy(vs.u_matrix, m, sizeof m);
    vs.u_clip[0] = 0;
    vs.u_clip[1] = 0;
    vs.u_clip[2] = 1;
    vs.u_clip[3] = 1;
    sg_apply_uniforms(UB_mk_raster_vs_params, SG_RANGE(vs));
    mk_raster_fs_params_t fs{};
    fs.u_adjust[0] = 0;
    fs.u_adjust[1] = 1;
    fs.u_adjust[2] = 0;
    fs.u_adjust[3] = 1;
    fs.u_adjust2[0] = 1;
    fs.u_hue0[0] = 1;
    fs.u_hue1[1] = 1;
    fs.u_hue2[2] = 1;
    sg_apply_uniforms(UB_mk_raster_fs_params, SG_RANGE(fs));
    sg_draw(0, 6, 1);
    draws++;
    keep[e.id] = std::move(entry);
  }
  for (auto& [id, c] : cache_) c.texture.release();
  cache_ = std::move(keep);
  return draws;
}

void Overlay::resetGpu(bool release) {
  for (auto& [id, c] : cache_) {
    if (release) c.texture.release();
    c.texture = {};
  }
  cache_.clear();
}

}
