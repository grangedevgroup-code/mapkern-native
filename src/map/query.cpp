#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

#include "formats/inflate.hpp"
#include "geo/mercator.hpp"
#include "map/geojson_source.hpp"
#include "map/map_impl.hpp"

namespace mapkern {

namespace {

constexpr double kSymbolRadius = 16;

GeometryType typeOf(int t) {
  switch (t) {
    case 1: return GeometryType::Point;
    case 2: return GeometryType::LineString;
    case 3: return GeometryType::Polygon;
    default: return GeometryType::Unknown;
  }
}

double segmentDistance(double px, double py, double ax, double ay, double bx, double by) {
  const double dx = bx - ax, dy = by - ay;
  double t = 0;
  if (dx != 0 || dy != 0) t = std::clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.0, 1.0);
  return std::hypot(px - (ax + dx * t), py - (ay + dy * t));
}

struct Parts {
  int type = 0;
  std::vector<std::vector<double>> rings;
};

struct Probe {
  Transform& transform;
  Terrain* terrain;
  double sx, sy;
  double gx, gy;
  double wrap;
  double margin;

  ScreenPoint screen(double x, double y) const {
    const double z = terrain ? terrain->heightAt(x, y) * transform.worldSize() : 0;
    return transform.project(x + wrap, y, z);
  }

  bool nearPoint(double x, double y, double radius) const {
    if (std::abs(x - gx) > margin || std::abs(y - gy) > margin) return false;
    const ScreenPoint p = screen(x, y);
    return p.w > 0 && std::hypot(p.x - sx, p.y - sy) <= radius;
  }

  bool nearPoints(const Parts& parts, double radius) const {
    for (const auto& ring : parts.rings)
      for (size_t i = 0; i + 1 < ring.size(); i += 2)
        if (nearPoint(ring[i], ring[i + 1], radius)) return true;
    return false;
  }

  bool nearLines(const Parts& parts, double radius) const {
    for (const auto& ring : parts.rings) {
      for (size_t i = 0; i + 3 < ring.size(); i += 2) {
        const double ax = ring[i], ay = ring[i + 1], bx = ring[i + 2], by = ring[i + 3];
        if (std::min(ax, bx) > gx + margin || std::max(ax, bx) < gx - margin || std::min(ay, by) > gy + margin ||
            std::max(ay, by) < gy - margin)
          continue;
        const ScreenPoint a = screen(ax, ay), b = screen(bx, by);
        if (a.w <= 0 || b.w <= 0) continue;
        if (segmentDistance(sx, sy, a.x, a.y, b.x, b.y) <= radius) return true;
      }
    }
    return false;
  }

  bool inside(const Parts& parts) const {
    bool in = false;
    for (const auto& ring : parts.rings) {
      const size_t n = ring.size();
      if (n < 6) continue;
      for (size_t i = 0, j = n - 2; i + 1 < n; j = i, i += 2) {
        const double xi = ring[i], yi = ring[i + 1], xj = ring[j], yj = ring[j + 1];
        if ((yi > gy) != (yj > gy) && gx < (xj - xi) * (gy - yi) / (yj - yi) + xi) in = !in;
      }
    }
    return in;
  }
};

Value positionsOf(const Ring& part, double tx, double ty, double n, double extent) {
  Array out;
  out.reserve(part.size() / 2);
  for (size_t i = 0; i + 1 < part.size(); i += 2) {
    const double x = (tx + part[i] / extent) / n, y = (ty + part[i + 1] / extent) / n;
    out.push_back(Value(Array{Value(lngFromMercatorX(x)), Value(latFromMercatorY(y))}));
  }
  return Value(std::move(out));
}

Value tileGeometry(int type, const std::vector<Ring>& parts, double tx, double ty, double n, double extent) {
  const auto named = [](const char* name, Value coordinates) {
    return Value(Object{{"type", Value(name)}, {"coordinates", std::move(coordinates)}});
  };
  if (type == 1) {
    Array points;
    for (const Ring& part : parts) {
      const Value positions = positionsOf(part, tx, ty, n, extent);
      for (const Value& p : positions.asArray()) points.push_back(p);
    }
    if (points.empty()) return Value();
    if (points.size() == 1) return named("Point", points[0]);
    return named("MultiPoint", Value(std::move(points)));
  }
  if (type == 2) {
    Array lines;
    for (const Ring& part : parts)
      if (part.size() >= 4) lines.push_back(positionsOf(part, tx, ty, n, extent));
    if (lines.empty()) return Value();
    if (lines.size() == 1) return named("LineString", lines[0]);
    return named("MultiLineString", Value(std::move(lines)));
  }
  if (type == 3) {
    Array polygons;
    for (const auto& polygon : classifyRings(parts)) {
      Array rings;
      for (const Ring& ring : polygon) rings.push_back(positionsOf(ring, tx, ty, n, extent));
      polygons.push_back(Value(std::move(rings)));
    }
    if (polygons.empty()) return Value();
    if (polygons.size() == 1) return named("Polygon", polygons[0]);
    return named("MultiPolygon", Value(std::move(polygons)));
  }
  return Value();
}

}

