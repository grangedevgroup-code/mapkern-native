#include "terrain/dem.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace mapkern {

const std::array<std::array<int, 2>, 8> kNeighbors = {{{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}}};

Unpack unpackFor(const Value& encoding) {
  if (encoding.isNull() || encoding.stringOr("") == "terrarium") return {256, 1, 1.0 / 256, -32768};
  if (encoding.stringOr("") == "mapbox") return {6553.6, 25.6, 0.1, -10000};
  if (encoding.isObject())
    return {encoding["red"].numberOr(0), encoding["green"].numberOr(0), encoding["blue"].numberOr(0), encoding["offset"].numberOr(0)};
  return {256, 1, 1.0 / 256, -32768};
}

namespace {

void fillMissing(std::vector<float>& e, int size, int stride) {
  std::vector<int32_t> queue(static_cast<size_t>(size) * size);
  size_t tail = 0;
  for (int y = 0; y < size; y++) {
    const int row = (y + kDemBorder) * stride + kDemBorder;
    for (int x = 0; x < size; x++)
      if (!std::isnan(e[row + x])) queue[tail++] = y * size + x;
  }
  auto spread = [&](int x, int y, float value) {
    const int o = (y + kDemBorder) * stride + x + kDemBorder;
    if (!std::isnan(e[o])) return;
    e[o] = value;
    queue[tail++] = y * size + x;
  };
  for (size_t head = 0; head < tail; head++) {
    const int cell = queue[head];
    const int x = cell % size, y = cell / size;
    const float value = e[(y + kDemBorder) * stride + x + kDemBorder];
    if (x > 0) spread(x - 1, y, value);
    if (x < size - 1) spread(x + 1, y, value);
    if (y > 0) spread(x, y - 1, value);
    if (y < size - 1) spread(x, y + 1, value);
  }
}

void clampBorder(std::vector<float>& e, int size, int stride) {
  const int first = kDemBorder, last = kDemBorder + size - 1;
  for (int y = first; y <= last; y++) {
    const int row = y * stride;
    for (int k = 0; k < kDemBorder; k++) {
      e[row + k] = e[row + first];
      e[row + last + 1 + k] = e[row + last];
    }
  }
  for (int k = 0; k < kDemBorder; k++) {
    const size_t row = static_cast<size_t>(stride);
    std::memcpy(&e[k * row], &e[first * row], sizeof(float) * row);
    std::memcpy(&e[(last + 1 + k) * row], &e[last * row], sizeof(float) * row);
  }
}

template <typename Sample>
std::optional<DemGrid> build(int size, Sample sample) {
  DemGrid g;
  g.size = size;
  g.stride = size + 2 * kDemBorder;
  g.elevation.assign(static_cast<size_t>(g.stride) * g.stride, 0.0f);
  float lo = std::numeric_limits<float>::infinity(), hi = -std::numeric_limits<float>::infinity();
  size_t missing = 0;
  for (int y = 0; y < size; y++) {
    int o = (y + kDemBorder) * g.stride + kDemBorder;
    for (int x = 0; x < size; x++, o++) {
      const float v = sample(x, y);
      if (v >= kLowestElevation) {
        g.elevation[o] = v;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      } else {
        g.elevation[o] = std::numeric_limits<float>::quiet_NaN();
        missing++;
      }
    }
  }
  if (missing == static_cast<size_t>(size) * size) return std::nullopt;
  if (missing > 0) {
    g.mask.resize(static_cast<size_t>(size) * size);
    for (int y = 0; y < size; y++) {
      const int row = (y + kDemBorder) * g.stride + kDemBorder;
      for (int x = 0; x < size; x++) g.mask[y * size + x] = std::isnan(g.elevation[row + x]) ? 0 : 255;
    }
    fillMissing(g.elevation, size, g.stride);
  }
  clampBorder(g.elevation, size, g.stride);
  g.min = lo;
  g.max = hi;
  return g;
}

void horn(const DemGrid& grid, int i, int j, float* out) {
  const auto& e = grid.elevation;
  const int stride = grid.stride;
  const int c = (j + kDemBorder - 1) * stride + i + kDemBorder - 1;
  const int n = c - stride, s = c + stride;
  const float nw = e[n - 1], ne = e[n + 1], sw = e[s - 1], se = e[s + 1];
  out[0] = (ne + 2 * e[c + 1] + se - (nw + 2 * e[c - 1] + sw)) / 8;
  out[1] = (sw + 2 * e[s] + se - (nw + 2 * e[n] + ne)) / 8;
}

uint16_t half(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000;
  int32_t exp = static_cast<int32_t>((x >> 23) & 0xff) - 127 + 15;
  uint32_t mant = x & 0x7fffff;
  if (((x >> 23) & 0xff) == 0xff) return static_cast<uint16_t>(sign | 0x7c00 | (mant ? 0x200 : 0));
  if (exp >= 31) return static_cast<uint16_t>(sign | 0x7c00);
  if (exp <= 0) {
    if (exp < -10) return static_cast<uint16_t>(sign);
    mant |= 0x800000;
    const int shift = 14 - exp;
    uint32_t h = mant >> shift;
    if ((mant >> (shift - 1)) & 1) h++;
    return static_cast<uint16_t>(sign | h);
  }
  uint32_t h = sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
  if (mant & 0x1000) h++;
  return static_cast<uint16_t>(h);
}

}

