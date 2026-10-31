#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/value.hpp"
#include "formats/mvt.hpp"
#include "style/expression.hpp"

namespace mapkern {

inline constexpr int kLineStride = 20;
inline constexpr int kFillStride = 8;
inline constexpr int kCircleStride = 12;
inline constexpr int kExtrusionStride = 12;
inline constexpr double kExtrudeScale = 2048;

class Growable {
 public:
  std::vector<uint8_t> bytes;
  void reserve(size_t extra) { bytes.reserve(bytes.size() + extra); }
  template <class T>
  void put(T v) {
    const size_t at = bytes.size();
    bytes.resize(at + sizeof(T));
    std::memcpy(bytes.data() + at, &v, sizeof(T));
  }
  size_t size() const { return bytes.size(); }
};

struct Geometry {
  std::vector<uint8_t> vertices;
  std::vector<uint32_t> indices;
  uint32_t vertexCount = 0;
};

class FillBuilder {
 public:
  Growable vertices;
  std::vector<uint32_t> indices;
  std::vector<uint32_t> edges;
  uint32_t vertexCount = 0;
  void addPolygon(const std::vector<Ring>& rings, uint32_t feature);

 private:
  std::vector<double> coords_;
  std::vector<uint32_t> holes_, starts_, tris_;
};

struct LineOptions {
  std::string cap = "butt";
  std::string join = "miter";
  double miterLimit = 2;
  double roundLimit = 0.94;
};

class LineBuilder {
 public:
  explicit LineBuilder(LineOptions options) : options_(std::move(options)) {}
  Growable vertices;
  std::vector<uint32_t> indices;
  uint32_t vertexCount = 0;
  void addLine(const Ring& points, uint32_t feature, bool closedHint = false);

 private:
  void shift(double x, double y) {
    shiftX_ = x;
    shiftY_ = y;
  }
  void jointShift(size_t s0, size_t s1);
  uint32_t vertex(double x, double y, double ex, double ey, double sx, double sy, bool square, double dist, uint32_t feature);
  bool simple(size_t s0, size_t s1) const;
  void miterPair(double x, double y, size_t s0, size_t s1, double dist, uint32_t feature);
  void cap(double x, double y, double dx, double dy, double dist, uint32_t feature);
  void join(double x, double y, double d0x, double d0y, double d1x, double d1y, double dist, uint32_t feature);
  void tri(uint32_t a, uint32_t b, uint32_t c) {
    indices.push_back(a);
    indices.push_back(b);
    indices.push_back(c);
  }

  LineOptions options_;
  std::vector<double> pts_, dirs_, lens_;
  int64_t pairLeft_ = -1, pairRight_ = -1;
  double shiftX_ = 0, shiftY_ = 0;
};

class ExtrusionBuilder {
 public:
  explicit ExtrusionBuilder(uint32_t extent) : extent_(extent) {}
  Growable vertices;
  std::vector<uint32_t> indices;
  uint32_t vertexCount = 0;
  void addPolygon(const std::vector<Ring>& polygon, uint32_t feature);

 private:
  void addWalls(const Ring& ring, size_t n, int outward, uint32_t feature);
  void vertex(int32_t x, int32_t y, int nx, int ny, int nz, bool top, uint32_t feature);
  uint32_t extent_;
  std::vector<double> coords_;
  std::vector<uint32_t> holes_, tris_;
};

class CircleBuilder {
 public:
  Growable vertices;
  std::vector<uint32_t> indices;
  uint32_t vertexCount = 0;
  void addPoint(int32_t x, int32_t y, uint32_t feature);
};

std::vector<double> clipRing(const std::vector<double>& ring, double x0, double y0, double x1, double y1);

}
