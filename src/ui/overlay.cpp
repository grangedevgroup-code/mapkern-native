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

const char* const kLogo[] = {
    "M230 127.5 229.2 117.8 227.6 108.1 224.8 98 220.9 88.4 216.4 79.7 211.1 71.4 205.3 64.2 198.5 57.1 191 50.8 182.5 44.9 174.4 40.4 165.4 36.5 156.7 33.6 147.1 31.5 137.4 30.3 127.5 30 117.8 30.8 108.1 32.4 98 35.2 88.4 39.1 79.7 43.6 71.4 49 64.2 54.7 57.1 61.5 50.8 69 44.9 77.5 40.4 85.6 36.5 94.6 33.6 103.3 31.5 112.9 30.3 122.6 30 132.4 30.8 142.2 32.4 151.9 35.2 161.9 39.1 171.6 42.4 178.2 46.5 185.1 68.2 215.5 230 129.5 230 128.2ZM166 124.7 165.8 128.2 165.2 131.5 164.4 134.7 163.3 137.8 162 140.6 160.1 143.8 158 146.7 155.5 149.5 152.7 152 149.8 154.1 146.6 156 143.4 157.4 140 158.6 136.8 159.3 132.9 159.9 129.6 160 125.8 159.8 122.5 159.2 119.3 158.4 116.2 157.3 113.4 156 110.2 154.1 107.3 152 104.5 149.5 102 146.7 99.9 143.8 98 140.6 96.6 137.4 95.4 134 94.7 130.8 94.1 126.9 94 123.6 94.2 119.8 94.8 116.5 95.6 113.3 96.7 110.2 98 107.4 99.9 104.2 102 101.3 104.5 98.5 107.3 96 110.2 93.9 113.4 92 116.6 90.6 120 89.4 123.2 88.7 127.1 88.1 130.4 88 134.2 88.2 137.5 88.8 140.7 89.6 143.8 90.7 146.6 92 149.8 93.9 152.7 96 155.5 98.5 158 101.3 160.1 104.2 162 107.4 163.4 110.6 164.6 114 165.3 117.2 165.9 121.1 166 124.4ZM180.5 231.3 155.3 183.9 75.8 226.2 130 302ZM211.4 188.1 214.5 183.5 217.6 178.2 220.4 172.8 222.4 168.3 224.8 161.9 226.4 156.7 228 150.1 228.9 144.8 166.8 177.8 189 219.5Z",
    "M301.6 218.2V113.8H334.5V121.5Q337.7 118.7 341.8 116.5Q351.1 111.6 362.7 111.6Q373.6 111.6 382.2 116.5Q388.7 120.2 393.2 125.8Q397.9 120.4 404.4 116.8Q414.3 111.6 426.5 111.6Q438.6 111.6 447.9 116.7Q457.3 121.7 462.8 131.1Q468.2 140.4 468.2 153.5V218.2H435.3V157.2Q435.3 149.2 430.5 144.8Q425.7 140.4 418.6 140.4Q413.6 140.4 409.8 142.5Q405.9 144.5 403.6 148.2Q401.4 151.8 401.4 157.2V218.2H368.5V157.2Q368.5 149.2 363.6 144.8Q358.8 140.4 351.7 140.4Q346.8 140.4 342.9 142.5Q339 144.5 336.8 148.2Q334.5 151.8 334.5 157.2V218.2ZM530 220.4Q515.6 220.4 504.3 213.3Q493 206.2 486.4 193.9Q479.9 181.7 479.9 166Q479.9 150.3 486.4 138Q493 125.8 504.3 118.7Q515.6 111.6 530 111.6Q540.5 111.6 549.1 115.7Q554.5 118.3 558.8 122.4V113.8H591V218.2H558.8V209.9Q554.5 213.7 549.1 216.3Q540.5 220.4 530 220.4ZM536.6 190.7Q547.2 190.7 553.6 183.7Q560.1 176.7 560.1 166Q560.1 158.7 557.2 153.1Q554.3 147.5 549 144.4Q543.7 141.3 536.8 141.3Q530 141.3 524.7 144.4Q519.4 147.5 516.3 153.1Q513.2 158.7 513.2 165.9Q513.2 173.1 516.2 178.7Q519.2 184.3 524.6 187.5Q530 190.7 536.6 190.7ZM671.4 220.4Q660.9 220.4 652.1 216.3Q647.4 214.2 643.7 211.1V262.3H611.2V113.8H644.1V121.1Q647.8 118.1 652.1 115.9Q660.9 111.6 671.4 111.6Q686.3 111.6 697.8 118.7Q709.3 125.8 715.8 138Q722.4 150.3 722.4 166Q722.4 181.7 715.8 193.9Q709.3 206.2 697.8 213.3Q686.3 220.4 671.4 220.4ZM665.4 190.7Q672.5 190.7 677.8 187.5Q683.1 184.3 686.1 178.7Q689.1 173.1 689.1 166Q689.1 158.7 686.1 153.1Q683.1 147.5 677.8 144.4Q672.5 141.3 665.6 141.3Q658.8 141.3 653.5 144.4Q648.2 147.5 645.2 153.1Q642.2 158.7 642.2 166Q642.2 173.1 645.1 178.7Q648 184.3 653.4 187.5Q658.8 190.7 665.4 190.7ZM803 218.2 769.1 167.9V218.2H736.2V62.2H769.1V160.4L802.8 113.8H839.6L801.3 163L841.7 218.2ZM902.2 220.6Q885.2 220.6 872 213.6Q858.7 206.6 851.2 194.2Q843.7 181.7 843.7 166Q843.7 150.3 851.1 137.9Q858.5 125.6 871.2 118.5Q883.9 111.4 899.8 111.4Q915.3 111.4 927.1 118.1Q938.9 124.7 945.7 136.5Q952.5 148.4 952.5 163.6Q952.5 166.4 952.2 169.5Q951.8 172.7 951 176.7L876.1 177Q876.8 179.5 877.9 181.7Q881.3 188.1 887.4 191.7Q893.6 195.2 901.9 195.2Q909.7 195.2 915.9 192.6Q922.2 189.9 926.9 184.5L944.9 202.5Q937.2 211.6 926.2 216.1Q915.3 220.6 902.2 220.6ZM876.1 154.4 921.7 154.2Q921.1 151.2 920 148.8Q917.4 142.8 912.4 139.7Q907.3 136.5 900 136.5Q892.3 136.5 886.6 140.1Q880.9 143.6 877.7 150.1Q876.8 152.2 876.1 154.4ZM965.4 218.2V113.8H998.3V121.7Q1008 111.6 1025.2 111.6Q1032.7 111.6 1038.4 113.9Q1044.1 116.1 1048.4 120.8L1028.8 145.6Q1026.7 143.2 1023.4 141.9Q1020.2 140.6 1016.1 140.6Q1008 140.6 1003.1 145.7Q998.3 150.7 998.3 160.8V218.2ZM1126.9 218.2V158.7Q1126.9 150.5 1121.8 145.5Q1116.8 140.4 1109 140.4Q1103.6 140.4 1099.6 142.7Q1095.5 144.9 1093.1 149.1Q1090.7 153.3 1090.7 158.7V218.2H1057.8V113.8H1090.7V121.9Q1094.2 118.9 1098.3 116.8Q1107.9 111.6 1120 111.6Q1131.6 111.6 1140.5 117.1Q1149.4 122.6 1154.6 131.6Q1159.8 140.6 1159.8 151.4V218.2Z"
};

