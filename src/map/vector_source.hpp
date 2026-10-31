#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "map/source.hpp"
#include "map/tileset.hpp"
#include "render/gpu.hpp"
#include "render/paint_table.hpp"
#include "worker/vector_task.hpp"

namespace mapkern {

struct GpuGeometry {
  GpuBuffer vertices;
  GpuBuffer indices;
  uint32_t indexCount = 0;
  bool sharedVertices = false;
};

struct VectorBucket {
  std::string layerId;
  BucketType type = BucketType::Fill;
  uint32_t extent = 4096;
  FeatureTable features;
  std::optional<GpuGeometry> main;
  std::optional<GpuGeometry> outline;
  std::optional<Geometry> cpuMain;
  std::optional<Geometry> cpuOutline;
  PaintTable paint;
  std::shared_ptr<void> labels;
};

struct VectorTileData {
  std::unordered_map<std::string, std::unique_ptr<VectorBucket>> buckets;
  bool uploaded = false;
  size_t bytes = 0;
  size_t featureCount = 0;
  SharedBytes raw;
};

class VectorTileLoader;

class VectorSource : public Source {
 public:
  VectorSource(std::string id, Value spec, SourceContext& ctx);
  ~VectorSource() override;
  bool loaded() const override { return manager != nullptr; }
  void load() override;
  void update(Transform& transform, double now, const std::vector<Transform*>& predicted) override;
  bool uploadPending(double deadline, double now) override;
  bool isIdle() const override;
  std::optional<TileStats> stats() const override;
  void reload() override;
  void setLayers(const std::vector<const StyleLayer*>& layers) override;
  std::string attribution() const override;
  void retryFailed() override;
  std::unique_ptr<TileManager<VectorTileData>> manager;
  std::optional<TileSet> tileSet;

 private:
  void start(TileSet set);
  std::shared_ptr<const std::vector<LayerProgram>> programs_;
  std::string signature_;
  std::shared_ptr<VectorTileLoader> loader_;
  CancelToken lifetime_;
};

}
