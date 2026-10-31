#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mapkern {

struct LngLat;
class Value;

enum class GraphicsBackend { Default, D3D11, Metal, OpenGL, OpenGLES };

struct GraphicsEnvironment {
  const void* metalDevice = nullptr;
  const void* d3d11Device = nullptr;
  const void* d3d11DeviceContext = nullptr;
  int colorFormat = 0;
  int depthFormat = 0;
  int sampleCount = 1;
};

struct RenderSurface {
  int width = 0;
  int height = 0;
  int sampleCount = 1;
  int colorFormat = 0;
  int depthFormat = 0;
  const void* metalCurrentDrawable = nullptr;
  const void* metalDepthStencilTexture = nullptr;
  const void* metalMsaaColorTexture = nullptr;
  const void* d3d11RenderView = nullptr;
  const void* d3d11ResolveView = nullptr;
  const void* d3d11DepthStencilView = nullptr;
  uint32_t glFramebuffer = 0;
};

struct CameraOptions {
  std::optional<double> lng, lat, zoom, bearing, pitch;
  std::optional<double> aroundX, aroundY;
};

struct AnimationOptions {
  double duration = -1;
  double curve = 1.42;
  bool essential = false;
};

struct MapOptions {
  std::string style = "https://tiles.openfreemap.org/styles/liberty";
  double lng = 0;
  double lat = 0;
  double zoom = 0;
  double bearing = 0;
  double pitch = 0;
  double minZoom = -2;
  double maxZoom = 22;
  double maxPitch = 85;
  bool globe = true;
  double pixelRatio = 1;
  std::string cacheDirectory;
  uint64_t diskCacheBytes = uint64_t{1} << 30;
  size_t tileCacheBytes = 192u << 20;
  int requestTimeoutMs = 30000;
  double fadeDuration = 200;
  bool offline = false;
  bool controls = false;
  bool touchControls = true;
  std::vector<std::pair<std::string, std::string>> basemaps;
  std::string terrainSource;
  std::string view = "globe";
};

std::vector<std::pair<std::string, std::string>> defaultBasemaps();
std::string defaultTerrainSource();

struct MapEvent {
  std::string type;
  std::string message;
  std::string sourceId;
  std::vector<std::string> skipped;
};

struct MapStats {
  int drawCalls = 0;
  int pieces = 0;
  int tiles = 0;
  int loading = 0;
  double blank = 0;
  double frameMs = 0;
  uint64_t diskCacheBytes = 0;
  uint64_t diskCacheEntries = 0;
};

class MapImpl;

class Map {
 public:
  explicit Map(const MapOptions& options = {});
  ~Map();
  Map(const Map&) = delete;
  Map& operator=(const Map&) = delete;

  void initGraphics(const GraphicsEnvironment& environment);
  void shutdownGraphics();
  bool render(const RenderSurface& surface);
  bool needsRender() const;
  void setInvalidateCallback(std::function<void()> callback);

  void resize(double width, double height, double pixelRatio);
  void setStyle(const std::string& urlOrJson);
  std::string styleJson() const;
  bool isStyleLoaded() const;
  bool isIdle() const;

  void jumpTo(const CameraOptions& camera);
  void easeTo(const CameraOptions& camera, const AnimationOptions& animation = {});
  void flyTo(const CameraOptions& camera, const AnimationOptions& animation = {});
  void stop();
  bool isMoving() const;
  double zoom() const;
  double bearing() const;
  double pitch() const;
  double centerLng() const;
  double centerLat() const;
  void setProjection(bool globe);
  bool globe() const;
  void setTerrain(const std::string& sourceId, double exaggeration = 1, double relief = 0);
  void clearTerrain();
  bool terrain() const;
  std::optional<double> elevationAt(double lng, double lat) const;

  void panBy(double dx, double dy);
  void zoomBy(double delta, double x, double y, bool animate = false);
  void rotateBy(double degrees);
  void pitchBy(double degrees);

  void pointerDown(int id, double x, double y, double timeMs = -1);
  void pointerMove(int id, double x, double y, double timeMs = -1);
  void pointerUp(int id, double x, double y, double timeMs = -1);
  void pointerCancel(int id);
  void wheel(double delta, double x, double y);
  std::string diagnostics();
  void setHud(std::vector<std::string> lines);
  void doubleTap(double x, double y);

  std::optional<std::pair<double, double>> unproject(double x, double y) const;
  std::pair<double, double> project(double lng, double lat) const;

  void addSource(const std::string& id, const std::string& specJson);
  void removeSource(const std::string& id);
  void addLayer(const std::string& specJson, const std::string& beforeId = "");
  void removeLayer(const std::string& id);
  void setPaintProperty(const std::string& layerId, const std::string& name, const std::string& valueJson);
  void setLayoutProperty(const std::string& layerId, const std::string& name, const std::string& valueJson);
  void setFilter(const std::string& layerId, const std::string& filterJson);
  void setLayerVisible(const std::string& layerId, bool visible);
  std::vector<std::string> layerIds() const;
  std::string attribution() const;

  void setOffline(bool offline);
  bool offline() const;
  void clearDiskCache();
  void downloadRegion(double west, double south, double east, double north, int minZoom, int maxZoom,
                      std::function<void(size_t done, size_t total)> progress = {});

  void on(std::function<void(const MapEvent&)> listener);
  MapStats stats() const;
  MapImpl& impl() { return *impl_; }

 private:
  std::unique_ptr<MapImpl> impl_;
};

}
