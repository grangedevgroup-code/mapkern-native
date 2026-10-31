#pragma once

#include <cmath>
#include <functional>
#include <optional>

#include "camera/transform.hpp"

namespace mapkern {

struct CameraState {
  double x = 0.5, y = 0.5, zoom = 0, bearing = 0, pitch = 0;
  Padding padding;
};

using Easing = std::function<double(double)>;

inline double easeOutCubic(double t) { return 1 - std::pow(1 - t, 3); }
inline double easeInOutCubic(double t) { return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2; }

struct Anchor {
  World world;
  double sx = 0, sy = 0;
  std::optional<double> z;
};

struct CameraMotion {
  double duration = 0;
  CameraState target;
  std::function<void(Transform&, double)> apply;
};

inline CameraState readState(Transform& t) {
  const auto c = t.centerWorld();
  return {c[0], c[1], t.zoom(), t.bearing(), t.pitch(), t.padding()};
}

inline double shortestX(double from, double to) {
  double dx = to - from;
  dx -= std::round(dx);
  return from + dx;
}

inline double shortestBearing(double from, double to) {
  double d = std::fmod(to - from, 360.0);
  if (d > 180) d -= 360;
  if (d < -180) d += 360;
  return from + d;
}

inline void easePadding(Transform& t, const Padding& from, const Padding& to, double k) {
  if (from == to) return;
  t.setPadding({from.top + (to.top - from.top) * k, from.right + (to.right - from.right) * k, from.bottom + (to.bottom - from.bottom) * k,
                from.left + (to.left - from.left) * k});
}

inline CameraMotion easeMotion(const CameraState& from, const CameraState& to, double duration, Easing easing,
                               std::optional<Anchor> anchor = std::nullopt) {
  const double tx = shortestX(from.x, to.x);
  const double tb = shortestBearing(from.bearing, to.bearing);
  CameraMotion m;
  m.duration = duration;
  m.target = to;
  m.apply = [=](Transform& t, double time) {
    const double k = easing(std::clamp(time, 0.0, 1.0));
    easePadding(t, from.padding, to.padding, k);
    t.setZoom(from.zoom + (to.zoom - from.zoom) * k);
    t.setBearing(from.bearing + (tb - from.bearing) * k);
    t.setPitch(from.pitch + (to.pitch - from.pitch) * k);
    if (anchor) t.setLocationAtPoint(anchor->world, anchor->sx, anchor->sy, anchor->z);
    else t.setCenterWorld(from.x + (tx - from.x) * k, from.y + (to.y - from.y) * k);
  };
  return m;
}

struct FlyOptions {
  double curve = 1.42;
  double speed = 1.2;
  std::optional<double> screenSpeed;
  std::optional<double> maxDuration;
  std::optional<double> duration;
  Easing easing = easeInOutCubic;
};

inline CameraMotion flyMotion(const CameraState& from, const CameraState& to, double viewport, const FlyOptions& options) {
  const double rho = options.curve;
  const double tx = shortestX(from.x, to.x);
  const double tb = shortestBearing(from.bearing, to.bearing);
  const double w0 = viewport / std::exp2(from.zoom);
  const double w1 = viewport / std::exp2(to.zoom);
  const double u1 = std::hypot(tx - from.x, to.y - from.y) * 512;
  const double rho2 = rho * rho;
  auto b = [&](int i) {
    const double w = i == 0 ? w0 : w1;
    return (w1 * w1 - w0 * w0 + (i == 0 ? 1 : -1) * rho2 * rho2 * u1 * u1) / (2 * w * rho2 * u1);
  };
  auto r = [&](int i) {
    const double bi = b(i);
    return std::log(std::sqrt(bi * bi + 1) - bi);
  };
  double S;
  std::function<double(double)> widthAt, progressAt;
  if (u1 < 1e-9 || !std::isfinite(r(0)) || !std::isfinite(r(1))) {
    const double k = w1 < w0 ? -1 : 1;
    S = std::abs(std::log(w1 / w0)) / rho;
    widthAt = [=](double s) { return w0 * std::exp(k * rho * s); };
    progressAt = [](double) { return 0.0; };
  } else {
    const double r0 = r(0), r1 = r(1);
    S = (r1 - r0) / rho;
    widthAt = [=](double s) { return w0 * std::cosh(r0) / std::cosh(rho * s + r0); };
    progressAt = [=](double s) { return w0 * (std::cosh(r0) * std::tanh(rho * s + r0) - std::sinh(r0)) / rho2 / u1; };
  }
  double duration;
  if (options.duration) {
    duration = *options.duration;
  } else {
    const double speed = options.screenSpeed ? *options.screenSpeed / rho : options.speed;
    duration = 1000 * S / std::max(speed, 1e-6);
    if (!std::isfinite(duration)) duration = 0;
    if (options.maxDuration && duration > *options.maxDuration) duration = 0;
  }
  if (!std::isfinite(S)) S = 0;
  CameraMotion m;
  m.duration = duration;
  m.target = to;
  const Easing easing = options.easing;
  m.apply = [=](Transform& t, double time) {
    const double k = easing(std::clamp(time, 0.0, 1.0));
    const double s = k * S;
    const double u = k >= 1 ? 1 : progressAt(s);
    const double zoom = k >= 1 ? to.zoom : from.zoom + std::log2(w0 / widthAt(s));
    easePadding(t, from.padding, to.padding, k);
    t.setZoom(zoom);
    t.setBearing(from.bearing + (tb - from.bearing) * k);
    t.setPitch(from.pitch + (to.pitch - from.pitch) * k);
    t.setCenterWorld(from.x + (tx - from.x) * u, from.y + (to.y - from.y) * u);
  };
  return m;
}

}
