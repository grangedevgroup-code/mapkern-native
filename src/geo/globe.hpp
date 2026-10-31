#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "geo/mercator.hpp"

namespace mapkern {

inline constexpr double kGlobeFullZoom = 10;
inline constexpr double kGlobeFlatZoom = 12;

enum class Projection { Mercator, Globe };

inline double globeness(double zoom) {
  if (zoom <= kGlobeFullZoom) return 1;
  if (zoom >= kGlobeFlatZoom) return 0;
  const double t = (zoom - kGlobeFullZoom) / (kGlobeFlatZoom - kGlobeFullZoom);
  return 1 - t * t * (3 - 2 * t);
}

inline double globeFitZoom(double width, double height, double fov, double lat) {
  if (!(width > 0 && height > 0)) return 0;
  const double d = height / 2 / std::tan(fov * kPi / 360);
  const double r = 0.42 * std::min(width, height);
  const double radius = (r * r + r * std::sqrt(r * r + d * d)) / d;
  const double rad = std::clamp(lat, -85.0, 85.0) * kPi / 180;
  return std::log2(2 * kPi * radius * std::cos(rad) / 512);
}

inline std::array<double, 3> unitFromMercator(double x, double y) {
  const double m = kPi * (1 - 2 * y);
  const double cosLat = 1 / std::cosh(m);
  const double sinLat = std::tanh(m);
  const double lng = 2 * kPi * x - kPi;
  return {cosLat * std::cos(lng), cosLat * std::sin(lng), sinLat};
}

inline std::array<double, 2> mercatorFromUnit(double px, double py, double pz) {
  const double length = std::hypot(px, py, pz);
  const double sinLat = std::clamp(pz / length, -1.0, 1.0);
  const double lng = std::atan2(py, px);
  const double x = (lng + kPi) / (2 * kPi);
  const double y = (1 - std::atanh(std::clamp(sinLat, -0.9999999999999, 0.9999999999999)) / kPi) / 2;
  return {x - std::floor(x), std::clamp(y, 0.0, 1.0)};
}

inline double nearestInBox(double lat0, double lat1, double lng0, double lng1, double latC, double lngC,
                           double out[2]) {
  const double span = lng1 - lng0;
  double d = lngC - lng0;
  d -= std::floor(d / (2 * kPi)) * 2 * kPi;
  const double sinC = std::sin(latC);
  const double cosC = std::cos(latC);
  if (d <= span) {
    const double lat = std::clamp(latC, lat0, lat1);
    out[0] = lat;
    out[1] = lngC;
    return std::cos(lat - latC);
  }
  const double east = d - span;
  const double west = 2 * kPi - d;
  const double toEdge = std::min(east, west);
  const double cosD = std::cos(toEdge);
  const double best = std::atan2(sinC, cosC * cosD);
  const double lat = std::clamp(best, lat0, lat1);
  out[0] = lat;
  out[1] = east <= west ? lng1 : lng0;
  return std::cos(lat) * cosC * cosD + std::sin(lat) * sinC;
}

}
