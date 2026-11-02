#include "map/gestures.hpp"

#include <algorithm>
#include <cmath>

#include "geo/mercator.hpp"
#include "style/layer.hpp"

namespace mapkern {

namespace {

constexpr double kMaxInertiaSpeed = 1.4;
constexpr double kInertiaDeceleration = 0.0025;
constexpr double kDoubleTapMs = 300;
constexpr double kRotateStart = 20 * kPi / 180;
constexpr double kRotateStartZooming = 40 * kPi / 180;
constexpr double kRotateArc = 30;
constexpr double kQuickZoomPixels = 120;
constexpr double kTiltStart = 5;
constexpr double kTiltSideBySide = 0.6;
constexpr double kTiltGiveUp = 24;
constexpr double kTiltTwist = 0.25;
constexpr double kPinchMove = 6;
constexpr double kPinchTwist = 0.1;

double angleDelta(double a, double b) {
  double d = a - b;
  while (d > kPi) d -= 2 * kPi;
  while (d < -kPi) d += 2 * kPi;
  return d;
}

}

Gestures::Gestures(Transform& transform, GestureHost host) : transform_(transform), host_(std::move(host)) {}

void Gestures::place(const Point3& anchor, double x, double y, bool keepSize) {
  transform_.setLocationAtPoint(World{anchor[0], anchor[1]}, x, y, anchor[2], keepSize);
}

void Gestures::follow(std::optional<Point3>& anchor, double x, double y) {
  if (transform_.globeAmount() <= 0) return;
  if (auto under = host_.anchor(x, y)) anchor = under;
}

void Gestures::drag(std::optional<Point3>& anchor, double fromX, double fromY, double x, double y) {
  const World before = transform_.centerWorld();
  if (transform_.globeAmount() >= 1) {
    transform_.panGlobe(x - fromX, y - fromY);
    anchor.reset();
    track(before, std::hypot(x - fromX, y - fromY), -1);
    return;
  }
  if (!anchor) anchor = host_.anchor(fromX, fromY);
  if (!anchor) return;
  const Point3 grabbed = *anchor;
  const double lift = grabbed[2] * transform_.worldSize();
  const ScreenPoint from = transform_.project(grabbed[0], grabbed[1], lift);
  place(grabbed, x, y, true);
  const ScreenPoint at = transform_.project(grabbed[0], grabbed[1], grabbed[2] * transform_.worldSize());
  track(before, std::hypot(x - from.x, y - from.y), at.w > 0 ? std::hypot(at.x - x, at.y - y) : -1);
  follow(anchor, x, y);
}

void Gestures::track(const World& before, double moved, double slip) {
  if (transform_.globeAmount() <= 0) return;
  const World after = transform_.centerWorld();
  const double turned = globeTurn(before[0], before[1], after[0], after[1]);
  PanStats& s = panStats_;
  s.moves++;
  if (slip >= 0) s.maxSlipPx = std::max(s.maxSlipPx, slip);
  s.maxTurnDeg = std::max(s.maxTurnDeg, turned * 180 / kPi);
  if (moved <= 0) return;
  const double ratio = turned / (moved / std::max(1.0, transform_.globeRadius()));
  s.maxTurnRatio = std::max(s.maxTurnRatio, ratio);
  if (ratio > 5 && turned > kPi / 180) s.wild++;
}

void Gestures::cancel() {
  mode_ = Mode{};
  pointers_.clear();
  samples_.clear();
}

void Gestures::stopMomentum() {
  inertia_.reset();
  wheel_.reset();
}

void Gestures::down(int id, double x, double y, double time) {
  host_.interrupt();
  stopMomentum();
  pointers_[id] = Pointer{id, x, y, x, y, time};
  if (pointers_.size() == 2 && options.touchZoomRotate) {
    beginTwoFinger(pointers_.begin()->second, std::next(pointers_.begin())->second, time);
    return;
  }
  if (pointers_.size() > 1) return;
  const bool quick = options.touchZoomRotate && lastTap_ && time - lastTap_->t < kDoubleTapMs &&
                     std::hypot(x - lastTap_->x, y - lastTap_->y) < 30;
  mode_ = Mode{};
  mode_.kind = Kind::Press;
  mode_.pointer = id;
  mode_.quick = quick;
  samples_ = {{time, x, y}};
}

void Gestures::move(int id, double x, double y, double time) {
  auto it = pointers_.find(id);
  if (it == pointers_.end()) return;
  Pointer& p = it->second;
  p.x = x;
  p.y = y;
  if (mode_.kind == Kind::Press && mode_.pointer == id) {
    if (std::hypot(x - p.startX, y - p.startY) < options.clickTolerance) return;
    mode_.moved = true;
    if (mode_.quick) {
      lastTap_.reset();
      Mode m;
      m.kind = Kind::Quick;
      m.pointer = id;
      m.startY = p.startY;
      m.zoom0 = transform_.zoom();
      m.anchor = host_.anchor(p.startX, p.startY);
      m.x = p.startX;
      m.y = p.startY;
      mode_ = m;
    } else {
      if (!options.dragPan) return;
      Mode m;
      m.kind = Kind::Pan;
      m.pointer = id;
      m.fling = true;
      m.x = p.startX;
      m.y = p.startY;
      m.anchor = host_.anchor(p.startX, p.startY);
      if (!m.anchor && transform_.globeAmount() < 1) {
        m.anchor = host_.anchor(x, y);
        if (!m.anchor) return;
        m.x = x;
        m.y = y;
        samples_ = {{time, x, y}};
        mode_ = m;
        return;
      }
      mode_ = m;
    }
  }
  if (mode_.kind == Kind::Quick && mode_.pointer == id) {
    transform_.setZoom(mode_.zoom0 + (y - mode_.startY) / kQuickZoomPixels);
    if (mode_.anchor) place(*mode_.anchor, mode_.x, mode_.y, false);
    host_.changed();
    return;
  }
  if (mode_.kind == Kind::Pan && mode_.pointer == id) {
    drag(mode_.anchor, mode_.x, mode_.y, x, y);
    mode_.x = x;
    mode_.y = y;
    samples_.push_back({time, x, y});
    while (samples_.size() > 2 && time - samples_.front().t > 100) samples_.erase(samples_.begin());
    host_.changed();
  } else if (mode_.kind == Kind::Touch2) {
    moveTwoFinger();
  }
}

void Gestures::up(int id, double x, double y, double time, bool cancelled) {
  if (!pointers_.erase(id)) return;
  if (mode_.kind == Kind::Press && mode_.pointer == id) {
    const bool moved = mode_.moved;
    mode_ = Mode{};
    if (cancelled || moved) return;
    detectDoubleTap(time, x, y);
    if (host_.click) host_.click(x, y);
    return;
  }
  if (mode_.kind == Kind::Pan && mode_.pointer == id) {
    const Mode m = mode_;
    mode_ = Mode{};
    if (!cancelled && m.fling) startInertia(m.anchor, x, y, time);
    return;
  }
  if (mode_.kind == Kind::Quick && mode_.pointer == id) {
    mode_ = Mode{};
    return;
  }
  if (mode_.kind == Kind::Touch2 && (mode_.ids[0] == id || mode_.ids[1] == id)) {
    if (!cancelled && mode_.sub == Sub::Undecided && time - mode_.started < kDoubleTapMs && options.doubleTapZoom) {
      const double mx = mode_.start.midX, my = mode_.start.midY;
      mode_ = Mode{};
      lastTap_.reset();
      if (host_.zoomBy) host_.zoomBy(-1, mx, my);
      return;
    }
    mode_ = Mode{};
    if (!pointers_.empty() && options.dragPan) {
      const Pointer& rest = pointers_.begin()->second;
      const auto anchor = host_.anchor(rest.x, rest.y);
      if (anchor || transform_.globeAmount() >= 1) {
        mode_.kind = Kind::Pan;
        mode_.pointer = rest.id;
        mode_.anchor = anchor;
        mode_.fling = false;
        mode_.x = rest.x;
        mode_.y = rest.y;
        samples_ = {{time, rest.x, rest.y}};
      }
    }
  }
}

void Gestures::startInertia(const std::optional<Point3>& anchor, double x, double y, double time) {
  if (samples_.empty()) return;
  const Sample first = samples_.front();
  samples_.clear();
  const double dt = time - first.t;
  if (dt <= 0 || dt > 150) return;
  double vx = (x - first.x) / dt;
  double vy = (y - first.y) / dt;
  const double speed = std::hypot(vx, vy);
  if (speed < 0.15) return;
  if (speed > kMaxInertiaSpeed) {
    vx *= kMaxInertiaSpeed / speed;
    vy *= kMaxInertiaSpeed / speed;
  }
  const double v = std::hypot(vx, vy);
  const double distance = v * v / (2 * kInertiaDeceleration);
  Inertia i;
  i.start = nowMs();
  i.duration = v / kInertiaDeceleration;
  i.from = {x, y};
  i.offset = {vx / v * distance, vy / v * distance};
  i.anchor = anchor;
  inertia_ = i;
  host_.changed();
}

Gestures::TwoFinger Gestures::twoFinger(const Pointer& a, const Pointer& b) const {
  TwoFinger f;
  f.dist = std::max(1.0, std::hypot(b.x - a.x, b.y - a.y));
  f.angle = std::atan2(b.y - a.y, b.x - a.x);
  f.midX = (a.x + b.x) / 2;
  f.midY = (a.y + b.y) / 2;
  f.x0 = a.x;
  f.y0 = a.y;
  f.x1 = b.x;
  f.y1 = b.y;
  return f;
}

void Gestures::beginTwoFinger(const Pointer& a, const Pointer& b, double time) {
  Mode m;
  m.kind = Kind::Touch2;
  m.ids = {a.id, b.id};
  m.start = twoFinger(a, b);
  m.zoom0 = transform_.zoom();
  m.bearing0 = transform_.bearing();
  m.pitch0 = transform_.pitch();
  m.anchor = host_.anchor(m.start.midX, m.start.midY);
  m.started = time;
  mode_ = m;
}

void Gestures::moveTwoFinger() {
  const auto a = pointers_.find(mode_.ids[0]);
  const auto b = pointers_.find(mode_.ids[1]);
  if (a == pointers_.end() || b == pointers_.end()) return;
  const TwoFinger now = twoFinger(a->second, b->second);
  const TwoFinger& s = mode_.start;
  if (mode_.sub == Sub::Undecided) {
    const double d0 = now.y0 - s.y0;
    const double d1 = now.y1 - s.y1;
    const double across = std::abs(now.x0 - s.x0) + std::abs(now.x1 - s.x1);
    const double scaleChange = std::abs(std::log(now.dist / s.dist));
    const double twist = std::abs(angleDelta(now.angle, s.angle));
    const double moved = std::hypot(now.midX - s.midX, now.midY - s.midY);
    const bool tiltable = options.touchPitch && transform_.maxPitch > transform_.minPitch && std::abs(std::sin(s.angle)) < kTiltSideBySide &&
                          d0 * d1 >= 0 && std::abs(d0) + std::abs(d1) >= 2 * across && scaleChange < 0.08 && twist < kTiltTwist;
    if (tiltable && std::abs(d0) >= kTiltStart && std::abs(d1) >= kTiltStart) {
      mode_.sub = Sub::Pitch;
    } else if (scaleChange > 0.04 || moved > (tiltable ? kTiltGiveUp : kPinchMove) || (!tiltable && twist > kPinchTwist)) {
      mode_.sub = Sub::Pinch;
    } else {
      return;
    }
  }
  if (mode_.sub == Sub::Pitch) {
    transform_.setPitch(mode_.pitch0 - (now.midY - s.midY) * 0.4);
    host_.changed();
    return;
  }
  const double scale = std::log2(now.dist / s.dist);
  transform_.setZoom(mode_.zoom0 + scale);
  const double turn = angleDelta(now.angle, s.angle);
  if (!mode_.turnStart && options.touchRotate) {
    const double needed = std::abs(scale) > 0.2 ? kRotateStartZooming : kRotateStart;
    if (std::abs(turn) > needed && std::abs(turn) * now.dist / 2 > kRotateArc) mode_.turnStart = turn;
  }
  if (mode_.turnStart) transform_.setBearing(mode_.bearing0 - (turn - *mode_.turnStart) * 180 / kPi);
  if (mode_.anchor) {
    mode_.zoom0 += transform_.setLocationAtPoint(World{(*mode_.anchor)[0], (*mode_.anchor)[1]}, now.midX, now.midY, (*mode_.anchor)[2], true);
    follow(mode_.anchor, now.midX, now.midY);
  }
  host_.changed();
}

void Gestures::detectDoubleTap(double t, double x, double y) {
  if (lastTap_ && t - lastTap_->t < kDoubleTapMs && std::hypot(x - lastTap_->x, y - lastTap_->y) < 30) {
    lastTap_.reset();
    if (options.doubleTapZoom && host_.zoomBy) host_.zoomBy(1, x, y);
    return;
  }
  lastTap_ = Tap{t, x, y};
}

void Gestures::wheel(double zoomDelta, double x, double y, double now) {
  if (zoomDelta == 0) return;
  host_.interrupt();
  inertia_.reset();
  if (!wheel_) {
    WheelZoom w;
    w.target = transform_.zoom();
    w.screen = {x, y};
    w.world = host_.anchor(x, y);
    w.last = now;
    wheel_ = w;
  } else if (std::hypot(x - wheel_->screen[0], y - wheel_->screen[1]) > 2) {
    wheel_->screen = {x, y};
    wheel_->world = host_.anchor(x, y);
  }
  wheel_->target = std::clamp(wheel_->target + zoomDelta, transform_.minZoom, transform_.maxZoom);
  wheel_->last = std::min(wheel_->last, now);
  host_.changed();
}

bool Gestures::step(double now) {
  bool busy = false;
  if (inertia_) {
    Inertia& i = *inertia_;
    const double t = std::min(1.0, (now - i.start) / i.duration);
    const double k = 1 - (1 - t) * (1 - t);
    drag(i.anchor, i.from[0] + i.offset[0] * i.done, i.from[1] + i.offset[1] * i.done, i.from[0] + i.offset[0] * k, i.from[1] + i.offset[1] * k);
    i.done = k;
    host_.changed();
    if (t >= 1) inertia_.reset();
    else busy = true;
  }
  if (wheel_) {
    WheelZoom& w = *wheel_;
    const double dt = std::min(100.0, std::max(1.0, now - w.last));
    w.last = now;
    const double current = transform_.zoom();
    const double diff = w.target - current;
    const double next = std::abs(diff) < 0.001 ? w.target : current + diff * (1 - std::exp(-dt / 70));
    transform_.setZoom(next);
    if (w.world) place(*w.world, w.screen[0], w.screen[1], false);
    host_.changed();
    if (next == w.target || transform_.zoom() != next) wheel_.reset();
    else busy = true;
  }
  return busy;
}

}
