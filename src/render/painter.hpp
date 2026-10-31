#pragma once

#include <map>
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
  int width = 0;
  int height = 0;
};

struct FrameInput {
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
  int layers = 0;
  double cpuMs = 0;
};

class Painter {
 public:
  bool render(const FrameInput& frame, const FrameTarget& target);
  const PaintStats& stats() const { return stats_; }

 private:
  void drawLayer(const FrameInput& frame, const StyleLayer& layer);
  void drawBackground(const FrameInput& frame, const StyleLayer& layer);
  void drawRaster(const FrameInput& frame, const StyleLayer& layer, RasterSource& source);
  void drawVector(const FrameInput& frame, const StyleLayer& layer, VectorSource& source);
  void drawMasks(const FrameInput& frame, const std::string& sourceId, const std::vector<RenderPiece<VectorTileData>>& pieces);
  void drawFill(const FrameInput& frame, const StyleLayer& layer, VectorBucket& bucket, const RenderPiece<VectorTileData>& piece);
  void drawLine(const FrameInput& frame, const StyleLayer& layer, VectorBucket& bucket, const RenderPiece<VectorTileData>& piece);
  void drawCircle(const FrameInput& frame, const StyleLayer& layer, VectorBucket& bucket, const RenderPiece<VectorTileData>& piece);
  void drawExtrusion(const FrameInput& frame, const StyleLayer& layer, VectorBucket& bucket, const RenderPiece<VectorTileData>& piece);
  sg_pipeline pipeline(Program program, sg_primitive_type primitive = SG_PRIMITIVETYPE_TRIANGLES, StencilMode stencil = StencilMode::Equal,
                       DepthMode depth = DepthMode::Off, bool colorWrite = true, Blend blend = Blend::Premultiplied);
  void bindPaint(sg_bindings& b, const PaintTable& paint);

  PassFormat format_;
  std::string maskSource_;
  int maskOffset_ = 0;
  std::unordered_map<const void*, int> maskRefs_;
  PaintStats stats_;
};

}