const std::vector<std::string> kPlus{"M12 6v12", "M6 12h12"};
const std::vector<std::string> kMinus{"M6 12h12"};
const std::vector<std::string> kLayers{"M12 3l9 5-9 5-9-5z", "M3 12.5l9 5 9-5", "M3 17l9 5 9-5"};
const std::vector<std::string> kGlobe{"M12 3a9 9 0 1 0 0 18a9 9 0 1 0 0-18z", "M12 3c-3.2 3-3.2 15 0 18", "M12 3c3.2 3 3.2 15 0 18", "M3.5 9h17",
                                      "M3.5 15h17"};
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

std::string ellipsize(const std::string& text, double px, double width) {
  if (Canvas::measure(text, px, false) <= width) return text;
  std::string cut = text;
  while (!cut.empty() && Canvas::measure(cut + "\xE2\x80\xA6", px, false) > width) {
    cut.pop_back();
    while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) cut.pop_back();
  }
  return cut + "\xE2\x80\xA6";
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
  for (size_t i = 0; i < html.size(); i++) {
    const char ch = html[i];
    if (ch == '<') tag = true;
    else if (ch == '>') tag = false;
    else if (!tag) {
      if (ch == '&') {
        const size_t end = html.find(';', i);
        if (end != std::string::npos && end - i <= 8) {
          const std::string ent = html.substr(i + 1, end - i - 1);
          if (ent == "copy") out += "\xC2\xA9";
          else if (ent == "amp") out += "&";
          else if (ent == "lt") out += "<";
          else if (ent == "gt") out += ">";
          else if (ent == "nbsp") out += " ";
          else if (ent == "quot") out += "\"";
          else out += html.substr(i, end - i + 1);
          i = end;
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
  const double maxText = std::max(60.0, W - 2 * m - b - m);
  if (!s.attribution.empty()) {
    const double px = 11, line = 18;
    const std::string text = ellipsize(s.attribution, px, maxText - 16);
    const double w = Canvas::measure(text, px, false) + 16, h = line + 4;
    Element e;
    e.id = "attribution";
    e.x = m;
    e.y = left - h;
    e.w = w;
    e.h = h;
    e.pad = 1;
    e.key = text;
    e.paint = [=](Canvas& c, double ox, double oy) {
      c.roundRect(ox, oy, w, h, 6, rgba(255, 255, 255, 0.8));
      c.text(ox + 8, oy + 2 + (line + Canvas::ascent(px, false) * 0.72) / 2, text, px, false, kInk);
    };
    elements_.push_back(std::move(e));
    left -= h + m;
  }
  {
    const double w = 94, h = 26;
    Element e;
    e.id = "logo";
    e.x = m;
    e.y = left - h;
    e.w = w;
    e.h = h;
    e.pad = 4;
    e.key = "logo";
    e.paint = [=](Canvas& c, double ox, double oy) {
      const double k = std::min(w / 1192, h / 332);
      const Affine a{k, 0, 0, k, ox + (w - 1192 * k) / 2, oy + (h - 332 * k) / 2};
      Canvas layer(c.width(), c.height(), c.scale());
      for (const char* d : kLogo) {
        layer.stroke(shape(d), a, 44 * k, kWhite);
        layer.fill(shape(d), a, kWhite);
      }
      for (const char* d : kLogo) layer.fill(shape(d), a, kInk);
      const auto px = layer.pixels();
      for (int y = 0; y < c.height(); y++)
        for (int x = 0; x < c.width(); x++) {
          const uint8_t* p = &px[(static_cast<size_t>(y) * c.width() + x) * 4];
          if (p[3] == 0) continue;
          const double alpha = p[3] / 255.0;
          c.rect(x / c.scale(), y / c.scale(), 1 / c.scale(), 1 / c.scale(), {p[0] / 255.0 / alpha, p[1] / 255.0 / alpha, p[2] / 255.0 / alpha, alpha * 0.6});
        }
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
