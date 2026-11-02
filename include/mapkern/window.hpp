#pragma once

#include <functional>
#include <string>

#include "mapkern/map.hpp"

namespace mapkern {

struct WindowOptions {
  std::string title = "Mapkern";
  int width = 1280;
  int height = 800;
  std::function<void(Map&)> ready;
  std::function<bool(Map&)> frame;
};

int runWindow(const MapOptions& options, const WindowOptions& window = {});

}
