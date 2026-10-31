#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/async.hpp"
#include "core/value.hpp"
#include "geo/tile.hpp"
#include "tiles/cache.hpp"
#include "tiles/fetch.hpp"

namespace mapkern {

struct VectorLayerInfo {
  std::string id;
  Object fields;
  std::optional<double> minZoom, maxZoom;
};

struct TileSet {
  std::vector<std::string> tiles;
  int minZoom = 0;
  int maxZoom = 22;
  std::optional<Rect> bounds;
  bool tms = false;
  std::string attribution;
  std::vector<VectorLayerInfo> vectorLayers;
  Value raw;
};

struct TileSetDefaults {
  int minZoom = 0;
  int maxZoom = 22;
};

using TileSetResult = std::variant<TileSet, std::string>;

void resolveTileSet(const std::string& sourceId, const Value& spec, TileSetDefaults defaults, const TransformRequest& transform,
                    int timeoutMs, std::shared_ptr<ResponseStore> store, CancelToken cancel, std::function<void(TileSetResult)> done);
std::string tileUrl(const TileSet& set, int z, int x, int y);
std::optional<Rect> boundsRect(const Value& value);
Value archiveSpec(const Value& spec);
void registerArchiveProtocols();

}
