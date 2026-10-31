#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mapkern {

inline constexpr double kPi = std::numbers::pi;
inline constexpr double kEarthRadius = 6378137;
inline constexpr double kEarthCircumference = 2 * kPi * kEarthRadius;
inline constexpr double kMaxLatitude = 85.0511287798066;
inline constexpr double kTileSize = 512;

struct LngLat {
  double lng = 0;
  double lat = 0;
};

inline double clampLatitude(double lat) { return std::clamp(lat, -kMaxLatitude, kMaxLatitude); }

inline double wrapLongitude(double lng) {
  double w = std::fmod(std::fmod(lng + 180, 360) + 360, 360) - 180;
  return w == -180 && lng > 0 ? 180 : w;
}

inline double mercatorX(double lng) { return (lng + 180) / 360; }

inline double mercatorY(double lat) {
  const double s = std::sin(clampLatitude(lat) * kPi / 180);
  return 0.5 - std::log((1 + s) / (1 - s)) / (4 * kPi);
}

inline double lngFromMercatorX(double x) { return x * 360 - 180; }

inline double latFromMercatorY(double y) { return 360 / kPi * std::atan(std::exp((0.5 - y) * 2 * kPi)) - 90; }

inline double worldSize(double zoom) { return kTileSize * std::exp2(zoom); }

inline double metersPerPixel(double lat, double zoom) {
  return std::cos(clampLatitude(lat) * kPi / 180) * kEarthCircumference / worldSize(zoom);
}

inline double metersToWorldUnits(double meters, double lat) {
  return meters / (kEarthCircumference * std::cos(clampLatitude(lat) * kPi / 180));
}

inline double metersToWorldUnitsAtY(double meters, double y) { return metersToWorldUnits(meters, latFromMercatorY(y)); }

inline double distanceMeters(LngLat a, LngLat b) {
  const double rad = kPi / 180;
  const double dLat = (b.lat - a.lat) * rad;
  const double dLng = (b.lng - a.lng) * rad;
  const double h = std::pow(std::sin(dLat / 2), 2) +
                   std::cos(a.lat * rad) * std::cos(b.lat * rad) * std::pow(std::sin(dLng / 2), 2);
  return 2 * kEarthRadius * std::asin(std::min(1.0, std::sqrt(h)));
}

}
