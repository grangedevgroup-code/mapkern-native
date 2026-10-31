#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "formats/mvt.hpp"
#include "style/expression.hpp"
#include "style/layer.hpp"
#include "worker/buckets.hpp"

namespace mapkern {

enum class BucketType { Fill, Line, Circle, Heatmap, Symbol, Extrusion };

struct LayerProgram {
  std::string id;
  BucketType type = BucketType::Fill;
  std::optional<std::string> sourceLayer;
  std::optional<Compiled> filter;
  std::optional<Compiled> sortKey;
  double minZoom = 0;
  double maxZoom = 24;
  std::vector<std::string> keys;
  LineOptions line;
  std::optional<std::pair<Compiled, Compiled>> outlines;
  Value symbolLayout;
  std::string signature;
};

struct FeatureRecord {
  std::optional<double> id;
  std::string textId;
  uint32_t source = 0;
  int type = 0;
  Object properties;
};

struct FeatureTable {
  std::vector<FeatureRecord> features;
  size_t count() const { return features.size(); }
};

struct BucketData {
  std::string layerId;
  BucketType type = BucketType::Fill;
  uint32_t extent = 4096;
  FeatureTable features;
  std::optional<Geometry> main;
  std::optional<Geometry> outline;
  std::shared_ptr<void> labels;
};

struct VectorTileResult {
  std::vector<BucketData> buckets;
  std::vector<std::string> layers;
  size_t featureCount = 0;
  double decodeMs = 0;
};

using PromoteId = std::variant<std::monostate, std::string, std::vector<std::pair<std::string, std::string>>>;

std::optional<LayerProgram> layerProgram(const StyleLayer& layer);
VectorTileResult buildVectorTile(const Bytes& bytes, int z, int x, int y, const std::vector<LayerProgram>& programs, bool overzoom,
                                 const PromoteId& promote);

class FeatureSource {
 public:
  virtual ~FeatureSource() = default;
  virtual const std::string& name() const = 0;
  virtual uint32_t extent() const = 0;
  virtual size_t size() const = 0;
  virtual int type(size_t i) const = 0;
  virtual std::optional<double> id(size_t i) const = 0;
  virtual std::string textId(size_t i) const { return {}; }
  virtual uint32_t sourceIndex(size_t i) const { return static_cast<uint32_t>(i); }
  virtual const Object& properties(size_t i) const = 0;
  virtual std::vector<Ring> geometry(size_t i) const = 0;
};

VectorTileResult buildBuckets(const std::vector<std::shared_ptr<FeatureSource>>& layers, int z, int x, int y,
                              const std::vector<LayerProgram>& programs, bool overzoom);

using SymbolBuilder = std::function<std::shared_ptr<void>(const LayerProgram& program, const FeatureSource& source,
                                                          const std::vector<size_t>& selected, int z, FeatureTable& table)>;
void setSymbolBuilder(SymbolBuilder builder);

}
