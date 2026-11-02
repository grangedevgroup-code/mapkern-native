#pragma once

#include <memory>
#include <optional>
#include <string>

#include "map/vector_source.hpp"
#include "worker/geojson.hpp"

namespace mapkern {

class GeoJsonTileLoader;

class GeoJsonSource : public VectorSource {
 public:
  GeoJsonSource(std::string id, Value spec, SourceContext& ctx);
  ~GeoJsonSource() override;
  void load() override;
  bool isIdle() const override;
  void setData(Value data);
  void setDataText(std::string json);
  const std::shared_ptr<GeoJsonIndex>& index() const { return index_; }

 private:
  friend class GeoJsonTileLoader;
  GeoJsonOptions options() const;
  void sync();
  void apply(std::shared_ptr<GeoJsonIndex> index);
  std::shared_ptr<GeoJsonIndex> index_;
  std::shared_ptr<GeoJsonTileLoader> geoJsonLoader_;
  std::optional<std::string> text_;
  uint64_t version_ = 1;
  uint64_t applied_ = 0;
  bool syncing_ = false;
  bool started_ = false;
};

}
