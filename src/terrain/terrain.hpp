#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "camera/transform.hpp"
#include "map/dem_source.hpp"

namespace mapkern {

struct TerrainSlot {
  CoveringTile cover;
  std::shared_ptr<Tile<DemTileData>> dem;
  std::array<float, 4> rect{0, 0, 1, 1};
};

std::optional<double> highestClearZoom(double requested, double lowest, const std::function<bool(double)>& clear);
double metersToWorld(double meters, double worldY);

class Terrain {
 public:
  Terrain(DemSource* source, double exaggeration) : source(source), exaggeration(exaggeration) {}
  DemSource* source;
  double exaggeration;
  double relief = 0;
  std::vector<TerrainSlot> slots;
  double ground() const { return lowest_; }
  void update(Transform& transform);
  std::optional<double> elevationAt(double x, double y) const;
  double surfaceAt(double x, double y) const { return elevationAt(x, y).value_or(0); }
  double heightAt(double x, double y) const { return metersToWorld(surfaceAt(x, y), y); }
  bool hidden(const std::array<double, 3>& camera, double x, double y, double z) const;
  std::optional<std::array<double, 3>> raycast(const std::array<double, 3>& origin, const std::array<double, 3>& dir, double maxDistance) const;
  std::pair<std::shared_ptr<Tile<DemTileData>>, std::array<float, 4>> demFor(int z, int x, int y) const;

 private:
  double lowest_ = 0;
};

}
