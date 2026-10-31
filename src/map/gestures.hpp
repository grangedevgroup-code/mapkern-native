#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "camera/transform.hpp"

namespace mapkern {

using Point3 = std::array<double, 3>;

struct GestureHost {
  std::function<std::optional<Point3>(double, double)> anchor;
  std::function<void()> interrupt;
  std::function<void()> changed;
  std::function<void(double, double, double)> zoomBy;
};

struct GestureOptions {
  bool dragPan = true;
  bool touchZoomRotate = true;
  bool touchRotate = true;
  bool touchPitch = true;
  bool doubleTapZoom = true;
  double clickTolerance = 4;
};

class Gestures {
 public:
  Gestures(Transform& transform, GestureHost host);

  void down(int id, double x, double y, double time);
  void move(int id, double x, double y, double time);
  void up(int id, double x, double y, double time, bool cancelled);
  void wheel(double zoomDelta, double x, double y, double now);
  bool step(double now);
  void stopMomentum();
  void cancel();

  bool touching() const { return !pointers_.empty(); }
  bool animating() const { return inertia_.has_value() || wheel_.has_value(); }
  bool active() const { return mode_.kind != Kind::Idle && mode_.kind != Kind::Press; }

  GestureOptions options;

 private:
  enum class Kind { Idle, Press, Quick, Pan, Touch2 };
  enum class Sub { Undecided, Pinch, Pitch };

  struct Pointer {
    int id = 0;
    double x = 0, y = 0, startX = 0, startY = 0, startTime = 0;
  };
  struct TwoFinger {
    double dist = 1, angle = 0, midX = 0, midY = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  };
  struct Mode {
    Kind kind = Kind::Idle;
    int pointer = -1;
    bool quick = false;
    bool moved = false;
    bool fling = false;
    double startY = 0, zoom0 = 0, bearing0 = 0, pitch0 = 0, x = 0, y = 0, started = 0;
    std::optional<Point3> anchor;
    std::array<int, 2> ids{-1, -1};
    TwoFinger start;
    Sub sub = Sub::Undecided;
    std::optional<double> turnStart;
  };
  struct Sample {
    double t = 0, x = 0, y = 0;
  };
  struct Inertia {
    double start = 0, duration = 0;
    std::array<double, 2> from{0, 0}, offset{0, 0};
    Point3 anchor{0, 0, 0};
  };
  struct WheelZoom {
    double target = 0;
    std::array<double, 2> screen{0, 0};
    std::optional<Point3> world;
    double last = 0;
  };
  struct Tap {
    double t = 0, x = 0, y = 0;
  };

  void startInertia(const Point3& anchor, double x, double y, double time);
  TwoFinger twoFinger(const Pointer& a, const Pointer& b) const;
  void beginTwoFinger(const Pointer& a, const Pointer& b, double time);
  void moveTwoFinger();
  void detectDoubleTap(double t, double x, double y);
  void place(const Point3& anchor, double x, double y, bool keepSize);
  void follow(std::optional<Point3>& anchor, double x, double y);

  Transform& transform_;
  GestureHost host_;
  std::map<int, Pointer> pointers_;
  Mode mode_;
  std::vector<Sample> samples_;
  std::optional<Inertia> inertia_;
  std::optional<WheelZoom> wheel_;
  std::optional<Tap> lastTap_;
};

}
