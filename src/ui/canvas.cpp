#include "ui/canvas.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "core/utf8.hpp"
#include "text/glyphs.hpp"

namespace mapkern::ui {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct PathReader {
  const std::string& s;
  size_t i = 0;
  void skip() {
    while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) i++;
  }
  bool number(double& out) {
    skip();
    if (i >= s.size()) return false;
    const char c = s[i];
    if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.')) return false;
    const char* begin = s.c_str() + i;
    char* end = nullptr;
    out = std::strtod(begin, &end);
    if (end == begin) return false;
    i += static_cast<size_t>(end - begin);
    return true;
  }
  bool flag(double& out) {
    skip();
    if (i < s.size() && (s[i] == '0' || s[i] == '1')) {
      out = s[i] == '1' ? 1 : 0;
      i++;
      return true;
    }
    return number(out);
  }
};

void arcTo(Polyline& line, double x1, double y1, double rx, double ry, double phi, bool large, bool sweep, double x2, double y2) {
  if (rx == 0 || ry == 0) {
    line.push_back({x2, y2});
    return;
  }
  rx = std::abs(rx);
  ry = std::abs(ry);
  const double cp = std::cos(phi), sp = std::sin(phi);
  const double dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
  const double xp = cp * dx + sp * dy, yp = -sp * dx + cp * dy;
  double lambda = xp * xp / (rx * rx) + yp * yp / (ry * ry);
  if (lambda > 1) {
    rx *= std::sqrt(lambda);
    ry *= std::sqrt(lambda);
  }
  const double num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp;
  const double den = rx * rx * yp * yp + ry * ry * xp * xp;
  double k = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0;
  if (large == sweep) k = -k;
  const double cxp = k * rx * yp / ry, cyp = -k * ry * xp / rx;
  const double cx = cp * cxp - sp * cyp + (x1 + x2) / 2, cy = sp * cxp + cp * cyp + (y1 + y2) / 2;
  auto angle = [](double ux, double uy, double vx, double vy) {
    const double a = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    return a;
  };
  const double t1 = angle(1, 0, (xp - cxp) / rx, (yp - cyp) / ry);
  double dt = angle((xp - cxp) / rx, (yp - cyp) / ry, (-xp - cxp) / rx, (-yp - cyp) / ry);
  if (!sweep && dt > 0) dt -= 2 * kPi;
  if (sweep && dt < 0) dt += 2 * kPi;
  const int steps = std::max(4, static_cast<int>(std::ceil(std::abs(dt) / (kPi / 16))));
  for (int s = 1; s <= steps; s++) {
    const double t = t1 + dt * s / steps;
    const double ex = rx * std::cos(t), ey = ry * std::sin(t);
    line.push_back({cp * ex - sp * ey + cx, sp * ex + cp * ey + cy});
  }
}

double segmentDistance(double px, double py, double ax, double ay, double bx, double by) {
  const double dx = bx - ax, dy = by - ay;
  const double len = dx * dx + dy * dy;
  double t = len > 0 ? ((px - ax) * dx + (py - ay) * dy) / len : 0;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(px - (ax + dx * t), py - (ay + dy * t));
}

double roundRectDistance(double px, double py, double x, double y, double w, double h, double r) {
  const double cx = x + w / 2, cy = y + h / 2;
  const double qx = std::abs(px - cx) - (w / 2 - r), qy = std::abs(py - cy) - (h / 2 - r);
  return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
}

}

Color rgba(int r, int g, int b, double a) { return {r / 255.0, g / 255.0, b / 255.0, a}; }

