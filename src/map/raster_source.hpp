#pragma once

#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "formats/image.hpp"
#include "map/source.hpp"
#include "map/tileset.hpp"
#include "render/gpu.hpp"

namespace mapkern {

struct RasterTileData {
  std::optional<Image> image;
  MipChain mips;
  GpuTexture texture;
  int width = 0;
  int height = 0;
};

class Placeholders {
 public:
  explicit Placeholders(std::function<void(int, int, int)> forget) : forget_(std::move(forget)) {}
  std::optional<uint32_t> note(TileId tile, const Bytes& data);
  bool suspect(TileId tile, uint32_t hash);
  void confirm(uint32_t hash, TileId tile);

 private:
  std::mutex mutex_;
  std::list<std::pair<uint64_t, uint32_t>> keys_;
  std::unordered_map<uint32_t, std::unordered_map<uint64_t, int>> twins_;
  std::unordered_set<uint32_t> known_;
  std::function<void(int, int, int)> forget_;
};

bool isFlatImage(const Image& image);

class RasterSource : public Source {
 public:
  RasterSource(std::string id, Value spec, SourceContext& ctx, std::string type = "raster");
  ~RasterSource() override;
  bool loaded() const override { return manager != nullptr; }
  void setCacheBytes(size_t bytes) override {
    Source::setCacheBytes(bytes);
    if (manager) manager->options.cacheBytes = bytes;
  }
  void load() override;
  void update(Transform& transform, double now, const std::vector<Transform*>& predicted) override;
  bool uploadPending(UploadBudget& budget, double now) override;
  bool isIdle() const override;
  bool isFading() const override { return manager && manager->isFading(); }
  std::optional<TileStats> stats() const override;
  void reload() override;
  std::string attribution() const override;
  void retryFailed() override;
  double tileSize() const;
  void setFadeDuration(double ms);
  std::unique_ptr<TileManager<RasterTileData>> manager;
  std::optional<TileSet> tileSet;

 private:
  void start(TileSet set);
  std::shared_ptr<TileLoader<RasterTileData>> loader_;
  CancelToken lifetime_;
  double fade_;
};

}
