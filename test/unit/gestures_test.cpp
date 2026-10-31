#include <cmath>

#include "camera/transform.hpp"
#include "check.hpp"
#include "map/gestures.hpp"

using namespace mapkern;

namespace {

struct Rig {
  Transform t;
  int zooms = 0;
  double zoomDelta = 0;
  Gestures g;
  Rig(bool globe, double zoom)
      : g(t, GestureHost{[this](double x, double y) -> std::optional<Point3> {
                           const auto w = t.unproject(x, y);
                           if (!w) return std::nullopt;
                           return Point3{(*w)[0], (*w)[1], 0};
                         },
                         [] {}, [] {},
                         [this](double d, double, double) {
                           zooms++;
                           zoomDelta += d;
                         }}) {
    t.resize(800, 1236);
    if (globe) t.setProjection(Projection::Globe);
    t.setZoom(zoom);
    t.setCenter(-122.33, 47.61);
  }
  void drag(int id, double x0, double y0, double x1, double y1, double start, int steps = 20) {
    g.down(id, x0, y0, start);
    for (int i = 1; i <= steps; i++) {
      const double k = static_cast<double>(i) / steps;
      g.move(id, x0 + (x1 - x0) * k, y0 + (y1 - y0) * k, start + i * 16);
    }
  }
};

}

TEST("gestures: dragging the globe up and down keeps its size") {
  Rig r(true, 2.5);
  const double before = r.t.globeRadius();
  r.drag(1, 400, 500, 400, 950, 0);
  const double down = r.t.globeRadius();
  r.g.up(1, 400, 950, 400, false);
  r.drag(1, 400, 950, 400, 250, 2000);
  const double up = r.t.globeRadius();
  r.g.up(1, 400, 250, 2400, false);
  EXPECT(std::abs(down / before - 1) < 0.02);
  EXPECT(std::abs(up / before - 1) < 0.02);
}

TEST("gestures: a drag keeps the grabbed place under the finger") {
  Rig r(false, 10);
  const auto grabbed = r.t.unproject(300, 400);
  r.drag(1, 300, 400, 520, 610, 0);
  const auto now = r.t.unproject(520, 610);
  EXPECT(grabbed && now);
  if (grabbed && now) EXPECT(std::abs((*grabbed)[0] - (*now)[0]) < 1e-9 && std::abs((*grabbed)[1] - (*now)[1]) < 1e-9);
}

TEST("gestures: small finger jitter is a tap, two taps zoom in") {
  Rig r(false, 10);
  const auto c0 = r.t.unproject(400, 600);
  r.g.down(1, 400, 600, 0);
  r.g.move(1, 402, 601, 30);
  r.g.up(1, 402, 601, 60, false);
  const auto c1 = r.t.unproject(400, 600);
  EXPECT(c0 && c1 && (*c0)[0] == (*c1)[0] && (*c0)[1] == (*c1)[1]);
  EXPECT(r.zooms == 0);
  r.g.down(1, 401, 600, 200);
  r.g.up(1, 401, 600, 260, false);
  EXPECT(r.zooms == 1 && r.zoomDelta == 1);
}

TEST("gestures: a two-finger tap zooms out") {
  Rig r(false, 10);
  r.g.down(1, 300, 600, 0);
  r.g.down(2, 500, 600, 10);
  r.g.up(1, 300, 600, 120, false);
  r.g.up(2, 500, 600, 130, false);
  EXPECT(r.zooms == 1 && r.zoomDelta == -1);
}

TEST("gestures: pinching apart zooms in without turning the map") {
  Rig r(false, 10);
  r.g.down(1, 350, 600, 0);
  r.g.down(2, 450, 600, 5);
  for (int i = 1; i <= 20; i++) {
    const double spread = 50 + i * 5;
    r.g.move(1, 400 - spread, 600 + i * 0.3, 5 + i * 16);
    r.g.move(2, 400 + spread, 600 - i * 0.3, 5 + i * 16);
  }
  EXPECT(std::abs(r.t.zoom() - (10 + std::log2(150.0 / 50.0))) < 0.05);
  EXPECT(r.t.bearing() == 0);
}

TEST("gestures: twisting two fingers far enough turns the map") {
  Rig r(false, 10);
  r.g.down(1, 300, 600, 0);
  r.g.down(2, 500, 600, 5);
  for (int i = 1; i <= 20; i++) {
    const double a = i * 2.0 * kPi / 180;
    r.g.move(1, 400 - 100 * std::cos(a), 600 - 100 * std::sin(a), 5 + i * 16);
    r.g.move(2, 400 + 100 * std::cos(a), 600 + 100 * std::sin(a), 5 + i * 16);
  }
  EXPECT(std::abs(r.t.bearing()) > 10);
}

TEST("gestures: two fingers moving up together tilt the map") {
  Rig r(false, 10);
  r.g.down(1, 300, 800, 0);
  r.g.down(2, 500, 800, 5);
  for (int i = 1; i <= 20; i++) {
    r.g.move(1, 300, 800 - i * 6, 5 + i * 16);
    r.g.move(2, 500, 800 - i * 6, 5 + i * 16);
  }
  EXPECT(r.t.pitch() > 20);
  EXPECT(std::abs(r.t.zoom() - 10) < 1e-9);
}

TEST("gestures: a quick flick keeps the map gliding") {
  Rig r(false, 10);
  r.drag(1, 400, 600, 400, 300, 0, 6);
  r.g.up(1, 400, 300, 6 * 16 + 4, false);
  EXPECT(r.g.animating());
}