Value MapImpl::queryFeatures(double x, double y, const std::vector<std::string>& wanted, double radius) {
  Array out;
  if (!styleLoaded) return Value(std::move(out));
  const auto ground = pickWorld(x, y);
  if (!ground) return Value(std::move(out));
  const double zoom = transform.zoom();
  const double margin = 3 * (radius + 64) / transform.worldSize();
  std::unordered_map<const void*, VectorTile> decoded;
  for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
    const StyleLayer& layer = **it;
    if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), layer.id) == wanted.end()) continue;
    if (!layer.isVisible(zoom)) continue;
    const bool fill = layer.type == LayerType::Fill || layer.type == LayerType::Extrusion;
    const bool line = layer.type == LayerType::Line;
    const bool circle = layer.type == LayerType::Circle;
    const bool symbol = layer.type == LayerType::Symbol;
    if (!fill && !line && !circle && !symbol) continue;
    const auto found = sources.find(layer.source);
    if (found == sources.end()) continue;
    auto* vector = dynamic_cast<VectorSource*>(found->second.get());
    if (!vector || !vector->manager) continue;
    auto* geojson = dynamic_cast<GeoJsonSource*>(vector);
    std::set<std::string> seen;
    for (const auto& piece : vector->manager->pieces) {
      const auto& tile = *piece.tile;
      if (!tile.data) continue;
      const auto entry = tile.data->buckets.find(layer.id);
      if (entry == tile.data->buckets.end()) continue;
      const VectorBucket& bucket = *entry->second;
      const double n = std::exp2(tile.z);
      const Probe probe{transform, terrain.get(), x, y, (*ground)[0] - piece.wrap, (*ground)[1], static_cast<double>(piece.wrap), margin};
      if (probe.gx < tile.x / n - margin || probe.gx > (tile.x + 1) / n + margin || probe.gy < tile.y / n - margin ||
          probe.gy > (tile.y + 1) / n + margin)
        continue;
      std::shared_ptr<VectorLayer> source;
      if (!geojson) {
        const SharedBytes& raw = tile.data->raw;
        if (!raw) continue;
        auto cached = decoded.find(raw.get());
        if (cached == decoded.end()) {
          try {
            cached = decoded.emplace(raw.get(), decodeVectorTile(std::make_shared<const Bytes>(decompressAuto(*raw)))).first;
          } catch (const std::exception&) {
            continue;
          }
        }
        if (layer.sourceLayer.empty()) {
          std::string first;
          for (const auto& [name, l] : cached->second)
            if (first.empty() || name < first) first = name;
          if (!first.empty()) source = cached->second[first];
        } else if (const auto l = cached->second.find(layer.sourceLayer); l != cached->second.end()) {
          source = l->second;
        }
        if (!source) continue;
      }
      const auto& records = bucket.features.features;
      for (size_t r = records.size(); r-- > 0;) {
        const FeatureRecord& record = records[r];
        std::vector<Parts> shapes;
        std::vector<GeoJsonItemPtr> items;
        std::vector<Ring> tileParts;
        Object properties;
        if (geojson) {
          if (!geojson->index()) continue;
          items = geojson->index()->itemsOf(record.source);
          if (!items.empty()) properties = items[0]->feature->properties;
          for (const auto& item : items) {
            Parts parts;
            parts.type = item->type;
            for (const auto& part : item->parts) {
              std::vector<double> ring;
              ring.reserve(part.points.size() / 3 * 2);
              for (size_t i = 0; i + 2 < part.points.size(); i += 3) {
                ring.push_back(part.points[i]);
                ring.push_back(part.points[i + 1]);
              }
              parts.rings.push_back(std::move(ring));
            }
            shapes.push_back(std::move(parts));
          }
        } else {
          if (record.source >= source->size()) continue;
          const VectorFeature feature = source->feature(record.source);
          tileParts = feature.geometry();
          properties = feature.properties();
          Parts parts;
          parts.type = feature.type;
          for (const Ring& part : tileParts) {
            std::vector<double> ring(part.size());
            for (size_t i = 0; i + 1 < part.size(); i += 2) {
              ring[i] = (tile.x + static_cast<double>(part[i]) / feature.extent) / n;
              ring[i + 1] = (tile.y + static_cast<double>(part[i + 1]) / feature.extent) / n;
            }
            parts.rings.push_back(std::move(ring));
          }
          shapes.push_back(std::move(parts));
        }
        FeatureContext context;
        context.properties = &record.properties;
        context.id = !record.textId.empty() ? Value(record.textId) : record.id ? Value(*record.id) : Value();
        context.geometryType = typeOf(record.type);
        EvalContext eval;
        eval.zoom = zoom;
        eval.feature = &context;
        bool hit = false;
        for (const Parts& parts : shapes) {
          if (fill) hit = parts.type == 3 && probe.inside(parts);
          else if (line) hit = probe.nearLines(parts, radius + layer.paintNumber("width", eval) / 2);
          else if (circle) hit = probe.nearPoints(parts, radius + layer.paintNumber("radius", eval) + layer.paintNumber("strokeWidth", eval));
          else if (parts.type == 1) hit = probe.nearPoints(parts, std::max(radius, kSymbolRadius));
          else hit = probe.nearLines(parts, std::max(radius, kSymbolRadius));
          if (hit) break;
        }
        if (!hit) continue;
        const std::string key = geojson ? std::to_string(record.source)
                                : !record.textId.empty() ? "t" + record.textId
                                : record.id ? "i" + std::to_string(*record.id)
                                            : std::to_string(tile.key) + "/" + std::to_string(record.source);
        if (!seen.insert(key).second) continue;
        Object feature{{"type", Value("Feature")}};
        if (!context.id.isNull()) feature.emplace_back("id", context.id);
        feature.emplace_back("properties", Value(std::move(properties)));
        feature.emplace_back("geometry", geojson ? geoJsonGeometry(items)
                                                 : tileGeometry(record.type, tileParts, tile.x, tile.y, n, source->extent));
        feature.emplace_back("layer", Value(layer.id));
        feature.emplace_back("source", Value(layer.source));
        if (!layer.sourceLayer.empty()) feature.emplace_back("sourceLayer", Value(layer.sourceLayer));
        out.push_back(Value(std::move(feature)));
      }
    }
  }
  return Value(std::move(out));
}

}
