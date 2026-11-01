#pragma once

#include <memory>
#include <optional>

#include "map/source.hpp"
#include "map/tileset.hpp"
#include "render/gpu.hpp"
#include "terrain/dem.hpp"

namespace mapkern {

struct DemTileData {
  int z = 0, x = 0, y = 0;
  DemGrid grid;
  std::vector<uint16_t> gradient;
  GpuTexture gradientTexture;
  GpuTexture maskTexture;
  GpuTexture elevationTexture;
  bool elevationDirty = false;
  bool gradientDirty = false;
  uint64_t version = 0;
};

class DemSource : public Source {
 public:
  DemSource(std::string id, Value spec, SourceContext& ctx);
  ~DemSource() override;
  bool loaded() const override { return manager != nullptr; }
  void setCacheBytes(size_t bytes) override {
    Source::setCacheBytes(bytes);
    if (manager) manager->options.cacheBytes = bytes;
  }
  size_t cachedBytes() const override { return manager ? manager->cachedBytes() : 0; }
  void evictionCandidates(std::vector<TileCandidate>& out, size_t owner) const override {
    if (manager) manager->evictionCandidates(out, owner);
  }
  void dropTile(uint64_t key) override {
    if (manager) manager->dropTile(key);
  }
  void load() override;
  void update(Transform& transform, double now, const std::vector<Transform*>& predicted) override;
  bool uploadPending(UploadBudget& budget, double now) override;
  bool isIdle() const override;
  bool isFading() const override { return manager && manager->isFading(); }
  std::optional<TileStats> stats() const override;
  void reload() override;
  void retryFailed() override;
  std::string attribution() const override;
  double tileSize() const;
  const GpuTexture* elevationTexture(DemTileData& data);
  const GpuTexture* gradientTexture(DemTileData& data);
  std::optional<double> elevationAt(double worldX, double worldY) const;
  void stitch(DemTileData& data);
  std::unique_ptr<TileManager<DemTileData>> manager;
  std::optional<TileSet> tileSet;

 private:
  void start(TileSet set);
  bool refresh(DemTileData& data);
  void resize(DemTileData& data);
  std::shared_ptr<TileLoader<DemTileData>> loader_;
  CancelToken lifetime_;
  Unpack unpack_;
  std::vector<TileId> stale_;
};

}
