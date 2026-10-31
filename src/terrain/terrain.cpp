#include "terrain/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "geo/mercator.hpp"

namespace mapkern {

namespace {
constexpr int kMaxSlotZoom = 22;
}

std::optional<double> highestClearZoom(double requested, double lowest, const std::function<bool(double)>& clear) {
  const double step = 1.0 / 32;
  double above = requested;
  while (above > lowest) {
    const double z = std::max(lowest, above - step);
    if (clear(z)) {
      double low = z, high = above;
      for (int i = 0; i < 6; i++) {
        const double mid = (low + high) / 2;
        if (clear(mid)) low = mid;
        else high = mid;
      }
      return low;
    }
    above = z;
  }
  return std::nullopt;
}

double metersToWorld(double meters, double worldY) { return meters * std::cosh(kPi * (1 - 2 * worldY)) / kEarthCircumference; }

void Terrain::update(Transform& transform) {
  CoverOptions cover;
  cover.tileSize = 512;
  cover.minZoom = 0;
  cover.maxZoom = std::min(kMaxSlotZoom, static_cast<int>(std::ceil(transform.maxZoom)));
  cover.round = true;
  auto tiles = transform.coveringTiles(cover);
  std::sort(tiles.begin(), tiles.end(), [](const CoveringTile& a, const CoveringTile& b) { return a.distance < b.distance; });
  std::vector<TerrainSlot> out;
  out.reserve(tiles.size());
  double lowest = std::numeric_limits<double>::infinity();
  for (const auto& c : tiles) {
    auto [tile, rect] = demFor(c.z, c.x, c.y);
    if (tile && tile->data) lowest = std::min(lowest, tile->data->grid.min * exaggeration);
    out.push_back({c, std::move(tile), rect});
  }
  slots = std::move(out);
  lowest_ = std::isfinite(lowest) ? lowest : 0;
}

std::optional<double> Terrain::elevationAt(double x, double y) const {
  auto m = source ? source->elevationAt(x, y) : std::nullopt;
  if (!m) return std::nullopt;
  return *m * exaggeration;
}

bool Terrain::hidden(const std::array<double, 3>& camera, double x, double y, double z) const {
  const double dx = camera[0] - x, dy = camera[1] - y, dz = camera[2] - z;
  const int steps = 32;
  for (int i = 1; i < steps; i++) {
    const double t = std::pow(static_cast<double>(i) / steps, 2);
    if (t < 0.002) continue;
    if (heightAt(x + dx * t, y + dy * t) > z + dz * t) return true;
  }
  return false;
}

std::optional<std::array<double, 3>> Terrain::raycast(const std::array<double, 3>& origin, const std::array<double, 3>& dir, double maxDistance) const {
  const double length = std::hypot(dir[0], dir[1], dir[2]);
  if (!(length > 0)) return std::nullopt;
  const double dx = dir[0] / length, dy = dir[1] / length, dz = dir[2] / length;
  auto height = [&](double t) {
    const double x = origin[0] + dx * t, y = origin[1] + dy * t;
    return origin[2] + dz * t - metersToWorld(surfaceAt(x, y), y);
  };
  double h = height(0);
  if (h <= 0) return std::nullopt;
  const double step = std::max(maxDistance / 4096, 1e-12);
  const double climb = std::abs(dz) + 3 * std::max(1.0, exaggeration) * std::hypot(dx, dy);
  double t = 0;
  while (t < maxDistance) {
    const double next = std::min(maxDistance, t + std::max(step, h / climb));
    const double hn = height(next);
    if (hn <= 0) {
      double lo = t, hi = next;
      for (int i = 0; i < 40; i++) {
        const double mid = (lo + hi) / 2;
        if (height(mid) > 0) lo = mid;
        else hi = mid;
      }
      return std::array<double, 3>{origin[0] + dx * hi, origin[1] + dy * hi, origin[2] + dz * hi};
    }
    t = next;
    h = hn;
  }
  return std::nullopt;
}

std::pair<std::shared_ptr<Tile<DemTileData>>, std::array<float, 4>> Terrain::demFor(int z, int x, int y) const {
  if (!source || !source->manager) return {nullptr, {0, 0, 1, 1}};
  const auto& tiling = source->manager->tiling;
  const int top = std::min(z, tiling.maxZoom);
  for (int dz = top; dz >= tiling.minZoom; dz--) {
    const int shift = z - dz;
    const int tx = x >> shift, ty = y >> shift;
    auto tile = source->manager->getTile(dz, tx, ty);
    if (!tile || !tile->isReady() || !tile->data) continue;
    if (shift == 0) return {tile, {0, 0, 1, 1}};
    const int n = 1 << shift;
    const float x0 = static_cast<float>(x - tx * n) / n, y0 = static_cast<float>(y - ty * n) / n;
    return {tile, {x0, y0, 1.0f / n, 1.0f / n}};
  }
  return {nullptr, {0, 0, 1, 1}};
}

}
