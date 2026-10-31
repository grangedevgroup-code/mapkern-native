#include "worker/buckets.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "geo/tessellate.hpp"

namespace mapkern {

namespace {

constexpr double kMaxJointScale = 4;
constexpr double kShiftSteps = 126;
constexpr int8_t kCornerX[4] = {-127, 127, 127, -127};
constexpr int8_t kCornerY[4] = {-127, -127, 127, 127};

int16_t clamp16(double v) { return static_cast<int16_t>(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
int8_t clamp8(double v) { return static_cast<int8_t>(v < -127 ? -127 : v > 127 ? 127 : v); }

int16_t clamp16i(int32_t v) { return static_cast<int16_t>(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

double jsRound(double v) { return std::floor(v + 0.5); }

bool inside(int edge, double x, double y, double x0, double y0, double x1, double y1) {
  switch (edge) {
    case 0: return x >= x0;
    case 1: return x <= x1;
    case 2: return y >= y0;
    default: return y <= y1;
  }
}

void intersect(int edge, double ax, double ay, double bx, double by, double x0, double y0, double x1, double y1, std::vector<double>& out) {
  double t;
  switch (edge) {
    case 0:
      t = (x0 - ax) / (bx - ax);
      out.push_back(x0);
      out.push_back(ay + (by - ay) * t);
      break;
    case 1:
      t = (x1 - ax) / (bx - ax);
      out.push_back(x1);
      out.push_back(ay + (by - ay) * t);
      break;
    case 2:
      t = (y0 - ay) / (by - ay);
      out.push_back(ax + (bx - ax) * t);
      out.push_back(y0);
      break;
    default:
      t = (y1 - ay) / (by - ay);
      out.push_back(ax + (bx - ax) * t);
      out.push_back(y1);
  }
}

double ringAreaD(const std::vector<int32_t>& ring) { return ringArea(ring); }

}

std::vector<double> clipRing(const std::vector<double>& ring, double x0, double y0, double x1, double y1) {
  std::vector<double> pts = ring;
  if (pts.size() >= 4 && pts[0] == pts[pts.size() - 2] && pts[1] == pts[pts.size() - 1]) pts.resize(pts.size() - 2);
  for (int edge = 0; edge < 4; edge++) {
    if (pts.size() < 6) return {};
    std::vector<double> out;
    const size_t n = pts.size();
    for (size_t i = 0; i < n; i += 2) {
      const size_t a = (i + n - 2) % n;
      const double ax = pts[a], ay = pts[a + 1], bx = pts[i], by = pts[i + 1];
      const bool ina = inside(edge, ax, ay, x0, y0, x1, y1);
      const bool inb = inside(edge, bx, by, x0, y0, x1, y1);
      if (inb) {
        if (!ina) intersect(edge, ax, ay, bx, by, x0, y0, x1, y1, out);
        out.push_back(bx);
        out.push_back(by);
      } else if (ina) {
        intersect(edge, ax, ay, bx, by, x0, y0, x1, y1, out);
      }
    }
    pts = std::move(out);
  }
  if (pts.size() < 6) return {};
  pts.push_back(pts[0]);
  pts.push_back(pts[1]);
  return pts;
}

void FillBuilder::addPolygon(const std::vector<Ring>& rings, uint32_t feature) {
  coords_.clear();
  holes_.clear();
  starts_.clear();
  for (size_t r = 0; r < rings.size(); r++) {
    const Ring& ring = rings[r];
    size_t n = ring.size() / 2;
    if (n > 1 && ring[0] == ring[ring.size() - 2] && ring[1] == ring[ring.size() - 1]) n--;
    if (n < 3) {
      if (r == 0) return;
      continue;
    }
    if (r > 0) holes_.push_back(static_cast<uint32_t>(coords_.size() / 2));
    starts_.push_back(static_cast<uint32_t>(coords_.size() / 2));
    for (size_t i = 0; i < n; i++) {
      coords_.push_back(ring[i * 2]);
      coords_.push_back(ring[i * 2 + 1]);
    }
  }
  const uint32_t base = vertexCount;
  starts_.push_back(static_cast<uint32_t>(coords_.size() / 2));
  for (size_t r = 0; r + 1 < starts_.size(); r++) {
    const uint32_t start = starts_[r], end = starts_[r + 1];
    for (uint32_t i = start; i < end; i++) {
      edges.push_back(base + i);
      edges.push_back(base + (i + 1 < end ? i + 1 : start));
    }
  }
  const size_t count = coords_.size() / 2;
  vertices.reserve(count * kFillStride);
  for (size_t i = 0; i < count; i++) {
    vertices.put(clamp16(coords_[i * 2]));
    vertices.put(clamp16(coords_[i * 2 + 1]));
    vertices.put(feature);
  }
  vertexCount += static_cast<uint32_t>(count);
  tris_.clear();
  triangulate(coords_, holes_, tris_);
  for (size_t t = 0; t + 2 < tris_.size(); t += 3) {
    indices.push_back(base + tris_[t]);
    indices.push_back(base + tris_[t + 1]);
    indices.push_back(base + tris_[t + 2]);
  }
}

void LineBuilder::jointShift(size_t s0, size_t s1) {
  const double n0x = -dirs_[s0 * 2 + 1], n0y = dirs_[s0 * 2];
  const double n1x = -dirs_[s1 * 2 + 1], n1y = dirs_[s1 * 2];
  const double mx = n0x + n1x, my = n0y + n1y;
  const double len2 = mx * mx + my * my;
  if (len2 < 1e-12) {
    shift(n0x, n0y);
    return;
  }
  double scale = 2 / len2;
  const double length = std::sqrt(len2) * scale;
  if (length > kMaxJointScale) scale *= kMaxJointScale / length;
  shift(mx * scale, my * scale);
}

uint32_t LineBuilder::vertex(double x, double y, double ex, double ey, double sx, double sy, bool square, double dist, uint32_t feature) {
  vertices.put(clamp16(x));
  vertices.put(clamp16(y));
  vertices.put(clamp16(jsRound(ex * kExtrudeScale)));
  vertices.put(clamp16(jsRound(ey * kExtrudeScale)));
  const double length = std::min(kMaxJointScale, std::hypot(shiftX_, shiftY_));
  const int steps = static_cast<int>(jsRound(length / kMaxJointScale * kShiftSteps));
  const int turn = static_cast<int>(jsRound(std::atan2(shiftY_, shiftX_) / std::numbers::pi * 128));
  vertices.put(clamp8(jsRound(sx * 63.5)));
  vertices.put(clamp8(jsRound(sy * 63.5)));
  vertices.put(static_cast<int8_t>(square ? -steps - 1 : steps));
  vertices.put(static_cast<int8_t>(turn >= 128 ? -128 : turn));
  vertices.put(static_cast<float>(dist));
  vertices.put(feature);
  return vertexCount++;
}

void LineBuilder::addLine(const Ring& points, uint32_t feature, bool closedHint) {
  pts_.clear();
  for (size_t i = 0; i + 1 < points.size(); i += 2) {
    const double x = points[i], y = points[i + 1];
    if (pts_.size() >= 2 && pts_[pts_.size() - 2] == x && pts_[pts_.size() - 1] == y) continue;
    pts_.push_back(x);
    pts_.push_back(y);
  }
  const size_t n = pts_.size();
  size_t count = n / 2;
  if (count < 2) return;
  const bool loops = pts_[0] == pts_[n - 2] && pts_[1] == pts_[n - 1];
  bool closed = closedHint || (count > 2 && loops);
  if (closed && loops) count--;
  if (count < 2) return;
  if (closed && count < 3) closed = false;
  const size_t segments = closed ? count : count - 1;
  dirs_.assign(segments * 2, 0);
  lens_.assign(segments, 0);
  for (size_t s = 0; s < segments; s++) {
    const size_t a = s, b = (s + 1) % count;
    const double dx = pts_[b * 2] - pts_[a * 2];
    const double dy = pts_[b * 2 + 1] - pts_[a * 2 + 1];
    const double len = std::hypot(dx, dy);
    lens_[s] = len;
    dirs_[s * 2] = len > 0 ? dx / len : 1;
    dirs_[s * 2 + 1] = len > 0 ? dy / len : 0;
  }
  int64_t pairL = -1, pairR = -1, firstL = -1, firstR = -1;
  double dist = 0;
  if (closed && simple(segments - 1, 0)) {
    jointShift(segments - 1, 0);
    miterPair(pts_[0], pts_[1], segments - 1, 0, 0, feature);
    pairL = pairLeft_;
    pairR = pairRight_;
    firstL = pairL;
    firstR = pairR;
  }
  for (size_t s = 0; s < segments; s++) {
    const size_t a = s, b = (s + 1) % count;
    const double ax = pts_[a * 2], ay = pts_[a * 2 + 1], bx = pts_[b * 2], by = pts_[b * 2 + 1];
    const double dx = dirs_[s * 2], dy = dirs_[s * 2 + 1];
    const double nx = -dy, ny = dx;
    const double d0 = dist, d1 = dist + lens_[s];
    int64_t startL = pairL, startR = pairR;
    if (startL < 0) {
      if (s == 0 && !closed) shift(nx, ny);
      else jointShift(s == 0 ? segments - 1 : s - 1, s);
      startL = vertex(ax, ay, nx, ny, 0, 1, false, d0, feature);
      startR = vertex(ax, ay, -nx, -ny, 0, -1, false, d0, feature);
      if (s == 0 && !closed) cap(ax, ay, -dx, -dy, d0, feature);
    }
    const bool last = s == segments - 1;
    const bool hasJoin = closed || !last;
    const size_t next = (s + 1) % segments;
    int64_t endL, endR;
    if (hasJoin && last && closed && firstL >= 0) {
      endL = firstL;
      endR = firstR;
      pairL = pairR = -1;
    } else if (hasJoin && simple(s, next)) {
      jointShift(s, next);
      miterPair(bx, by, s, next, d1, feature);
      endL = pairLeft_;
      endR = pairRight_;
      pairL = endL;
      pairR = endR;
    } else {
      if (hasJoin) jointShift(s, next);
      else shift(nx, ny);
      endL = vertex(bx, by, nx, ny, 0, 1, false, d1, feature);
      endR = vertex(bx, by, -nx, -ny, 0, -1, false, d1, feature);
      pairL = pairR = -1;
      if (hasJoin) join(bx, by, dx, dy, dirs_[next * 2], dirs_[next * 2 + 1], d1, feature);
      else if (!closed) cap(bx, by, dx, dy, d1, feature);
    }
    tri(static_cast<uint32_t>(startL), static_cast<uint32_t>(startR), static_cast<uint32_t>(endL));
    tri(static_cast<uint32_t>(startR), static_cast<uint32_t>(endR), static_cast<uint32_t>(endL));
    dist = d1;
  }
}

bool LineBuilder::simple(size_t s0, size_t s1) const {
  const double dot = dirs_[s0 * 2] * dirs_[s1 * 2] + dirs_[s0 * 2 + 1] * dirs_[s1 * 2 + 1];
  if (options_.join == "miter") {
    const double cosHalf = std::sqrt(std::max(0.0, (1 + dot) / 2));
    return cosHalf > 1e-6 && 1 / cosHalf <= options_.miterLimit;
  }
  return dot >= options_.roundLimit;
}

void LineBuilder::miterPair(double x, double y, size_t s0, size_t s1, double dist, uint32_t feature) {
  const double n0x = -dirs_[s0 * 2 + 1], n0y = dirs_[s0 * 2];
  const double n1x = -dirs_[s1 * 2 + 1], n1y = dirs_[s1 * 2];
  double mx = n0x + n1x, my = n0y + n1y;
  const double len = std::hypot(mx, my);
  if (len < 1e-9) {
    mx = n0x;
    my = n0y;
  } else {
    const double scale = 1 / std::max(len / 2, 1e-6) / len;
    mx *= scale;
    my *= scale;
  }
  pairLeft_ = vertex(x, y, mx, my, 0, 1, false, dist, feature);
  pairRight_ = vertex(x, y, -mx, -my, 0, -1, false, dist, feature);
}

void LineBuilder::cap(double x, double y, double dx, double dy, double dist, uint32_t feature) {
  if (options_.cap == "butt") return;
  const double nx = -dy, ny = dx;
  const bool square = options_.cap == "square";
  const uint32_t a = vertex(x, y, nx, ny, 0, 1, square, dist, feature);
  const uint32_t b = vertex(x, y, -nx, -ny, 0, -1, square, dist, feature);
  const uint32_t c = vertex(x, y, nx + dx, ny + dy, 1, 1, square, dist, feature);
  const uint32_t d = vertex(x, y, -nx + dx, -ny + dy, 1, -1, square, dist, feature);
  tri(a, b, c);
  tri(b, d, c);
}

void LineBuilder::join(double x, double y, double d0x, double d0y, double d1x, double d1y, double dist, uint32_t feature) {
  const double turn = d0x * d1y - d0y * d1x;
  const double dot = d0x * d1x + d0y * d1y;
  if (std::abs(turn) < 1e-9 && dot > 0) return;
  const double side = turn > 0 ? -1 : 1;
  const double o0x = -d0y * side, o0y = d0x * side, o1x = -d1y * side, o1y = d1x * side;
  std::string kind = options_.join;
  const double mx = o0x + o1x, my = o0y + o1y;
  const double mlen = std::hypot(mx, my);
  const double miter = mlen > 1e-9 ? 1 / std::max(mlen / 2, 1e-6) : INFINITY;
  if (kind == "miter" && miter > options_.miterLimit) kind = "bevel";
  const uint32_t center = vertex(x, y, 0, 0, 0, 0, false, dist, feature);
  if (kind == "bevel") {
    const uint32_t a = vertex(x, y, o0x, o0y, 0, 1, false, dist, feature);
    const uint32_t b = vertex(x, y, o1x, o1y, 0, 1, false, dist, feature);
    tri(center, a, b);
    return;
  }
  if (kind == "miter") {
    const uint32_t a = vertex(x, y, o0x, o0y, 0, 1, false, dist, feature);
    const uint32_t b = vertex(x, y, o1x, o1y, 0, 1, false, dist, feature);
    const uint32_t m = vertex(x, y, mx / mlen * miter, my / mlen * miter, 0, 1, false, dist, feature);
    tri(center, a, m);
    tri(center, m, b);
    return;
  }
  const double start = std::atan2(o0y, o0x);
  double sweep = std::atan2(o1y, o1x) - start;
  while (sweep > std::numbers::pi) sweep -= 2 * std::numbers::pi;
  while (sweep < -std::numbers::pi) sweep += 2 * std::numbers::pi;
  const int steps = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / (std::numbers::pi / 2))));
  const double step = sweep / steps;
  const double radius = 1 / std::cos(std::abs(step) / 2);
  uint32_t prev = vertex(x, y, o0x, o0y, o0x, o0y, false, dist, feature);
  for (int i = 0; i < steps; i++) {
    const double angle = start + step * (i + 0.5);
    const double ex = std::cos(angle) * radius, ey = std::sin(angle) * radius;
    const uint32_t cur = vertex(x, y, ex, ey, ex, ey, false, dist, feature);
    tri(center, prev, cur);
    prev = cur;
  }
  const uint32_t end = vertex(x, y, o1x, o1y, o1x, o1y, false, dist, feature);
  tri(center, prev, end);
}

void ExtrusionBuilder::addPolygon(const std::vector<Ring>& polygon, uint32_t feature) {
  std::vector<Ring> rings = polygon;
  bool outside = false;
  for (const auto& ring : rings) {
    for (int32_t v : ring)
      if (v < 0 || v > static_cast<int32_t>(extent_)) {
        outside = true;
        break;
      }
    if (outside) break;
  }
  if (outside) {
    std::vector<Ring> out;
    for (size_t r = 0; r < rings.size(); r++) {
      std::vector<double> d(rings[r].begin(), rings[r].end());
      const auto clipped = clipRing(d, 0, 0, extent_, extent_);
      Ring ring;
      for (double v : clipped) ring.push_back(static_cast<int32_t>(jsRound(v)));
      if (ring.empty() || std::abs(ringAreaD(ring)) < 1) {
        if (r == 0) return;
        continue;
      }
      out.push_back(std::move(ring));
    }
    rings = std::move(out);
  }
  if (rings.empty()) return;
  const int outward = ringArea(rings[0]) >= 0 ? 1 : -1;
  coords_.clear();
  holes_.clear();
  for (size_t r = 0; r < rings.size(); r++) {
    const Ring& ring = rings[r];
    size_t n = ring.size() / 2;
    if (n > 1 && ring[0] == ring[ring.size() - 2] && ring[1] == ring[ring.size() - 1]) n--;
    if (n < 3) {
      if (r == 0) return;
      continue;
    }
    if (r > 0) holes_.push_back(static_cast<uint32_t>(coords_.size() / 2));
    for (size_t i = 0; i < n; i++) {
      coords_.push_back(ring[i * 2]);
      coords_.push_back(ring[i * 2 + 1]);
    }
    addWalls(ring, n, outward, feature);
  }
  const uint32_t base = vertexCount;
  const size_t count = coords_.size() / 2;
  for (size_t i = 0; i < count; i++)
    vertex(static_cast<int32_t>(coords_[i * 2]), static_cast<int32_t>(coords_[i * 2 + 1]), 0, 0, 127, true, feature);
  tris_.clear();
  triangulate(coords_, holes_, tris_);
  for (size_t t = 0; t + 2 < tris_.size(); t += 3) {
    indices.push_back(base + tris_[t]);
    indices.push_back(base + tris_[t + 1]);
    indices.push_back(base + tris_[t + 2]);
  }
}

void ExtrusionBuilder::addWalls(const Ring& ring, size_t n, int outward, uint32_t feature) {
  const int32_t e = static_cast<int32_t>(extent_);
  for (size_t i = 0; i < n; i++) {
    const int32_t ax = ring[i * 2], ay = ring[i * 2 + 1];
    const size_t j = (i + 1) % n;
    const int32_t bx = ring[j * 2], by = ring[j * 2 + 1];
    if ((ax <= 0 && bx <= 0) || (ax >= e && bx >= e) || (ay <= 0 && by <= 0) || (ay >= e && by >= e)) continue;
    const double dx = bx - ax, dy = by - ay;
    const double len = std::hypot(dx, dy);
    if (len == 0) continue;
    const int nx = static_cast<int>(jsRound(dy / len * outward * 127));
    const int ny = static_cast<int>(jsRound(-dx / len * outward * 127));
    const uint32_t v = vertexCount;
    vertex(ax, ay, nx, ny, 0, false, feature);
    vertex(ax, ay, nx, ny, 0, true, feature);
    vertex(bx, by, nx, ny, 0, false, feature);
    vertex(bx, by, nx, ny, 0, true, feature);
    indices.insert(indices.end(), {v, v + 2, v + 1, v + 1, v + 2, v + 3});
  }
}

void ExtrusionBuilder::vertex(int32_t x, int32_t y, int nx, int ny, int nz, bool top, uint32_t feature) {
  vertices.put(clamp16i(x));
  vertices.put(clamp16i(y));
  vertices.put(static_cast<int8_t>(nx));
  vertices.put(static_cast<int8_t>(ny));
  vertices.put(static_cast<int8_t>(nz));
  vertices.put(static_cast<int8_t>(top ? 127 : 0));
  vertices.put(feature);
  vertexCount++;
}

void CircleBuilder::addPoint(int32_t x, int32_t y, uint32_t feature) {
  const uint32_t base = vertexCount;
  const int16_t px = clamp16i(x), py = clamp16i(y);
  for (int k = 0; k < 4; k++) {
    vertices.put(px);
    vertices.put(py);
    vertices.put(kCornerX[k]);
    vertices.put(kCornerY[k]);
    vertices.put(static_cast<int8_t>(0));
    vertices.put(static_cast<int8_t>(0));
    vertices.put(feature);
  }
  vertexCount += 4;
  indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

}
