#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "render/gpu.hpp"
#include "ui/canvas.hpp"

namespace mapkern::ui {

struct Basemap {
  std::string name;
  std::string style;
};

struct OverlayState {
  double width = 0, height = 0, pixelRatio = 1;
  double zoom = 0, minZoom = 0, maxZoom = 22, bearing = 0, pitch = 0;
  std::string view = "terrain";
  std::string attribution;
  double metersPerMaxWidth = 0;
  std::vector<std::string> hud;
};

struct OverlayActions {
  std::function<void()> zoomIn, zoomOut, resetNorth;
  std::function<void(size_t)> selectBasemap;
  std::function<void(const std::string&)> setView;
};

struct ScaleBar {
  std::string label;
  double meters = 0;
  double width = 0;
};

ScaleBar scaleBar(double meters, double maxWidth, bool imperial);
std::string imperialLabel(double meters);
std::string plainAttribution(const std::string& html);

class Overlay {
 public:
  bool touch = true;
  std::vector<Basemap> basemaps;
  size_t selected = 0;
  std::vector<std::string> views{"terrain", "tilted", "globe"};
  OverlayActions actions;

  void update(const OverlayState& state);
  bool pointerDown(int id, double x, double y);
  bool pointerMove(int id, double x, double y);
  bool pointerUp(int id, double x, double y);
  void pointerCancel(int id) { pressed_.erase(id); }
  int draw(const PassFormat& format, double width, double height);
  void resetGpu(bool release);
  bool menuOpen() const { return menu_; }
  bool contains(double x, double y) const { return hit(x, y, nullptr) != nullptr; }
  void setPendingView(const std::string& view) { pending_ = view; }

 private:
  struct Element {
    std::string id;
    double x = 0, y = 0, w = 0, h = 0;
    double pad = 0;
    std::string key;
    std::function<void(Canvas&, double, double)> paint;
    std::function<void(size_t)> click;
    std::vector<std::array<double, 5>> hits;
  };
  struct Cached {
    std::string key;
    GpuTexture texture;
    int pw = 0, ph = 0;
  };
  void layout(const OverlayState& state);
  const Element* hit(double x, double y, size_t* part) const;
  std::vector<Element> elements_;
  std::map<std::string, Cached> cache_;
  OverlayState state_;
  bool menu_ = false;
  std::string pending_;
  std::map<int, std::pair<std::string, size_t>> pressed_;
};

}