Shape parsePath(const std::string& d) {
  Shape shape;
  PathReader r{d};
  double x = 0, y = 0, sx = 0, sy = 0;
  char cmd = 0;
  Polyline current;
  auto flush = [&](bool closed) {
    if (current.size() >= 2) {
      shape.rings.push_back(current);
      shape.closed.push_back(closed);
    }
    current.clear();
  };
  while (true) {
    r.skip();
    if (r.i >= d.size()) break;
    const char c = d[r.i];
    if (std::isalpha(static_cast<unsigned char>(c))) {
      cmd = c;
      r.i++;
      if (cmd == 'z' || cmd == 'Z') {
        if (!current.empty()) current.push_back({sx, sy});
        flush(true);
        x = sx;
        y = sy;
        continue;
      }
    }
    const bool rel = std::islower(static_cast<unsigned char>(cmd));
    const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
    double a[7];
    if (up == 'M') {
      if (!r.number(a[0]) || !r.number(a[1])) break;
      flush(false);
      x = rel ? x + a[0] : a[0];
      y = rel ? y + a[1] : a[1];
      sx = x;
      sy = y;
      current.push_back({x, y});
      cmd = rel ? 'l' : 'L';
    } else if (up == 'L') {
      if (!r.number(a[0]) || !r.number(a[1])) break;
      x = rel ? x + a[0] : a[0];
      y = rel ? y + a[1] : a[1];
      current.push_back({x, y});
    } else if (up == 'H') {
      if (!r.number(a[0])) break;
      x = rel ? x + a[0] : a[0];
      current.push_back({x, y});
    } else if (up == 'V') {
      if (!r.number(a[0])) break;
      y = rel ? y + a[0] : a[0];
      current.push_back({x, y});
    } else if (up == 'C' || up == 'Q') {
      const int n = up == 'C' ? 6 : 4;
      bool ok = true;
      for (int k = 0; k < n; k++) ok = ok && r.number(a[k]);
      if (!ok) break;
      if (rel)
        for (int k = 0; k < n; k += 2) {
          a[k] += x;
          a[k + 1] += y;
        }
      const double x0 = x, y0 = y;
      for (int s = 1; s <= 16; s++) {
        const double t = s / 16.0, u = 1 - t;
        if (up == 'C') {
          current.push_back({u * u * u * x0 + 3 * u * u * t * a[0] + 3 * u * t * t * a[2] + t * t * t * a[4],
                             u * u * u * y0 + 3 * u * u * t * a[1] + 3 * u * t * t * a[3] + t * t * t * a[5]});
        } else {
          current.push_back({u * u * x0 + 2 * u * t * a[0] + t * t * a[2], u * u * y0 + 2 * u * t * a[1] + t * t * a[3]});
        }
      }
      x = a[n - 2];
      y = a[n - 1];
    } else if (up == 'A') {
      if (!r.number(a[0]) || !r.number(a[1]) || !r.number(a[2]) || !r.flag(a[3]) || !r.flag(a[4]) || !r.number(a[5]) || !r.number(a[6])) break;
      const double ex = rel ? x + a[5] : a[5], ey = rel ? y + a[6] : a[6];
      arcTo(current, x, y, a[0], a[1], a[2] * kPi / 180, a[3] != 0, a[4] != 0, ex, ey);
      x = ex;
      y = ey;
    } else {
      break;
    }
  }
  flush(false);
  return shape;
}

Canvas::Canvas(int width, int height, double scale)
    : width_(std::max(1, width)), height_(std::max(1, height)), scale_(scale), data_(static_cast<size_t>(width_) * height_ * 4, 0.0f) {}

void Canvas::clear() { std::fill(data_.begin(), data_.end(), 0.0f); }

void Canvas::blend(int x, int y, const Color& c, double coverage) {
  if (x < 0 || y < 0 || x >= width_ || y >= height_ || coverage <= 0) return;
  const double a = c[3] * std::min(1.0, coverage);
  float* p = &data_[(static_cast<size_t>(y) * width_ + x) * 4];
  const double k = 1 - a;
  p[0] = static_cast<float>(c[0] * a + p[0] * k);
  p[1] = static_cast<float>(c[1] * a + p[1] * k);
  p[2] = static_cast<float>(c[2] * a + p[2] * k);
  p[3] = static_cast<float>(a + p[3] * k);
}

