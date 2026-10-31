#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "camera/animation.hpp"
#include "camera/transform.hpp"
#include "core/async.hpp"
#include "core/value.hpp"
#include "map/gestures.hpp"
#include "map/source.hpp"
#include "mapkern/map.hpp"
#include "render/painter.hpp"
#include "render/paint_table.hpp"
#include "style/layer.hpp"
#include "terrain/terrain.hpp"
#include "ui/overlay.hpp"
#include "tiles/cache.hpp"
#include "tiles/scheduler.hpp"

namespace mapkern {

class MapImpl {
 public:
  explicit MapImpl(const MapOptions& options);
  ~MapImpl();

  void initGraphics(const GraphicsEnvironment& environment);
  void shutdownGraphics();
  bool render(const RenderSurface& surface);
  bool renderSwapchain(const sg_swapchain& swapchain);
  bool renderTarget(const FrameTarget& target);
  void initGraphicsNative(const sg_environment& environment);
  bool needsRender() const;
  void tick(double now);
  void invalidate();

  void resize(double width, double height, double pixelRatio);
  void setStyle(const std::string& urlOrJson);
  void applyStyle(const Value& style, const std::vector<std::string>& skipped);
  Value styleJson() const;

  void jumpTo(const CameraOptions& camera);
  void easeTo(const CameraOptions& camera, const AnimationOptions& animation);
  void flyTo(const CameraOptions& camera, const AnimationOptions& animation);
  void stop();
  CameraState targetState(const CameraOptions& camera);

  void pointerDown(int id, double x, double y, double time);
  void pointerMove(int id, double x, double y, double time);
  void pointerUp(int id, double x, double y, double time, bool cancelled);
  void wheel(double delta, double x, double y);
  std::optional<Point3> pickWorld(double px, double py);
  void easeZoom(double delta, double x, double y);

  void addSource(const std::string& id, const Value& spec);
  void loadSprite(const Value& sprite);
  void removeSource(const std::string& id);
  void addLayer(const Value& spec, const std::string& beforeId);
  void removeLayer(const std::string& id);
  StyleLayer* layer(const std::string& id);
  void layersChanged();
  void emit(const MapEvent& e);
  void error(const std::string& code, const std::string& message, const std::string& sourceId = "");
  std::string attribution() const;
  MapStats stats() const;
  void updateTerrain();
  void setupControls();
  void ensureDemoTerrain();
  void setView(const std::string& view);
  std::string currentView();
  void updateOverlay();
  std::optional<double> cameraFloor(Terrain& terrain);
  double nearPeak(Terrain& terrain);
  void setTerrain(const Value& spec);
  void downloadRegion(double west, double south, double east, double north, int minZoom, int maxZoom,
                      std::function<void(size_t, size_t)> progress);

  MapOptions options;
  Transform transform;
  Mailbox mailbox;
  RequestScheduler scheduler{24};
  std::shared_ptr<DiskCache> diskCache;
  SourceContext sourceCtx;
  std::vector<std::unique_ptr<StyleLayer>> layers;
  std::map<std::string, std::unique_ptr<Source>> sources;
  Value styleRoot;
  bool styleLoaded = false;
  FeatureStates states;
  std::unique_ptr<Terrain> terrain;
  std::unique_ptr<ui::Overlay> overlay;
  std::set<int> overlayPointers;
  Painter painter;
  bool graphics = false;
  bool dirty = true;
  double pixelRatio = 1;
  std::vector<std::function<void(const MapEvent&)>> listeners;
  std::function<void()> invalidateCallback;
  std::optional<CameraMotion> motion;
  double motionStart = 0;
  double wakeAt = 0;
  double lastFrameMs = 0;
  CancelToken styleRequest;

  Gestures gestures{transform, GestureHost{[this](double x, double y) { return pickWorld(x, y); }, [this] { motion.reset(); },
                                           [this] { invalidate(); }, [this](double d, double x, double y) { easeZoom(d, x, y); }}};

  struct FrameWindow {
    uint64_t frames = 0, slow = 0, intervals = 0, draws = 0, drawsMax = 0;
    double cpuSum = 0, cpuMax = 0, tickSum = 0, tickMax = 0, intervalSum = 0, intervalMax = 0;
    std::array<uint64_t, 6> buckets{};
  };
  FrameWindow frameWindow;
  double lastFrameStart = 0;
  double lastSnapshot = 0;
  diag::Totals lastTotals;
  std::deque<std::pair<double, std::string>> recentErrors;
  uint64_t errorCount = 0;
  uint64_t errorsReported = 0;
  std::vector<std::string> hudLines;
  bool fitWorld = false;
  std::string diagnosticsJson();
};

}
