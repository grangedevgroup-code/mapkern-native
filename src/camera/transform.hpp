#pragma once

#include <array>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "geo/globe.hpp"
#include "geo/mat4.hpp"
#include "geo/mercator.hpp"
#include "geo/tile.hpp"

namespace mapkern {

inline constexpr double kMinZoom = -2;

struct CoverOptions {
  double tileSize = 512;
  int minZoom = 0;
  int maxZoom = 22;
  bool round = false;
  int maxTiles = 2000;
};

struct CoveringTile : TileSlot {
  double distance = 0;
};

struct Point {
  double x = 0;
  double y = 0;
};

struct ScreenPoint {
  double x = 0;
  double y = 0;
  double w = 0;
};

struct Padding {
  double top = 0, right = 0, bottom = 0, left = 0;
  bool operator==(const Padding&) const = default;
};

using World = std::array<double, 2>;

class Transform {
 public:
  double width = 1;
  double height = 1;
  double minZoom = 0;
  double maxZoom = 22;
  double minPitch = 0;
  double maxPitch = 60;
  bool renderWorldCopies = true;
  bool keepInWorld = true;

  mat4::Mat4 projMatrix = mat4::identity();
  mat4::Mat4 viewMatrix = mat4::identity();
  mat4::Mat4 pixelMatrix = mat4::identity();
  mat4::Mat4 pixelMatrixInverse = mat4::identity();
  mat4::Mat4 unitMatrix = mat4::identity();
  mat4::Mat4 globeMatrix = mat4::identity();
  mat4::Mat4 globeMatrixInverse = mat4::identity();

  LngLat center() const { return {lngFromMercatorX(cx_), latFromMercatorY(cy_)}; }
  World centerWorld() const { return {cx_, cy_}; }
  double zoom() const { return z_; }
  double bearing() const { return bearing_; }
  double pitch() const { return pitch_; }
  double fov() const { return fov_; }
  double scale() const { return std::exp2(z_); }
  double worldSize() const { return mapkern::worldSize(z_); }
  double cameraToCenterDistance();
  std::array<double, 3> cameraPosition();
  double elevation() const { return elevation_; }
  double ground() const { return ground_; }
  double pixelsPerMeter() const;
  double centerElevationPixels();
  double far();
  double near();
  Padding padding() const { return pad_; }
  Point centerPoint() const;
  std::optional<Rect> maxBounds() const { return limits_; }
  Projection projection() const { return mode_; }
  double globeAmount();
  double globeRadius();
  std::array<double, 3> globeCamera();

  void setProjection(Projection projection);
  void resize(double width, double height);
  void setPadding(const Padding& padding);
  void setMaxBounds(std::optional<Rect> bounds);
  void setCenter(double lng, double lat);
  void setCenterWorld(double x, double y);
  void setZoom(double zoom);
  void setBearing(double bearing);
  void setPitch(double pitch);
  void setElevation(double meters);
  void setGround(double meters);
  void setPeak(double meters);
  void setElevationKeepCamera(double meters);
  void setZoomRange(double minZoom, double maxZoom);
  void setFov(double degrees);
  void setPitchRange(double minPitch, double maxPitch);
  double lowestZoom();

  void update();
  void surfaceLocal(double x, double y, double h, double out[3]);
  std::array<double, 3> globeDirection(double lng, double lat);
  bool globeHidden(double lx, double ly, double lz) const;
  double groundFrame(double x, double y, double zPixels, double out[6]);
  std::array<float, 9> groundMatrix(double refX, double refY, double width, double height);
  std::array<float, 16> tileMatrix(int z, int x, int y, int wrap, double extent);
  mat4::Mat4 tileMatrixDouble(int z, int x, int y, int wrap, double extent);
  ScreenPoint project(double x, double y, double zPixels = 0);
  double projectTo(double out[2], double x, double y, double zPixels = 0);
  Point projectLngLat(double lng, double lat);
  void screenRay(double px, double py, std::array<double, 3>& origin, std::array<double, 3>& dir);
  std::optional<World> unproject(double px, double py, std::optional<double> groundZ = std::nullopt);
  std::optional<World> unprojectExact(double px, double py, std::optional<double> groundZ = std::nullopt);
  World unprojectClamped(double px, double py, std::optional<double> groundZ = std::nullopt);
  std::optional<LngLat> unprojectLngLat(double px, double py);
  double setLocationAtPoint(World world, double px, double py, std::optional<double> z = std::nullopt,
                            bool keepSize = false);
  double panGlobe(double dx, double dy);
  void zoomAround(double zoom, double px, double py, std::optional<double> z = std::nullopt);
  std::vector<World> footprint();
  double tileArea(int z, int x, int y, int wrap);
  std::vector<CoveringTile> coveringTiles(const CoverOptions& options);

 private:
  struct ViewExtent {
    Rect bounds;
    bool wraps;
    double left, right, top, bottom, fit;
  };
  ViewExtent view() const;
  void constrain();
  double projectGlobe(double x, double y, double zPixels, double out[2]);
  std::array<double, 3> globeRay(double px, double py);
  World localToMercator(double lx, double ly, double lz, bool keepPoles = false) const;
  std::optional<World> sphereHit(double px, double py, double lift, bool clamp, bool keepPoles = false);
  std::optional<World> refineGlobe(World guess, double px, double py, double h);
  std::optional<World> unprojectGlobe(double px, double py, std::optional<double> groundZ, bool clamp, bool keepPoles = false);
  std::optional<World> flatUnproject(double px, double py, std::optional<double> groundZ);
  World flatUnprojectClamped(double px, double py, std::optional<double> groundZ);
  double rotateGlobeTo(World world, double px, double py, std::optional<double> z, bool keepSize);
  std::vector<World> globeFootprint();
  double globeSpan() const;
  std::vector<CoveringTile> globeCovering(const CoverOptions& options);

  double fov_ = 0;
  Projection mode_ = Projection::Mercator;
  double globeAmount_ = 0;
  double radius_ = 1;
  std::array<double, 3> globeCam_{0, 0, 1};
  std::array<double, 3> forward_{0, 0, -1};
  std::array<double, 3> axisC_{1, 0, 0};
  std::array<double, 3> axisE_{0, 1, 0};
  std::array<double, 3> axisN_{0, 0, 1};
  std::unordered_map<std::string, std::vector<CoveringTile>> coverCache_;
  std::unordered_map<uint64_t, double> areaCache_;
  double cx_ = 0.5;
  double cy_ = 0.5;
  double z_ = 0;
  double bearing_ = 0;
  double pitch_ = 0;
  bool dirty_ = true;
  double cameraDistance_ = 1;
  std::array<double, 3> cameraPos_{0, 0, 1};
  double far_ = 1;
  double near_ = 1;
  double elevation_ = 0;
  double ground_ = 0;
  double peak_ = -std::numeric_limits<double>::infinity();
  double peakPx_ = 0;
  double elevationPx_ = 0;
  double groundPx_ = 0;
  Padding pad_;
  std::optional<Rect> limits_;

 public:
  Transform();
};

std::vector<World> convexHull(std::vector<World> points);
bool polygonIntersectsRect(const std::vector<World>& poly, double x0, double y0, double x1, double y1);

}