void Canvas::roundRect(double x, double y, double w, double h, double r, const Color& color) { roundRectCorners(x, y, w, h, {r, r, r, r}, color); }

void Canvas::roundRectCorners(double x, double y, double w, double h, const std::array<double, 4>& radii, const Color& color) {
  const double s = scale_;
  const double X = x * s, Y = y * s, W = w * s, H = h * s;
  const int x0 = static_cast<int>(std::floor(X)), x1 = static_cast<int>(std::ceil(X + W));
  const int y0 = static_cast<int>(std::floor(Y)), y1 = static_cast<int>(std::ceil(Y + H));
  for (int py = y0; py < y1; py++)
    for (int px = x0; px < x1; px++) {
      const double cx = px + 0.5, cy = py + 0.5;
      const bool right = cx > X + W / 2, bottom = cy > Y + H / 2;
      const double r = radii[bottom ? (right ? 2 : 3) : (right ? 1 : 0)] * s;
      const double d = roundRectDistance(cx, cy, X, Y, W, H, std::min(r, std::min(W, H) / 2));
      blend(px, py, color, std::clamp(0.5 - d, 0.0, 1.0));
    }
}

void Canvas::shadow(double x, double y, double w, double h, double r, double blur, double dy, double alpha) {
  const double s = scale_;
  const double X = x * s, Y = (y + dy) * s, W = w * s, H = h * s, B = std::max(0.5, blur * s);
  const int x0 = static_cast<int>(std::floor(X - B * 2)), x1 = static_cast<int>(std::ceil(X + W + B * 2));
  const int y0 = static_cast<int>(std::floor(Y - B * 2)), y1 = static_cast<int>(std::ceil(Y + H + B * 2));
  const Color ink = rgba(16, 25, 29, alpha);
  for (int py = y0; py < y1; py++)
    for (int px = x0; px < x1; px++) {
      const double d = roundRectDistance(px + 0.5, py + 0.5, X, Y, W, H, std::min(r * s, std::min(W, H) / 2));
      blend(px, py, ink, 0.5 * std::erfc(d / (B * 0.7071)));
    }
}

void Canvas::rect(double x, double y, double w, double h, const Color& color) { roundRect(x, y, w, h, 0, color); }

void Canvas::fill(const Shape& shape, const Affine& m, const Color& color) {
  std::vector<Polyline> rings;
  double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
  for (const auto& ring : shape.rings) {
    Polyline out;
    for (const auto& p : ring) {
      auto q = m.apply(p[0], p[1]);
      q[0] *= scale_;
      q[1] *= scale_;
      minX = std::min(minX, q[0]);
      minY = std::min(minY, q[1]);
      maxX = std::max(maxX, q[0]);
      maxY = std::max(maxY, q[1]);
      out.push_back(q);
    }
    if (out.size() >= 3) rings.push_back(std::move(out));
  }
  if (rings.empty()) return;
  const int x0 = std::max(0, static_cast<int>(std::floor(minX))), x1 = std::min(width_, static_cast<int>(std::ceil(maxX)) + 1);
  const int y0 = std::max(0, static_cast<int>(std::floor(minY))), y1 = std::min(height_, static_cast<int>(std::ceil(maxY)) + 1);
  constexpr int kSub = 4;
  for (int py = y0; py < y1; py++)
    for (int px = x0; px < x1; px++) {
      int inside = 0;
      for (int sy = 0; sy < kSub; sy++)
        for (int sx = 0; sx < kSub; sx++) {
          const double qx = px + (sx + 0.5) / kSub, qy = py + (sy + 0.5) / kSub;
          int winding = 0;
          for (const auto& ring : rings)
            for (size_t i = 0; i < ring.size(); i++) {
              const auto& a = ring[i];
              const auto& b = ring[(i + 1) % ring.size()];
              if (a[1] <= qy) {
                if (b[1] > qy && (b[0] - a[0]) * (qy - a[1]) - (qx - a[0]) * (b[1] - a[1]) > 0) winding++;
              } else if (b[1] <= qy && (b[0] - a[0]) * (qy - a[1]) - (qx - a[0]) * (b[1] - a[1]) < 0) {
                winding--;
              }
            }
          if (winding != 0) inside++;
        }
      blend(px, py, color, static_cast<double>(inside) / (kSub * kSub));
    }
}

