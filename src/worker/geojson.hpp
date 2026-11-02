#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/value.hpp"
#include "geo/tile.hpp"
#include "worker/vector_task.hpp"

namespace mapkern {

struct GeoJsonOptions {
  int maxZoom = 18;
  double tolerance = 0.375;
  double buffer = 64;
  uint32_t extent = 4096;
  std::string promoteId;
  bool generateId = false;
};

struct GeoJsonFeature {
  std::optional<double> id;
  std::string textId;
  uint32_t index = 0;
  Object properties;
};

struct GeoJsonPart {
  std::vector<double> points;
  bool outer = true;
  double size = 0;
};

struct GeoJsonItem {
  std::shared_ptr<const GeoJsonFeature> feature;
  int type = 1;
  std::vector<GeoJsonPart> parts;
  Rect bounds;
  size_t vertices = 0;
};

using GeoJsonItemPtr = std::shared_ptr<const GeoJsonItem>;

class GeoJsonIndex {
 public:
  GeoJsonIndex(const Value& data, GeoJsonOptions options);

  std::shared_ptr<FeatureSource> tile(int z, int x, int y);
  std::vector<GeoJsonItemPtr> itemsOf(uint32_t index) const;
  size_t featureCount() const { return features_; }
  size_t vertexCount() const { return root_->vertices; }
  const std::optional<Rect>& coverage() const { return coverage_; }
  const GeoJsonOptions& options() const { return options_; }

 private:
  struct Node {
    std::vector<GeoJsonItemPtr> items;
    size_t vertices = 0;
    size_t cost = 0;
  };
  using NodePtr = std::shared_ptr<const Node>;
  struct Cached {
    NodePtr node;
    std::list<uint64_t>::iterator order;
  };

  NodePtr node(int z, int x, int y);
  NodePtr touch(uint64_t key);
  NodePtr derive(const Node& parent, int z, int x, int y);
  void trim();

  GeoJsonOptions options_;
  NodePtr root_;
  std::vector<GeoJsonItemPtr> originals_;
  size_t features_ = 0;
  std::optional<Rect> coverage_;
  std::mutex mutex_;
  std::unordered_map<uint64_t, Cached> cache_;
  std::list<uint64_t> order_;
  size_t budget_ = 0;
  size_t cachedCost_ = 0;
};

Value geoJsonGeometry(const std::vector<GeoJsonItemPtr>& items);

}