std::optional<DemGrid> decodeDem(const uint8_t* p, int size, const Unpack& unpack, bool repair) {
  std::vector<float> values(static_cast<size_t>(size) * size);
  for (size_t i = 0; i < values.size(); i++) {
    const uint8_t* px = p + i * 4;
    values[i] = px[3] == 255 ? static_cast<float>(px[0] * unpack[0] + px[1] * unpack[1] + px[2] * unpack[2] + unpack[3])
                             : std::numeric_limits<float>::quiet_NaN();
  }
  if (repair) removeSpikes(values, size, std::abs(unpack[0]) * 0.45);
  return build(size, [&](int x, int y) { return values[static_cast<size_t>(y) * size + x]; });
}

int removeSpikes(std::vector<float>& values, int size, double threshold) {
  std::vector<float> around;
  int fixed = 0;
  for (int y = 0; y < size; y++)
    for (int x = 0; x < size; x++) {
      const float v = values[y * size + x];
      if (std::isnan(v)) continue;
      around.clear();
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          if (dx == 0 && dy == 0) continue;
          const int nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= size || ny >= size) continue;
          const float n = values[ny * size + nx];
          if (!std::isnan(n)) around.push_back(n);
        }
      if (around.size() < 3) continue;
      std::sort(around.begin(), around.end());
      if (v > around.back() + threshold || v < around.front() - threshold) {
        values[y * size + x] = around[around.size() >> 1];
        fixed++;
      }
    }
  return fixed;
}

std::optional<DemGrid> decodeElevations(const float* values, size_t count, int size) {
  if (count < static_cast<size_t>(size) * size) return std::nullopt;
  return build(size, [&](int x, int y) { return values[static_cast<size_t>(y) * size + x]; });
}

bool backfill(DemGrid& target, const DemGrid& source, int dx, int dy) {
  if (target.size != source.size || (dx == 0 && dy == 0)) return false;
  const int size = target.size, stride = target.stride;
  const int x0 = dx < 0 ? 0 : dx > 0 ? kDemBorder + size : kDemBorder;
  const int x1 = dx < 0 ? kDemBorder : dx > 0 ? stride : kDemBorder + size;
  const int y0 = dy < 0 ? 0 : dy > 0 ? kDemBorder + size : kDemBorder;
  const int y1 = dy < 0 ? kDemBorder : dy > 0 ? stride : kDemBorder + size;
  const int shift = -dy * size * stride - dx * size;
  for (int y = y0; y < y1; y++) {
    const int row = y * stride;
    for (int x = x0; x < x1; x++) target.elevation[row + x] = source.elevation[row + x + shift];
  }
  return true;
}

std::vector<float> gradients(const DemGrid& grid) {
  const int n = grid.size + 2;
  std::vector<float> out(static_cast<size_t>(n) * n * 2);
  for (int j = 0; j < n; j++)
    for (int i = 0; i < n; i++) horn(grid, i, j, &out[(static_cast<size_t>(j) * n + i) * 2]);
  return out;
}

std::vector<FrameStrip> frameStrips(const DemGrid& grid) {
  const int n = grid.size + 2;
  auto strip = [&](int x, int y, int w, int h) {
    FrameStrip s{x, y, w, h, std::vector<float>(static_cast<size_t>(w) * h * 2)};
    for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++) horn(grid, x + i, y + j, &s.data[(static_cast<size_t>(j) * w + i) * 2]);
    return s;
  };
  return {strip(0, 0, n, 2), strip(0, n - 2, n, 2), strip(0, 2, 2, n - 4), strip(n - 2, 2, 2, n - 4)};
}

void writeFrame(const DemGrid& grid, std::vector<float>& out) {
  const int n = grid.size + 2;
  for (const auto& s : frameStrips(grid))
    for (int j = 0; j < s.height; j++)
      std::memcpy(&out[(static_cast<size_t>(s.y + j) * n + s.x) * 2], &s.data[static_cast<size_t>(j) * s.width * 2], sizeof(float) * s.width * 2);
}

std::optional<double> sampleElevation(const DemGrid& grid, double u, double v) {
  const int size = grid.size, stride = grid.stride;
  if (!grid.mask.empty()) {
    const int mx = std::clamp(static_cast<int>(std::floor(u * size)), 0, size - 1);
    const int my = std::clamp(static_cast<int>(std::floor(v * size)), 0, size - 1);
    if (grid.mask[my * size + mx] == 0) return std::nullopt;
  }
  const double fx = std::clamp(u * size - 0.5 + kDemBorder, 0.0, stride - 1.0);
  const double fy = std::clamp(v * size - 0.5 + kDemBorder, 0.0, stride - 1.0);
  const int x0 = std::min(stride - 2, static_cast<int>(std::floor(fx)));
  const int y0 = std::min(stride - 2, static_cast<int>(std::floor(fy)));
  const double tx = fx - x0, ty = fy - y0;
  const auto& e = grid.elevation;
  const int o = y0 * stride + x0;
  const double top = e[o] + (e[o + 1] - e[o]) * tx;
  const double bottom = e[o + stride] + (e[o + stride + 1] - e[o + stride]) * tx;
  return top + (bottom - top) * ty;
}

std::vector<uint16_t> toHalf(const std::vector<float>& values) {
  std::vector<uint16_t> out(values.size());
  for (size_t i = 0; i < values.size(); i++) out[i] = half(values[i]);
  return out;
}

bool isRawElevation(const uint8_t* b, size_t size) { return size >= 8 && b[0] == 0x4d && b[1] == 0x4b && b[2] == 0x45 && b[3] == 0x4c; }

}