void Canvas::stroke(const Shape& shape, const Affine& m, double width, const Color& color) {
  std::vector<std::array<double, 4>> segments;
  double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
  for (const auto& ring : shape.rings) {
    for (size_t i = 0; i + 1 < ring.size(); i++) {
      auto a = m.apply(ring[i][0], ring[i][1]);
      auto b = m.apply(ring[i + 1][0], ring[i + 1][1]);
      segments.push_back({a[0] * scale_, a[1] * scale_, b[0] * scale_, b[1] * scale_});
      minX = std::min({minX, a[0] * scale_, b[0] * scale_});
      maxX = std::max({maxX, a[0] * scale_, b[0] * scale_});
      minY = std::min({minY, a[1] * scale_, b[1] * scale_});
      maxY = std::max({maxY, a[1] * scale_, b[1] * scale_});
    }
  }
  if (segments.empty()) return;
  const double half = width * scale_ / 2;
  const int x0 = std::max(0, static_cast<int>(std::floor(minX - half - 1))), x1 = std::min(width_, static_cast<int>(std::ceil(maxX + half + 1)));
  const int y0 = std::max(0, static_cast<int>(std::floor(minY - half - 1))), y1 = std::min(height_, static_cast<int>(std::ceil(maxY + half + 1)));
  for (int py = y0; py < y1; py++)
    for (int px = x0; px < x1; px++) {
      double d = 1e9;
      for (const auto& s : segments) d = std::min(d, segmentDistance(px + 0.5, py + 0.5, s[0], s[1], s[2], s[3]));
      blend(px, py, color, std::clamp(half - d + 0.5, 0.0, 1.0));
    }
}

double Canvas::text(double x, double baseline, const std::string& text, double px, bool bold, const Color& color) {
  const auto cps = decodeUtf8(text);
  double pen = x * scale_;
  const double base = baseline * scale_;
  std::vector<uint8_t> bmp;
  for (size_t i = 0; i < cps.size(); i++) {
    int w = 0, h = 0, l = 0, t = 0;
    double adv = 0, kern = 0;
    if (!LocalFonts::get().bitmap(bold, cps[i], px * scale_, bmp, w, h, l, t, adv, kern, i + 1 < cps.size() ? cps[i + 1] : 0)) break;
    const int ox = static_cast<int>(std::lround(pen)) + l, oy = static_cast<int>(std::lround(base)) + t;
    for (int yy = 0; yy < h; yy++)
      for (int xx = 0; xx < w; xx++) blend(ox + xx, oy + yy, color, bmp[static_cast<size_t>(yy) * w + xx] / 255.0);
    pen += adv + kern;
  }
  return pen / scale_ - x;
}

double Canvas::measure(const std::string& text, double px, bool bold) {
  const auto cps = decodeUtf8(text);
  double total = 0;
  std::vector<uint8_t> bmp;
  for (size_t i = 0; i < cps.size(); i++) {
    int w = 0, h = 0, l = 0, t = 0;
    double adv = 0, kern = 0;
    if (!LocalFonts::get().bitmap(bold, cps[i], px * 4, bmp, w, h, l, t, adv, kern, i + 1 < cps.size() ? cps[i + 1] : 0)) break;
    total += (adv + kern) / 4;
  }
  return total;
}

double Canvas::ascent(double px, bool bold) { return LocalFonts::get().ascent(bold, px); }

std::vector<uint8_t> Canvas::pixels() const {
  std::vector<uint8_t> out(data_.size());
  for (size_t i = 0; i < data_.size(); i++) out[i] = static_cast<uint8_t>(std::clamp(std::lround(data_[i] * 255.0f), 0L, 255L));
  return out;
}

}
