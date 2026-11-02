#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "mapkern/window.hpp"

namespace {

struct ArgReader {
  std::vector<std::string> items;
  size_t at = 0;
  bool more() const { return at < items.size(); }
  std::string next() { return at < items.size() ? items[at++] : std::string(); }
  double number() { return std::atof(next().c_str()); }
};

}

int main(int argc, char** argv) {
  mapkern::MapOptions options;
  options.controls = true;
  options.touchControls = false;
  int exitAfter = 0;
  ArgReader r{{argv + 1, argv + argc}};
  while (r.more()) {
    const std::string a = r.next();
    if (a == "--style") options.style = r.next();
    else if (a == "--center") {
      options.lng = r.number();
      options.lat = r.number();
    } else if (a == "--zoom") options.zoom = r.number();
    else if (a == "--pitch") options.pitch = r.number();
    else if (a == "--bearing") options.bearing = r.number();
    else if (a == "--flat") {
      options.globe = false;
      options.view = "terrain";
    } else if (a == "--view") options.view = r.next();
    else if (a == "--no-controls") options.controls = false;
    else if (a == "--touch") options.touchControls = true;
    else if (a == "--offline") options.offline = true;
    else if (a == "--cache") options.cacheDirectory = r.next();
    else if (a == "--exit-after") exitAfter = static_cast<int>(r.number());
  }
  mapkern::WindowOptions window;
  if (exitAfter > 0) {
    const auto started = std::chrono::steady_clock::now();
    window.frame = [started, exitAfter, frames = 0, done = false](mapkern::Map& map) mutable {
      frames++;
      if (done || std::chrono::steady_clock::now() - started <= std::chrono::milliseconds(exitAfter)) return !done;
      done = true;
      const auto s = map.stats();
      std::printf("frames=%d tiles=%d loading=%d blank=%.3f draws=%d frame=%.2fms cache=%llu\n", frames, s.tiles, s.loading, s.blank, s.drawCalls, s.frameMs,
                  static_cast<unsigned long long>(s.diskCacheEntries));
      return false;
    };
  }
  return mapkern::runWindow(options, window);
}
