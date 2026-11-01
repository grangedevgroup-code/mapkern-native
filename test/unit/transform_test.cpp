#include <algorithm>
#include <cmath>

#include "camera/transform.hpp"
#include "check.hpp"

using namespace mapkern;

static Transform make(double zoom, double pitch = 0, double bearing = 0, double w = 800, double h = 600) {
  Transform t;
  t.resize(w, h);
  t.setPitchRange(0, 85);
  t.setZoom(zoom);
  t.setPitch(pitch);
  t.setBearing(bearing);
  t.setCenter(-122.33, 47.6);
  return t;
}

TEST("transform: center projects to the viewport middle") {
  for (double pitch : {0.0, 30.0, 60.0}) {
    for (double bearing : {0.0, 45.0, -120.0}) {
      auto t = make(12, pitch, bearing);
      const auto p = t.project(mercatorX(-122.33), mercatorY(47.6));
      EXPECT_NEAR(p.x, 400, 1e-6);
      EXPECT_NEAR(p.y, 300, 1e-6);
    }
  }
}

TEST("transform: one world pixel is one screen pixel at pitch 0") {
  auto t = make(10);
  const auto c = t.centerWorld();
  const double ws = t.worldSize();
  const auto a = t.project(c[0], c[1]);
  const auto b = t.project(c[0] + 100 / ws, c[1]);
  EXPECT_NEAR(b.x - a.x, 100, 1e-6);
  EXPECT_NEAR(b.y - a.y, 0, 1e-6);
}

TEST("transform: bearing 90 puts east at the top") {
  auto t = make(8, 0, 90);
  const auto c = t.centerWorld();
  const auto p = t.project(c[0] + 0.001, c[1]);
  EXPECT_NEAR(p.x, 400, 1e-6);
  EXPECT(p.y < 300);
}

TEST("transform: unproject inverts project") {
  for (double pitch : {0.0, 40.0, 60.0}) {
    auto t = make(14, pitch, 33);
    const double pts[4][2] = {{10, 590}, {400, 300}, {790, 300}, {200, 450}};
    for (const auto& q : pts) {
      const auto w = t.unproject(q[0], q[1]);
      EXPECT(w.has_value());
      if (!w) continue;
      const auto p = t.project((*w)[0], (*w)[1]);
      EXPECT_NEAR(p.x, q[0], 1e-5);
      EXPECT_NEAR(p.y, q[1], 1e-5);
    }
  }
}

TEST("transform: globe unproject inverts project") {
  auto t = make(3, 20, 10);
  t.setProjection(Projection::Globe);
  const auto w = t.unproject(420, 310);
  EXPECT(w.has_value());
  if (w) {
    const auto p = t.project((*w)[0], (*w)[1]);
    EXPECT_NEAR(p.x, 420, 1e-4);
    EXPECT_NEAR(p.y, 310, 1e-4);
  }
}

TEST("transform: an exact globe pick lands on its pixel, polar caps included") {
  for (double pitch : {0.0, 40.0}) {
    Transform t;
    t.resize(800, 1236);
    t.setZoomRange(-2, 22);
    t.setProjection(Projection::Globe);
    t.setZoom(1.8);
    t.setPitch(pitch);
    t.setBearing(30);
    t.setCenter(0, -60);
    double worst = 0;
    int caps = 0;
    for (double y = 20; y < 1236; y += 20)
      for (double x = 10; x < 800; x += 20) {
        const auto w = t.unprojectExact(x, y);
        if (!w) continue;
        if ((*w)[1] > 1) caps++;
        const auto p = t.project((*w)[0], (*w)[1]);
        worst = std::max(worst, std::hypot(p.x - x, p.y - y));
      }
    EXPECT(caps > 0);
    EXPECT(worst < 1e-3);
  }
}

TEST("transform: covering tiles reach the centre zoom and are sorted") {
  auto t = make(12.4, 45, 20);
  CoverOptions o;
  o.maxZoom = 14;
  const auto tiles = t.coveringTiles(o);
  EXPECT(!tiles.empty());
  EXPECT(tiles.front().z == 12);
  for (size_t i = 1; i < tiles.size(); i++) EXPECT(tiles[i - 1].distance <= tiles[i].distance);
}

TEST("transform: globe covering at zoom 0 sees about half the world") {
  auto t = make(0.5);
  t.setProjection(Projection::Globe);
  CoverOptions o;
  o.maxZoom = 14;
  const auto tiles = t.coveringTiles(o);
  EXPECT(!tiles.empty());
  EXPECT(tiles.size() < 64);
}

TEST("transform: convex hull keeps corners") {
  const auto hull = convexHull({{0, 0}, {1, 0}, {0.5, 0.5}, {1, 1}, {0, 1}});
  EXPECT(hull.size() == 4);
  EXPECT(polygonIntersectsRect(hull, 0.9, 0.9, 2, 2));
  EXPECT(!polygonIntersectsRect(hull, 1.1, 1.1, 2, 2));
}
