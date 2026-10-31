#pragma once

#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

#include "camera/transform.hpp"
#include "map/raster_source.hpp"
#include "map/source.hpp"
#include "map/vector_source.hpp"
#include "render/gpu.hpp"
#include "render/paint_table.hpp"
#include "style/layer.hpp"

namespace mapkern {

struct FrameTarget {
  sg_swapchain swapchain{};
  sg_attachments attachments{};
  bool offscreen = false;
  PassFormat format;
  int width = 0;
  int height = 0;
};

struct RenderView {
  double zoom = 0;
  double bearing = 0;
  double width = 1;
  double height = 1;
  std::function<std::array<float, 16>(int z, int x, int y, int wrap, double extent)> tileMatrix;
  std::optional<Rect> region;
  bool overlaps(int z, int x, int y, int wrap, double margin = 0) const;
};

struct DrapeEntry {
  sg_image image{};
  sg_view texture{};
  sg_view attachment{};
  int size = 0;
  uint64_t signature = 0;
  bool drawn = false;
  double zoom = 0;
  uint64_t used = 0;
  void release();
};

struct FrameInput {
  const RenderView* view = nullptr;
  Transform* transform = nullptr;
  const std::vector<std::unique_ptr<StyleLayer>>* layers = nullptr;
  const std::map<std::string, std::unique_ptr<Source>>* sources = nullptr;
  const FeatureStates* states = nullptr;
  double pixelRatio = 1;
  double now = 0;
  Color clearColor{0, 0, 0, 0};
};

struct PaintStats {
  int drawCalls = 0;
  int pieces = 0;
  int drapes = 0;
  int layers = 0;
  double cpuMs = 0;
};

class Painter {
 public:
  ~Painter();
  bool render(const FrameInput& frame, const FrameTarget& target);
  const PaintStats& stats() const { return stats_; }
  bool drapesBehind() const { return drapesBehind_; }

 private:
  void drawLayer(const FrameInput& frame, const StyleLayer& layer);
  void drawBackground(const FrameInput& frame, const StyleLayer& layer);
  void drawRaster(const FrameInput& frame, const StyleLayer& layer, RasterSource& source);
  void drawVector(const FrameInput& frame, const StyleLayer& layer, VectorSource& source);
  static bool clipped(const RenderPiece<VectorTileData>& piece);
  void applyClip(const RenderPiece<VectorTileData>& piece, double extent);
  void drawFill(const FrameInput& frame, const StyleLayer& layer, VectorBucket& bucket, const RenderPiece<VectorTileData>& piece);
  void drawLine(const FrameInput& frame, const StyleLayer& layer, VectorBucket& bucket, const RenderPiece<VectorTileData>& piece);
  void drawCircle(const FrameInput& frame, const StyleLayer& layer, VectorBucket& bucket, const RenderPiece<VectorTileData>& piece);
  void drawExtrusion(const FrameInput& frame, const StyleLayer& layer, VectorBucket& bucket, const RenderPiece<VectorTileData>& piece);
  sg_pipeline pipeline(Program program, sg_primitive_type primitive = SG_PRIMITIVETYPE_TRIANGLES, StencilMode stencil = StencilMode::Off,
                       DepthMode depth = DepthMode::Off, bool colorWrite = true, Blend blend = Blend::Premultiplied);
  void bindPaint(sg_bindings& b, const PaintTable& paint);

  void renderDrapes(const FrameInput& frame, const std::vector<CoveringTile>& slots);
  void renderPoles(const std::vector<CoveringTile>& slots);
  void drawAtmosphere(const FrameInput& frame);
  void drawGlobe(const FrameInput& frame, const std::vector<CoveringTile>& slots, const Color& base);
  std::pair<const DrapeEntry*, std::array<float, 4>> slotDrape(const CoveringTile& slot);
  void ensureMeshes();

  PassFormat format_;
  PaintStats stats_;
  std::unordered_map<uint64_t, DrapeEntry> drapes_;
  uint64_t frameCount_ = 0;
  bool drapesBehind_ = false;
  GpuBuffer meshVertices_, meshIndices_, capVertices_, capIndices_;
  uint32_t meshCount_ = 0, capCount_ = 0;
  sg_image poleImage_{};
  sg_view poleTexture_{}, poleAttachment_{};
  GpuTexture flatDem_;
};

}
