#include <cstdio>

#include "mapkern/window.hpp"

int main() {
  mapkern::MapOptions options;
  options.lng = -122.33;
  options.lat = 47.61;
  options.zoom = 11;
  options.controls = true;

  mapkern::WindowOptions window;
  window.title = "My map";
  window.ready = [](mapkern::Map& map) {
    map.addSource("places", R"({
      "type": "geojson",
      "data": {
        "type": "Feature",
        "properties": { "name": "Space Needle" },
        "geometry": { "type": "Point", "coordinates": [-122.3493, 47.6205] }
      }
    })");
    map.addLayer(R"({
      "id": "dots",
      "type": "circle",
      "source": "places",
      "paint": { "radius": 8, "color": "#d01f4f", "strokeWidth": 2, "strokeColor": "#ffffff" }
    })");
    map.on([&map](const mapkern::MapEvent& event) {
      if (event.type != "click") return;
      std::printf("clicked %f, %f: %s\n", event.lng, event.lat, map.queryRenderedFeatures(event.x, event.y, {"dots"}).c_str());
    });
  };
  return mapkern::runWindow(options, window);
}
