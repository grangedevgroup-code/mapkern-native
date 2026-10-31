#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/value.hpp"

namespace mapkern {

using Ring = std::vector<int32_t>;

struct VectorFeature {
  std::optional<double> id;
  int type = 0;
  uint32_t extent = 4096;
  Object properties() const;
  const Value* property(std::string_view key) const;
  std::vector<Ring> geometry() const;

  const uint8_t* buf = nullptr;
  size_t tagStart = 0, tagEnd = 0, geomStart = 0, geomEnd = 0;
  bool hasTags = false, hasGeometry = false;
  const std::vector<std::string>* keys = nullptr;
  const std::vector<Value>* values = nullptr;
};

class VectorLayer {
 public:
  VectorLayer(std::shared_ptr<const std::vector<uint8_t>> data, size_t start, size_t end);
  std::string name;
  uint32_t extent = 4096;
  uint32_t version = 1;
  size_t size() const { return features_.size(); }
  VectorFeature feature(size_t index) const;

 private:
  std::shared_ptr<const std::vector<uint8_t>> data_;
  std::vector<std::pair<size_t, size_t>> features_;
  std::vector<std::string> keys_;
  std::vector<Value> values_;
};

using VectorTile = std::unordered_map<std::string, std::shared_ptr<VectorLayer>>;

VectorTile decodeVectorTile(std::shared_ptr<const std::vector<uint8_t>> bytes);
std::vector<Ring> decodeGeometry(const uint8_t* buf, size_t start, size_t end, int type);
double ringArea(const Ring& ring);
std::vector<std::vector<Ring>> classifyRings(const std::vector<Ring>& rings);
std::optional<double> numericId(const Value& value);
std::optional<double> featureKey(const Value& value);

}
