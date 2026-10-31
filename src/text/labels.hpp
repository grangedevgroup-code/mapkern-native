#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "camera/transform.hpp"
#include "map/vector_source.hpp"
#include "render/gpu.hpp"
#include "render/paint_table.hpp"
#include "style/layer.hpp"
#include "text/glyphs.hpp"
#include "worker/symbols.hpp"

namespace mapkern {

struct StyleImage {
  int width = 0, height = 0;
  double pixelRatio = 1;
  bool sdf = false;
  std::vector<uint8_t> rgba;
  int x = 0, y = 0;
};

class ImageAtlas {
 public:
  ImageAtlas();
  int width = 256, height = 256;
  std::vector<uint8_t> data;
  uint64_t generation = 0;
  uint64_t version = 0;
  bool dirty = true;
  bool has(const std::string& id) const { return images_.contains(id); }
  const StyleImage* get(const std::string& id) const;
  void add(const std::string& id, StyleImage image);
  void clear();

 private:
  bool place(StyleImage& image);
  std::unordered_map<std::string, StyleImage> images_;
  struct Shelf {
    int y, height, x;
  };
  std::vector<Shelf> shelves_;
  int bottom_ = 0;
};

struct SymbolLayerInput {
  const StyleLayer* layer = nullptr;
  TileManager<VectorTileData>* manager = nullptr;
};

struct LaidGlyph {
  GlyphMetrics m;
  double x = 0, y = 0;
  int line = 0;
};

struct Laid {
  std::vector<LaidGlyph> glyphs;
  double width = 0, height = 0;
  std::vector<double> lines;
};

struct IconPart {
  std::string id;
  double width = 0, height = 0, left = 0, top = 0, size = 1, rotate = 0;
  bool alignMap = false, alongLine = false, sdf = false;
  size_t segment = 0;
  Color color{0, 0, 0, 1}, halo{0, 0, 0, 0};
  double haloWidth = 0, opacity = 1, pixelRatio = 1;
};

struct LabelTerrain {
  std::function<double(double, double)> height;
  std::function<bool(double, double, double)> hidden;
};

struct Label {
  std::string key, dedupe;
  const StyleLayer* layer = nullptr;
  std::string text;
  bool line = false;
  double wx = 0, wy = 0, wz = 0;
  std::vector<double> path;
  std::vector<double> heights;
  double anchorDist = 0;
  std::shared_ptr<const Laid> laid;
  double scale = 1;
  bool ground = false;
  double size = 14;
  Color color{0, 0, 0, 1}, halo{0, 0, 0, 0};
  double haloWidth = 0, haloBlur = 0;
  double opacity = 0;
  double target = 1;
  std::vector<double> boxes;
  bool showText = true;
  std::optional<IconPart> icon;
  std::string anchor = "center";
  double offsetX = 0, offsetY = 0, justify = 0.5, rotate = 0;
  uint64_t tileKey = 0;
};

class LabelRenderer {
 public:
  GlyphAtlas atlas;
  ImageAtlas images;
  bool update(Transform& transform, const std::vector<SymbolLayerInput>& inputs, double now, uint64_t cameraVersion, double pixelRatio,
              const LabelTerrain* terrain = nullptr);
  int draw(Transform& transform, double pixelRatio, const PassFormat& format);
  void setGlyphs(const std::string& tmpl, std::shared_ptr<ResponseStore> store, Mailbox* mailbox, std::function<void()> arrived);
  void clear();
  void resetGpu();
  size_t visibleCount() const { return visible_; }
  void invalidatePlacement() { reflow_ = true; }

 private:
  struct TextShape {
    double letterSpacing = 0, lineHeight = 1.2, maxWidth = 10;
    FontSpec font;
  };
  void placement(Transform& transform, const std::vector<SymbolLayerInput>& inputs, double now);
  std::shared_ptr<const Laid> layout(const StyleLayer& layer, const std::string& text, bool line, int& budget, const TextShape& shape);
  std::shared_ptr<const Laid> computeLayout(const std::string& text, bool line, int& budget, const TextShape& shape);
  FontSpec font(const StyleLayer& layer, const EvalContext& ctx);
  void build(Transform& transform, double pixelRatio);
  void syncTextures();

  std::unordered_map<std::string, Label> labels_;
  const LabelTerrain* terrain_ = nullptr;
  bool hadTerrain_ = false;
  std::unordered_map<std::string, std::shared_ptr<const Laid>> layoutCache_;
  uint64_t layoutGeneration_ = UINT64_MAX;
  std::unique_ptr<GlyphServer> server_;
  std::string placedStructure_;
  uint64_t placedCamera_ = UINT64_MAX;
  double lastPlacement_ = -1e18;
  double lastFrame_ = 0;
  bool pendingGlyphs_ = false;
  bool reflow_ = false;
  std::vector<float> textVertices_, iconVertices_;
  size_t glyphCount_ = 0, iconCount_ = 0, visible_ = 0;
  GpuTexture glyphTexture_, iconTexture_;
  uint64_t glyphGeneration_ = UINT64_MAX, iconGeneration_ = UINT64_MAX;
  GpuBuffer textBuffer_, iconBuffer_, indexBuffer_;
  size_t indexQuads_ = 0;
};

}
