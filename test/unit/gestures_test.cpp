#include <algorithm>
#include <cmath>

#include "camera/transform.hpp"
#include "check.hpp"
#include "geo/mercator.hpp"
#include "map/gestures.hpp"
#include "style/layer.hpp"

using namespace mapkern;

namespace {

double turn(World a, World b) {
  const double la = latFromMercatorY(a[1]) * kPi / 180, lb = latFromMercatorY(b[1]) * kPi / 180;
  const double dl = (b[0] - a[0]) * 2 * kPi;
  const double s = std::sin((lb - la) / 2), t = std::sin(dl / 2);
  return 2 * std::asin(std::min(1.0, std::sqrt(s * s + std::cos(la) * std::cos(lb) * t * t)));
}

struct Rig {
  Transform t;
  int zooms = 0;
  double zoomDelta = 0;
  Gestures g;
  Rig(bool globe, double zoom)
      : g(t, GestureHost{[this](double x, double y) -> std::optional<Point3> {
                           const auto w = t.unprojectExact(x, y);
                           if (!w) return std::nullopt;
                           return Point3{(*w)[0], (*w)[1], 0};
                         },
                         [] {}, [] {},
                         [this](double d, double, double) {
                           zooms++;
                           zoomDelta += d;
                         }}) {
    t.resize(800, 1236);
    t.setZoomRange(-2, 22);
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

TEST("gestures: dragging over the north pole turns the globe without jumping") {
  Rig r(true, 1.6);
  r.t.resize(600, 927);
  r.t.setCenter(-100, 70);
  double lng = r.t.center().lng, lat = r.t.center().lat, worst = 0, back = 0;
  r.g.down(1, 420, 460, 0);
  for (int i = 1; i <= 40; i++) {
    r.g.move(1, 420 - i, 460 + i * 11.5, i * 16.0);
    const auto c = r.t.center();
    double d = std::abs(c.lng - lng);
    if (d > 180) d = 360 - d;
    worst = std::max(worst, d);
    back = std::max(back, lat - c.lat);
    lng = c.lng;
    lat = c.lat;
  }
  EXPECT(worst < 15);
  EXPECT(back < 0.5);
}

TEST("gestures: a drag keeps the grabbed place under the finger") {
  Rig r(false, 10);
  const auto grabbed = r.t.unproject(300, 400);
  r.drag(1, 300, 400, 520, 610, 0);
  const auto now = r.t.unproject(520, 610);
  EXPECT(grabbed && now);
  if (grabbed && now) EXPECT(std::abs((*grabbed)[0] - (*now)[0]) < 1e-9 && std::abs((*grabbed)[1] - (*now)[1]) < 1e-9);
}

TEST("gestures: a globe drag moves the place at the centre with the finger") {
  Rig r(true, 2.5);
  const auto grabbed = r.t.unproject(400, 618);
  r.drag(1, 400, 618, 460, 658, 0);
  EXPECT(grabbed.has_value());
  if (grabbed) {
    const ScreenPoint p = r.t.project((*grabbed)[0], (*grabbed)[1]);
    EXPECT(std::hypot(p.x - 460, p.y - 658) < 4.5);
  }
}

TEST("gestures: a globe drag in a rotated view still moves the place at the centre with the finger") {
  Rig r(true, 2.5);
  r.t.setBearing(90);
  const auto grabbed = r.t.unproject(400, 618);
  r.drag(1, 400, 618, 440, 578, 0);
  EXPECT(grabbed.has_value());
  if (grabbed) {
    const ScreenPoint p = r.t.project((*grabbed)[0], (*grabbed)[1]);
    EXPECT(std::hypot(p.x - 440, p.y - 578) < 2);
  }
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

TEST("gestures: dragging the globe sideways spins it without drifting or jumping") {
  int moves = 0, drifts = 0;
  double worst = 0, sizeChange = 0;
  for (double zoom : {0.0, 1.8, 2.5, 4.0, 8.0}) {
    for (double lat : {-60.0, 0.0, 30.0, 60.0, 75.0}) {
      for (double pitch : {0.0, 40.0}) {
        for (double bearing : {0.0, 30.0}) {
          for (double speed : {6.0, 30.0}) {
            for (double fy : {0.3, 0.7}) {
              for (int dir : {-1, 1}) {
                Rig r(true, zoom);
                r.t.setPitch(pitch);
                r.t.setBearing(bearing);
                r.t.setCenter(0, lat);
                const double radius = r.t.globeRadius();
                double x = dir > 0 ? 100 : 700, time = 0;
                const double y = 1236 * fy;
                r.g.down(1, x, y, time);
                for (int i = 1; i <= 30; i++) {
                  const World before = r.t.centerWorld();
                  const double nx = x + dir * speed;
                  time += 16;
                  r.g.move(1, nx, y, time);
                  if (i > 1 && r.g.active() && r.t.zoom() > r.t.minZoom + 1e-6) {
                    moves++;
                    worst = std::max(worst, turn(before, r.t.centerWorld()) / (speed / r.t.globeRadius()));
                    if (bearing == 0 && std::abs(r.t.centerWorld()[1] - before[1]) > 1e-12) drifts++;
                    sizeChange = std::max(sizeChange, std::abs(r.t.globeRadius() / radius - 1));
                  }
                  x = nx;
                  if (x < 20 || x > 780) break;
                }
                r.g.up(1, x, y, time + 16, true);
              }
            }
          }
        }
      }
    }
  }
  EXPECT(moves > 5000);
  EXPECT(worst < 1.1);
  EXPECT(drifts == 0);
  EXPECT(sizeChange < 1e-9);
}

TEST("gestures: pan statistics count globe moves and flag no wild turns on a plain drag") {
  Rig r(true, 1.8);
  r.t.setCenter(10, 20);
  r.drag(1, 250, 600, 560, 640, 0, 30);
  r.g.up(1, 560, 640, 30 * 16 + 4, false);
  for (int i = 0; i < 60 && r.g.animating(); i++) r.g.step(nowMs() + i * 16);
  const PanStats s = r.g.takePanStats();
  EXPECT(s.moves >= 29);
  EXPECT(s.wild == 0);
  EXPECT(s.maxSlipPx < 1);
  EXPECT(s.maxTurnRatio < 4.6);
  EXPECT(r.g.takePanStats().moves == 0);
}

TEST("gestures: a drag that starts off the globe still turns it") {
  Rig r(true, 1.8);
  r.t.setCenter(0, 0);
  const double edge = 618 - r.t.globeRadius();
  EXPECT(!r.t.unproject(400, edge - 60));
  r.g.down(1, 400, edge - 60, 0);
  const World start = r.t.centerWorld();
  for (int i = 1; i <= 20; i++) r.g.move(1, 400, edge - 60 + i * 10, i * 16.0);
  EXPECT(r.g.active());
  EXPECT(turn(start, r.t.centerWorld()) > 5 * kPi / 180);
  EXPECT(r.t.center().lat > 0);
}

TEST("gestures: a flat drag that starts above the horizon grabs the map once the finger reaches it") {
  Rig r(false, 12);
  r.t.setPitchRange(0, 85);
  r.t.setPitch(80);
  double top = 0;
  while (top < 1236 && !r.t.unproject(400, top)) top += 4;
  EXPECT(top > 0 && top < 1236);
  r.g.down(1, 400, top - 40, 0);
  for (int i = 1; i <= 10; i++) r.g.move(1, 400, top - 40 + i * 12, i * 16.0);
  EXPECT(r.g.active());
  const auto under = r.t.unproject(400, top + 80);
  r.g.move(1, 400, top + 120, 11 * 16.0);
  EXPECT(under.has_value());
  if (under) {
    const ScreenPoint p = r.t.project((*under)[0], (*under)[1]);
    EXPECT(std::hypot(p.x - 400, p.y - (top + 120)) < 1.5);
  }
}
