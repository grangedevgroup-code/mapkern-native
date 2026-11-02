#include "worker/geojson.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "formats/mvt.hpp"
#include "geo/mercator.hpp"

namespace mapkern {

namespace {

constexpr double kAlwaysKeep = std::numeric_limits<double>::infinity();
constexpr size_t kDirectLimit = 16384;
constexpr size_t kMinCacheCost = size_t{1} << 20;
constexpr size_t kMaxCachedNodes = 4096;

using Points = std::vector<double>;

double signedArea(const Points& ring) {
  double sum = 0;
  const size_t n = ring.size();
  if (n < 3) return 0;
  for (size_t i = 0, j = n - 3; i < n; j = i, i += 3) sum += (ring[j] - ring[i]) * (ring[i + 1] + ring[j + 1]);
  return sum / 2;
}

Rect pointBounds(const Points& p) {
  Rect r{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
         -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
  for (size_t i = 0; i + 1 < p.size(); i += 3) {
    r.x0 = std::min(r.x0, p[i]);
    r.x1 = std::max(r.x1, p[i]);
    r.y0 = std::min(r.y0, p[i + 1]);
    r.y1 = std::max(r.y1, p[i + 1]);
  }
  return r;
}

Rect unionOf(const Rect& a, const Rect& b) {
  return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

double segmentDistanceSq(double px, double py, double ax, double ay, double bx, double by) {
  double x = ax, y = ay;
  const double dx = bx - ax, dy = by - ay;
  if (dx != 0 || dy != 0) {
    const double t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    if (t > 1) {
      x = bx;
      y = by;
    } else if (t > 0) {
      x += dx * t;
      y += dy * t;
    }
  }
  return (px - x) * (px - x) + (py - y) * (py - y);
}

void rankVertices(Points& points) {
  const size_t n = points.size() / 3;
  if (n == 0) return;
  for (size_t i = 0; i < n; i++) points[i * 3 + 2] = 0;
  points[2] = kAlwaysKeep;
  points[(n - 1) * 3 + 2] = kAlwaysKeep;
  struct Span {
    size_t first, last;
    double limit;
  };
  std::vector<Span> stack{{0, n - 1, kAlwaysKeep}};
  while (!stack.empty()) {
    const Span s = stack.back();
    stack.pop_back();
    if (s.last - s.first < 2) continue;
    const double ax = points[s.first * 3], ay = points[s.first * 3 + 1];
    const double bx = points[s.last * 3], by = points[s.last * 3 + 1];
    const double middle = (static_cast<double>(s.first) + static_cast<double>(s.last)) / 2;
    double best = 0;
    size_t index = 0;
    bool found = false;
    for (size_t i = s.first + 1; i < s.last; i++) {
      const double d = segmentDistanceSq(points[i * 3], points[i * 3 + 1], ax, ay, bx, by);
      const bool closer = found && std::abs(static_cast<double>(i) - middle) < std::abs(static_cast<double>(index) - middle);
      if (d > best || (d == best && closer)) {
        best = d;
        index = i;
        found = true;
      }
    }
    if (!found) continue;
    const double rank = std::min(best, s.limit);
    points[index * 3 + 2] = rank;
    stack.push_back({s.first, index, rank});
    stack.push_back({index, s.last, rank});
  }
}

Points reversedRing(const Points& ring) {
  Points out(ring.size());
  for (size_t i = 0, j = ring.size() - 3; i < ring.size(); i += 3, j -= 3) {
    out[i] = ring[j];
    out[i + 1] = ring[j + 1];
    out[i + 2] = ring[j + 2];
  }
  return out;
}

bool insideEdge(int edge, double x, double y, const Rect& r) {
  switch (edge) {
    case 0: return x >= r.x0;
    case 1: return x <= r.x1;
    case 2: return y >= r.y0;
    default: return y <= r.y1;
  }
}

void addCrossing(int edge, double ax, double ay, double bx, double by, const Rect& r, Points& out) {
  double t = 0;
  switch (edge) {
    case 0:
      t = (r.x0 - ax) / (bx - ax);
      out.push_back(r.x0);
      out.push_back(ay + (by - ay) * t);
      break;
    case 1:
      t = (r.x1 - ax) / (bx - ax);
      out.push_back(r.x1);
      out.push_back(ay + (by - ay) * t);
      break;
    case 2:
      t = (r.y0 - ay) / (by - ay);
      out.push_back(ax + (bx - ax) * t);
      out.push_back(r.y0);
      break;
    default:
      t = (r.y1 - ay) / (by - ay);
      out.push_back(ax + (bx - ax) * t);
      out.push_back(r.y1);
      break;
  }
  out.push_back(kAlwaysKeep);
}

std::optional<Points> clipRing(const Points& ring, const Rect& r) {
  Points pts = ring;
  if (pts.size() >= 6 && pts[0] == pts[pts.size() - 3] && pts[1] == pts[pts.size() - 2]) pts.resize(pts.size() - 3);
  for (int edge = 0; edge < 4; edge++) {
    if (pts.size() < 9) return std::nullopt;
    Points out;
    out.reserve(pts.size() + 12);
    const size_t n = pts.size();
    for (size_t i = 0; i < n; i += 3) {
      const size_t a = (i + n - 3) % n;
      const double ax = pts[a], ay = pts[a + 1], bx = pts[i], by = pts[i + 1];
      const bool ina = insideEdge(edge, ax, ay, r), inb = insideEdge(edge, bx, by, r);
      if (inb) {
        if (!ina) addCrossing(edge, ax, ay, bx, by, r, out);
        out.push_back(pts[i]);
        out.push_back(pts[i + 1]);
        out.push_back(pts[i + 2]);
      } else if (ina) {
        addCrossing(edge, ax, ay, bx, by, r, out);
      }
    }
    pts = std::move(out);
  }
  if (pts.size() < 9) return std::nullopt;
  pts.push_back(pts[0]);
  pts.push_back(pts[1]);
  pts.push_back(pts[2]);
  return pts;
}

struct ClippedSegment {
  double ax, ay, bx, by;
  bool entered, exited;
};

std::optional<ClippedSegment> clipSegment(double ax, double ay, double bx, double by, const Rect& r) {
  const double dx = bx - ax, dy = by - ay;
  double t0 = 0, t1 = 1;
  const double p[4] = {-dx, dx, -dy, dy};
  const double q[4] = {ax - r.x0, r.x1 - ax, ay - r.y0, r.y1 - ay};
  for (int i = 0; i < 4; i++) {
    if (p[i] == 0) {
      if (q[i] < 0) return std::nullopt;
    } else {
      const double t = q[i] / p[i];
      if (p[i] < 0) {
        if (t > t1) return std::nullopt;
        if (t > t0) t0 = t;
      } else {
        if (t < t0) return std::nullopt;
        if (t < t1) t1 = t;
      }
    }
  }
  return ClippedSegment{ax + dx * t0, ay + dy * t0, ax + dx * t1, ay + dy * t1, t0 > 0, t1 < 1};
}

std::vector<Points> clipLine(const Points& line, const Rect& r) {
  std::vector<Points> out;
  Points current;
  const auto flush = [&] {
    if (current.size() >= 6) out.push_back(current);
    current.clear();
  };
  for (size_t i = 0; i + 5 < line.size(); i += 3) {
    const size_t j = i + 3;
    const auto seg = clipSegment(line[i], line[i + 1], line[j], line[j + 1], r);
    if (!seg) {
      flush();
      continue;
    }
    const size_t tail = current.size();
    if (tail == 0 || seg->entered || current[tail - 3] != seg->ax || current[tail - 2] != seg->ay) {
      flush();
      current.push_back(seg->ax);
      current.push_back(seg->ay);
      current.push_back(seg->entered ? kAlwaysKeep : line[i + 2]);
    }
    current.push_back(seg->bx);
    current.push_back(seg->by);
    current.push_back(seg->exited ? kAlwaysKeep : line[j + 2]);
    if (seg->exited) flush();
  }
  flush();
  return out;
}

GeoJsonItemPtr makeItem(std::shared_ptr<const GeoJsonFeature> feature, int type, std::vector<GeoJsonPart> parts) {
  auto item = std::make_shared<GeoJsonItem>();
  item->feature = std::move(feature);
  item->type = type;
  bool first = true;
  for (const auto& part : parts) {
    const Rect b = pointBounds(part.points);
    item->bounds = first ? b : unionOf(item->bounds, b);
    first = false;
    item->vertices += part.points.size() / 3;
  }
  item->parts = std::move(parts);
  return item;
}

std::vector<GeoJsonPart> clipPoints(const GeoJsonItem& item, const Rect& r) {
  std::vector<GeoJsonPart> out;
  for (const auto& part : item.parts) {
    Points kept;
    const Points& p = part.points;
    for (size_t i = 0; i + 2 < p.size(); i += 3) {
      if (p[i] < r.x0 || p[i] > r.x1 || p[i + 1] < r.y0 || p[i + 1] > r.y1) continue;
      kept.push_back(p[i]);
      kept.push_back(p[i + 1]);
      kept.push_back(kAlwaysKeep);
    }
    if (!kept.empty()) out.push_back({std::move(kept), true, 0});
  }
  return out;
}

std::vector<GeoJsonPart> clipLines(const GeoJsonItem& item, const Rect& r) {
  std::vector<GeoJsonPart> out;
  for (const auto& part : item.parts)
    for (auto& points : clipLine(part.points, r)) out.push_back({std::move(points), true, part.size});
  return out;
}

std::vector<GeoJsonPart> clipPolygons(const GeoJsonItem& item, const Rect& r) {
  std::vector<GeoJsonPart> out;
  const double full = (r.x1 - r.x0) * (r.y1 - r.y0) * (1 - 1e-9);
  std::vector<GeoJsonPart> group;
  bool hollow = false;
  const auto flush = [&] {
    if (!hollow) out.insert(out.end(), std::make_move_iterator(group.begin()), std::make_move_iterator(group.end()));
    group.clear();
  };
  for (const auto& part : item.parts) {
    if (part.outer) {
      flush();
      hollow = false;
      if (auto ring = clipRing(part.points, r)) group.push_back({std::move(*ring), true, part.size});
      continue;
    }
    if (group.empty() || hollow) continue;
    auto ring = clipRing(part.points, r);
    if (!ring) continue;
    if (std::abs(signedArea(*ring)) >= full) hollow = true;
    else group.push_back({std::move(*ring), false, part.size});
  }
  flush();
  return out;
}

GeoJsonItemPtr clipItem(const GeoJsonItemPtr& item, const Rect& r) {
  const Rect& b = item->bounds;
  if (b.x0 > r.x1 || b.x1 < r.x0 || b.y0 > r.y1 || b.y1 < r.y0) return nullptr;
  if (b.x0 >= r.x0 && b.x1 <= r.x1 && b.y0 >= r.y0 && b.y1 <= r.y1) return item;
  std::vector<GeoJsonPart> parts = item->type == 1   ? clipPoints(*item, r)
                                   : item->type == 2 ? clipLines(*item, r)
                                                     : clipPolygons(*item, r);
  return parts.empty() ? nullptr : makeItem(item->feature, item->type, std::move(parts));
}

GeoJsonItemPtr shifted(const GeoJsonItem& item, double dx) {
  std::vector<GeoJsonPart> parts = item.parts;
  for (auto& part : parts)
    for (size_t i = 0; i < part.points.size(); i += 3) part.points[i] += dx;
  return makeItem(item.feature, item.type, std::move(parts));
}

std::optional<Points> projected(const Value& positions, size_t minimum) {
  if (!positions.isArray()) return std::nullopt;
  Points out;
  out.reserve(positions.size() * 3);
  for (const Value& p : positions.asArray()) {
    if (!p.isArray() || p.size() < 2) continue;
    const Value& lng = p[size_t{0}];
    const Value& lat = p[size_t{1}];
    if (!lng.isNumber() || !lat.isNumber() || !std::isfinite(lng.asNumber()) || !std::isfinite(lat.asNumber())) continue;
    out.push_back(mercatorX(lng.asNumber()));
    out.push_back(mercatorY(lat.asNumber()));
    out.push_back(0);
  }
  if (out.size() / 3 < minimum) return std::nullopt;
  return out;
}

double pathLength(const Points& p) {
  double length = 0;
  for (size_t i = 3; i < p.size(); i += 3) length += std::hypot(p[i] - p[i - 3], p[i + 1] - p[i - 2]);
  return length;
}

void closeRing(Points& p) {
  const size_t last = p.size() - 3;
  if (p[0] == p[last] && p[1] == p[last + 1]) return;
  const double x = p[0], y = p[1];
  p.push_back(x);
  p.push_back(y);
  p.push_back(0);
}

void readPolygon(const Value& polygon, std::vector<GeoJsonPart>& parts) {
  if (!polygon.isArray()) return;
  const Array& rings = polygon.asArray();
  for (size_t r = 0; r < rings.size(); r++) {
    const bool outer = r == 0;
    auto ring = projected(rings[r], 3);
    if (ring) closeRing(*ring);
    const double area = ring && ring->size() >= 12 ? signedArea(*ring) : 0;
    if (!ring || area == 0) {
      if (outer) return;
      continue;
    }
    Points oriented = (area > 0) == outer ? std::move(*ring) : reversedRing(*ring);
    rankVertices(oriented);
    parts.push_back({std::move(oriented), outer, std::abs(area)});
  }
}

GeoJsonItemPtr readGeometry(const Value& g, const std::shared_ptr<const GeoJsonFeature>& feature) {
  const std::string type = g["type"].stringOr("");
  const Value& c = g["coordinates"];
  std::vector<GeoJsonPart> parts;
  int kind = 0;
  if (type == "Point" || type == "MultiPoint") {
    kind = 1;
    auto points = type == "Point" ? projected(Value(Array{c}), 1) : projected(c, 1);
    if (points) {
      for (size_t i = 2; i < points->size(); i += 3) (*points)[i] = kAlwaysKeep;
      parts.push_back({std::move(*points), true, 0});
    }
  } else if (type == "LineString" || type == "MultiLineString") {
    kind = 2;
    const auto add = [&](const Value& line) {
      auto points = projected(line, 2);
      if (!points) return;
      rankVertices(*points);
      const double size = pathLength(*points);
      parts.push_back({std::move(*points), true, size});
    };
    if (type == "LineString") add(c);
    else if (c.isArray())
      for (const Value& line : c.asArray()) add(line);
  } else if (type == "Polygon" || type == "MultiPolygon") {
    kind = 3;
    if (type == "Polygon") readPolygon(c, parts);
    else if (c.isArray())
      for (const Value& polygon : c.asArray()) readPolygon(polygon, parts);
  } else {
    return nullptr;
  }
  return parts.empty() ? nullptr : makeItem(feature, kind, std::move(parts));
}

Object readProperties(const Value& raw) {
  Object out;
  if (!raw.isObject()) return out;
  for (const auto& [key, value] : raw.asObject()) {
    if (value.isString() || value.isBool()) out.emplace_back(key, value);
    else if (value.isNumber()) {
      if (std::isfinite(value.asNumber())) out.emplace_back(key, value);
    } else if (!value.isNull()) {
      out.emplace_back(key, value.toJson());
    }
  }
  return out;
}

struct Grid {
  double scale, ox, oy;
};

Ring quantize(const Points& points, double sq, const Grid& grid, bool closed) {
  const size_t n = points.size() / 3;
  Ring out;
  out.reserve(n * 2 + 2);
  const size_t end = closed ? n - 1 : n;
  for (size_t i = 0; i < end; i++) {
    if (i != 0 && i != n - 1 && !(points[i * 3 + 2] > sq)) continue;
    const auto x = static_cast<int32_t>(std::round(points[i * 3] * grid.scale - grid.ox));
    const auto y = static_cast<int32_t>(std::round(points[i * 3 + 1] * grid.scale - grid.oy));
    const size_t k = out.size();
    if (k >= 2 && x == out[k - 2] && y == out[k - 1]) continue;
    out.push_back(x);
    out.push_back(y);
  }
  if (closed) {
    while (out.size() >= 4 && out[out.size() - 2] == out[0] && out[out.size() - 1] == out[1]) out.resize(out.size() - 2);
    if (out.size() >= 2) {
      const int32_t x = out[0], y = out[1];
      out.push_back(x);
      out.push_back(y);
    }
  }
  return out;
}

Ring reversedInts(const Ring& ring) {
  Ring out(ring.size());
  for (size_t i = 0, j = ring.size() - 2; i < ring.size(); i += 2, j -= 2) {
    out[i] = ring[j];
    out[i + 1] = ring[j + 1];
  }
  return out;
}

std::vector<Ring> writePoints(const GeoJsonItem& item, const Grid& grid) {
  Ring out;
  for (const auto& part : item.parts) {
    const Points& p = part.points;
    for (size_t i = 0; i + 2 < p.size(); i += 3) {
      out.push_back(static_cast<int32_t>(std::round(p[i] * grid.scale - grid.ox)));
      out.push_back(static_cast<int32_t>(std::round(p[i + 1] * grid.scale - grid.oy)));
    }
  }
  if (out.empty()) return {};
  return {std::move(out)};
}

std::vector<Ring> writeLines(const GeoJsonItem& item, const Grid& grid, double tolerance) {
  std::vector<Ring> out;
  const double sq = tolerance * tolerance;
  for (const auto& part : item.parts) {
    if (tolerance > 0 && part.size < tolerance) continue;
    Ring line = quantize(part.points, sq, grid, false);
    if (line.size() >= 4) out.push_back(std::move(line));
  }
  return out;
}

std::vector<Ring> writePolygons(const GeoJsonItem& item, const Grid& grid, double tolerance) {
  std::vector<Ring> out;
  const double sq = tolerance * tolerance;
  bool open = false;
  for (const auto& part : item.parts) {
    const bool small = tolerance > 0 && part.size < sq;
    if (part.outer) open = !small;
    if (!open || small) continue;
    Ring ring = quantize(part.points, sq, grid, true);
    const double area = ring.size() >= 8 ? ringArea(ring) : 0;
    if (area == 0) {
      if (part.outer) open = false;
      continue;
    }
    if ((area > 0) != part.outer) ring = reversedInts(ring);
    out.push_back(std::move(ring));
  }
  return out;
}

class GeoJsonLayer : public FeatureSource {
 public:
  explicit GeoJsonLayer(uint32_t extent) : extent_(extent) {}
  void add(GeoJsonItemPtr item, std::vector<Ring> parts) { entries_.push_back({std::move(item), std::move(parts)}); }
  const std::string& name() const override {
    static const std::string layer = "geojson";
    return layer;
  }
  uint32_t extent() const override { return extent_; }
  size_t size() const override { return entries_.size(); }
  int type(size_t i) const override { return entries_[i].item->type; }
  std::optional<double> id(size_t i) const override { return entries_[i].item->feature->id; }
  std::string textId(size_t i) const override { return entries_[i].item->feature->textId; }
  uint32_t sourceIndex(size_t i) const override { return entries_[i].item->feature->index; }
  const Object& properties(size_t i) const override { return entries_[i].item->feature->properties; }
  std::vector<Ring> geometry(size_t i) const override { return entries_[i].parts; }

 private:
  struct Entry {
    GeoJsonItemPtr item;
    std::vector<Ring> parts;
  };
  uint32_t extent_;
  std::vector<Entry> entries_;
};

struct Reader {
  const GeoJsonOptions& options;
  std::vector<GeoJsonItemPtr> items;
  std::vector<GeoJsonItemPtr> originals;
  uint32_t features = 0;
  uint32_t order = 0;

  bool add(const Value& geometry, const std::shared_ptr<const GeoJsonFeature>& feature) {
    if (!geometry.isObject()) return false;
    if (geometry["type"].stringOr("") == "GeometryCollection") {
      bool added = false;
      if (geometry["geometries"].isArray())
        for (const Value& child : geometry["geometries"].asArray()) added = add(child, feature) || added;
      return added;
    }
    GeoJsonItemPtr item = readGeometry(geometry, feature);
    if (!item) return false;
    items.push_back(item);
    originals.push_back(item);
    if (item->bounds.x1 > 1) items.push_back(shifted(*item, -1));
    if (item->bounds.x0 < 0) items.push_back(shifted(*item, 1));
    return true;
  }

  void visit(const Value& node) {
    if (!node.isObject()) return;
    const std::string type = node["type"].stringOr("");
    if (type == "FeatureCollection") {
      if (node["features"].isArray())
        for (const Value& f : node["features"].asArray()) visit(f);
      return;
    }
    auto feature = std::make_shared<GeoJsonFeature>();
    feature->index = features;
    const uint32_t position = order++;
    if (type == "Feature") {
      feature->properties = readProperties(node["properties"]);
      Value raw = node["id"];
      if (options.generateId) {
        raw = Value(static_cast<double>(position));
      } else if (!options.promoteId.empty()) {
        for (const auto& [key, value] : feature->properties)
          if (key == options.promoteId) raw = value;
      }
      feature->id = featureKey(raw);
      if (raw.isString() && feature->id) feature->textId = raw.asString();
      if (add(node["geometry"], feature)) features++;
    } else {
      if (options.generateId) feature->id = static_cast<double>(position);
      if (add(node, feature)) features++;
    }
  }
};

Value positionOf(double x, double y) { return Value(Array{Value(lngFromMercatorX(x)), Value(latFromMercatorY(y))}); }

Value coordinatesOf(const GeoJsonPart& part) {
  Array out;
  out.reserve(part.points.size() / 3);
  for (size_t i = 0; i + 2 < part.points.size(); i += 3) out.push_back(positionOf(part.points[i], part.points[i + 1]));
  return Value(std::move(out));
}

Value itemGeometry(const GeoJsonItem& item) {
  if (item.type == 1) {
    Array points;
    for (const auto& part : item.parts) {
      const Value coordinates = coordinatesOf(part);
      for (const Value& p : coordinates.asArray()) points.push_back(p);
    }
    if (points.size() == 1) return Value(Object{{"type", Value("Point")}, {"coordinates", points[0]}});
    return Value(Object{{"type", Value("MultiPoint")}, {"coordinates", Value(std::move(points))}});
  }
  if (item.type == 2) {
    Array lines;
    for (const auto& part : item.parts) lines.push_back(coordinatesOf(part));
    if (lines.size() == 1) return Value(Object{{"type", Value("LineString")}, {"coordinates", lines[0]}});
    return Value(Object{{"type", Value("MultiLineString")}, {"coordinates", Value(std::move(lines))}});
  }
  std::vector<Array> polygons;
  for (const auto& part : item.parts) {
    if (part.outer) polygons.emplace_back();
    if (!polygons.empty()) polygons.back().push_back(coordinatesOf(part));
  }
  if (polygons.size() == 1) return Value(Object{{"type", Value("Polygon")}, {"coordinates", Value(std::move(polygons[0]))}});
  Array all;
  for (auto& polygon : polygons) all.push_back(Value(std::move(polygon)));
  return Value(Object{{"type", Value("MultiPolygon")}, {"coordinates", Value(std::move(all))}});
}

}

GeoJsonIndex::GeoJsonIndex(const Value& data, GeoJsonOptions options) : options_(std::move(options)) {
  Reader reader{options_};
  reader.visit(data);
  auto root = std::make_shared<Node>();
  for (const auto& item : reader.items) {
    root->vertices += item->vertices;
    coverage_ = coverage_ ? unionOf(*coverage_, item->bounds) : item->bounds;
  }
  root->cost = root->vertices;
  root->items = std::move(reader.items);
  budget_ = std::max(kMinCacheCost, root->vertices);
  root_ = std::move(root);
  originals_ = std::move(reader.originals);
  features_ = reader.features;
}

std::vector<GeoJsonItemPtr> GeoJsonIndex::itemsOf(uint32_t index) const {
  const auto low = std::lower_bound(originals_.begin(), originals_.end(), index,
                                    [](const GeoJsonItemPtr& item, uint32_t i) { return item->feature->index < i; });
  std::vector<GeoJsonItemPtr> out;
  for (auto it = low; it != originals_.end() && (*it)->feature->index == index; ++it) out.push_back(*it);
  return out;
}

GeoJsonIndex::NodePtr GeoJsonIndex::touch(uint64_t key) {
  const auto it = cache_.find(key);
  if (it == cache_.end()) return nullptr;
  order_.splice(order_.end(), order_, it->second.order);
  return it->second.node;
}

GeoJsonIndex::NodePtr GeoJsonIndex::derive(const Node& parent, int z, int x, int y) {
  const double n = std::exp2(z);
  const double pad = options_.buffer / 512 / n;
  const Rect rect{x / n - pad, y / n - pad, (x + 1) / n + pad, (y + 1) / n + pad};
  auto node = std::make_shared<Node>();
  for (const auto& item : parent.items) {
    GeoJsonItemPtr clipped = clipItem(item, rect);
    if (!clipped) continue;
    node->vertices += clipped->vertices;
    node->cost += clipped == item ? 1 : clipped->vertices;
    node->items.push_back(std::move(clipped));
  }
  const uint64_t key = tileKey(z, x, y);
  order_.push_back(key);
  cache_[key] = Cached{node, std::prev(order_.end())};
  cachedCost_ += node->cost;
  trim();
  return node;
}

void GeoJsonIndex::trim() {
  if (cachedCost_ <= budget_ && cache_.size() <= kMaxCachedNodes) return;
  while (!order_.empty() && (cachedCost_ > budget_ * 3 / 4 || cache_.size() > kMaxCachedNodes * 3 / 4)) {
    const auto it = cache_.find(order_.front());
    cachedCost_ -= it->second.node->cost;
    cache_.erase(it);
    order_.pop_front();
  }
}

GeoJsonIndex::NodePtr GeoJsonIndex::node(int z, int x, int y) {
  if (NodePtr hit = touch(tileKey(z, x, y))) return hit;
  NodePtr parent = root_;
  int level = -1;
  for (int k = z - 1; k >= 0; k--) {
    if (NodePtr ancestor = touch(tileKey(k, x >> (z - k), y >> (z - k)))) {
      parent = std::move(ancestor);
      level = k;
      break;
    }
  }
  for (;;) {
    const int next = parent->vertices > kDirectLimit ? level + 1 : z;
    NodePtr made = derive(*parent, next, x >> (z - next), y >> (z - next));
    if (next == z) return made;
    parent = std::move(made);
    level = next;
  }
}

std::shared_ptr<FeatureSource> GeoJsonIndex::tile(int z, int x, int y) {
  NodePtr found;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    found = node(z, x, y);
  }
  const double n = std::exp2(z);
  const double extent = options_.extent;
  const double tolerance = z >= options_.maxZoom ? 0 : options_.tolerance / (512 * n);
  const Grid grid{n * extent, x * extent, y * extent};
  auto layer = std::make_shared<GeoJsonLayer>(options_.extent);
  for (const auto& item : found->items) {
    std::vector<Ring> geometry = item->type == 1   ? writePoints(*item, grid)
                                 : item->type == 2 ? writeLines(*item, grid, tolerance)
                                                   : writePolygons(*item, grid, tolerance);
    if (!geometry.empty()) layer->add(item, std::move(geometry));
  }
  return layer;
}

Value geoJsonGeometry(const std::vector<GeoJsonItemPtr>& items) {
  if (items.empty()) return Value();
  if (items.size() == 1) return itemGeometry(*items[0]);
  Array geometries;
  for (const auto& item : items) geometries.push_back(itemGeometry(*item));
  return Value(Object{{"type", Value("GeometryCollection")}, {"geometries", Value(std::move(geometries))}});
}

}
