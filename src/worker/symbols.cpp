#include "worker/symbols.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "core/utf8.hpp"

namespace mapkern {

namespace {

double numberOr(const Value& v, double fallback) { return v.isNumber() && std::isfinite(v.asNumber()) ? v.asNumber() : fallback; }

std::string normalizeText(const std::string& raw) {
  std::string unified;
  for (size_t i = 0; i < raw.size(); i++) {
    if (raw[i] == '\r') {
      unified += '\n';
      if (i + 1 < raw.size() && raw[i + 1] == '\n') i++;
    } else {
      unified += raw[i];
    }
  }
  std::string out;
  size_t start = 0;
  while (start <= unified.size()) {
    size_t end = unified.find('\n', start);
    if (end == std::string::npos) end = unified.size();
    std::string line;
    bool space = false;
    for (size_t i = start; i < end; i++) {
      const char c = unified[i];
      if (c == ' ' || c == '\t' || c == '\f' || c == '\v') {
        space = !line.empty();
      } else {
        if (space) line += ' ';
        space = false;
        line += c;
      }
    }
    if (!line.empty()) {
      if (!out.empty()) out += '\n';
      out += line;
    }
    start = end + 1;
  }
  return out;
}

std::array<double, 2> interiorPoint(const Ring& ring) {
  double cx = 0, cy = 0, a = 0;
  const size_t n = ring.size();
  for (size_t i = 0, j = n - 2; i < n; j = i, i += 2) {
    const double f = static_cast<double>(ring[j]) * ring[i + 1] - static_cast<double>(ring[i]) * ring[j + 1];
    cx += (ring[j] + ring[i]) * f;
    cy += (ring[j + 1] + ring[i + 1]) * f;
    a += f;
  }
  auto inside = [&](double x, double y) {
    bool c = false;
    for (size_t i = 0, j = n - 2; i < n; j = i, i += 2) {
      const double xi = ring[i], yi = ring[i + 1], xj = ring[j], yj = ring[j + 1];
      if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) c = !c;
    }
    return c;
  };
  if (a != 0) {
    cx /= 3 * a;
    cy /= 3 * a;
    if (inside(cx, cy)) return {cx, cy};
  }
  double minY = INFINITY, maxY = -INFINITY;
  for (size_t i = 1; i < n; i += 2) {
    minY = std::min(minY, static_cast<double>(ring[i]));
    maxY = std::max(maxY, static_cast<double>(ring[i]));
  }
  const double y = (minY + maxY) / 2;
  std::vector<double> xs;
  for (size_t i = 0, j = n - 2; i < n; j = i, i += 2) {
    const double yi = ring[i + 1], yj = ring[j + 1];
    if ((yi > y) != (yj > y)) xs.push_back(ring[i] + (y - yi) * (ring[j] - ring[i]) / (yj - yi));
  }
  std::sort(xs.begin(), xs.end());
  double best = 0, bx = ring[0];
  for (size_t k = 0; k + 1 < xs.size(); k += 2) {
    const double w = xs[k + 1] - xs[k];
    if (w > best) {
      best = w;
      bx = (xs[k] + xs[k + 1]) / 2;
    }
  }
  return {bx, y};
}

void lineLabels(const std::vector<Ring>& parts, const LabelInfo& info, uint32_t index, const SymbolLayout& layout, uint32_t extent,
                PackedLabels& out) {
  const double spacing = std::max(16.0, layout.spacing) * (extent / 512.0);
  const double minLength = std::min(utf8::decode(info.text).size() * info.size * 0.5, 400.0) * (extent / 512.0);
  const bool centered = layout.placement == "line-center";
  for (const auto& part : parts) {
    const double length = lineLength(part);
    if (length < minLength) continue;
    std::vector<float> anchors;
    const int count = centered ? 1 : std::max(1, static_cast<int>(std::floor(length / spacing)));
    const double step = length / count;
    for (int i = 0; i < count; i++) {
      const double at = step * (i + 0.5);
      const auto p = pointAlong(part, at);
      anchors.insert(anchors.end(), {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(at)});
    }
    out.add(info.text, info.icon, index, info.size, info.priority, true, anchors, &part);
  }
}

void labelCandidates(const FeatureSource& source, size_t f, uint32_t index, const EvalContext& ctx, const SymbolLayout& layout,
                     uint32_t extent, PackedLabels& out, bool& added) {
  const auto info = labelInfo(ctx, layout);
  if (!info) return;
  const auto geometry = source.geometry(f);
  if (geometry.empty()) return;
  const int type = source.type(f);
  if (layout.placement != "point" && (type == 2 || type == 3)) {
    const size_t before = out.count();
    lineLabels(geometry, *info, index, layout, extent, out);
    added = out.count() > before;
    return;
  }
  if (type == 1) {
    std::vector<float> anchors;
    for (const auto& part : geometry)
      for (size_t i = 0; i + 1 < part.size(); i += 2) anchors.insert(anchors.end(), {static_cast<float>(part[i]), static_cast<float>(part[i + 1]), 0.0f});
    out.add(info->text, info->icon, index, info->size, info->priority, false, anchors, nullptr);
    added = true;
    return;
  }
  if (type == 2) {
    const Ring* best = nullptr;
    double bestLength = 0;
    for (const auto& part : geometry) {
      const double length = lineLength(part);
      if (length > bestLength) {
        bestLength = length;
        best = &part;
      }
    }
    if (best) {
      const auto p = pointAlong(*best, bestLength / 2);
      out.add(info->text, info->icon, index, info->size, info->priority, false, {static_cast<float>(p[0]), static_cast<float>(p[1]), 0.0f},
              nullptr);
      added = true;
    }
    return;
  }
  const auto polygons = classifyRings(geometry);
  const Ring* bestRing = nullptr;
  double bestArea = 0;
  for (const auto& rings : polygons) {
    const double area = std::abs(ringArea(rings[0]));
    if (area > bestArea) {
      bestArea = area;
      bestRing = &rings[0];
    }
  }
  if (bestRing) {
    const auto p = interiorPoint(*bestRing);
    if (p[0] >= 0 && p[1] >= 0 && p[0] < extent && p[1] < extent) {
      out.add(info->text, info->icon, index, info->size, info->priority, false, {static_cast<float>(p[0]), static_cast<float>(p[1]), 0.0f},
              nullptr);
      added = true;
    }
  }
}

GeometryType geometryTypeOf(int t) {
  switch (t) {
    case 1: return GeometryType::Point;
    case 2: return GeometryType::LineString;
    case 3: return GeometryType::Polygon;
    default: return GeometryType::Unknown;
  }
}

std::mutex layoutMutex;
std::unordered_map<std::string, std::shared_ptr<SymbolLayout>> layouts;

std::shared_ptr<SymbolLayout> layoutFor(const LayerProgram& program, int z) {
  const std::string key = program.id + "|" + program.signature;
  std::shared_ptr<SymbolLayout> base;
  {
    std::lock_guard lock(layoutMutex);
    if (auto it = layouts.find(key); it != layouts.end()) base = it->second;
  }
  if (!base) {
    const Value& l = program.symbolLayout;
    auto make = [&](const char* name, OutputKind kind, Value fallback) {
      const Value v = l.has(name) ? l[name] : fallback;
      return compile(v, kind, fallback, program.id + "." + name);
    };
    base = std::make_shared<SymbolLayout>();
    base->text = make("text", OutputKind::String, "");
    base->icon = make("icon", OutputKind::String, "");
    base->size = make("size", OutputKind::Number, 14);
    base->priority = make("priority", OutputKind::Number, 0);
    base->transform = make("transform", OutputKind::String, "none");
    std::lock_guard lock(layoutMutex);
    if (layouts.size() > 2048) layouts.clear();
    layouts[key] = base;
  }
  auto layout = std::make_shared<SymbolLayout>(*base);
  const Value& l = program.symbolLayout;
  EvalContext ctx;
  ctx.zoom = z;
  const std::string placement = compile(l.has("placement") ? l["placement"] : Value("point"), OutputKind::String, "point").string(ctx);
  layout->placement = placement == "line" || placement == "line-center" ? placement : "point";
  layout->spacing = compile(l.has("spacing") ? l["spacing"] : Value(250), OutputKind::Number, 250).number(ctx);
  return layout;
}

}

void PackedLabels::add(const std::string& text, const std::string& icon, uint32_t featureIndex, double sz, double prio, bool isLine,
                       const std::vector<float>& anchorList, const Ring* path) {
  texts.push_back(text);
  icons.push_back(icon);
  feature.push_back(featureIndex);
  size.push_back(static_cast<float>(sz));
  priority.push_back(static_cast<float>(prio));
  line.push_back(isLine ? 1 : 0);
  anchors.insert(anchors.end(), anchorList.begin(), anchorList.end());
  anchorStart.push_back(static_cast<uint32_t>(anchors.size()));
  if (path) paths.insert(paths.end(), path->begin(), path->end());
  pathStart.push_back(static_cast<uint32_t>(paths.size()));
}

std::optional<LabelInfo> labelInfo(const EvalContext& ctx, const SymbolLayout& layout) {
  const Value raw = layout.text.evaluate(ctx);
  std::string text = raw.isString() ? raw.asString() : raw.isNumber() ? formatNumber(raw.asNumber()) : "";
  text = normalizeText(text);
  const Value rawIcon = layout.icon.evaluate(ctx);
  const std::string icon = rawIcon.isString() ? rawIcon.asString() : "";
  if (text.empty() && icon.empty()) return std::nullopt;
  auto cps = utf8::decode(text);
  if (cps.size() > 200) {
    cps.resize(199);
    cps.push_back(0x2026);
    text = utf8::encode(cps);
  }
  const std::string transform = layout.transform.string(ctx);
  if (transform == "uppercase") text = utf8::upper(text);
  else if (transform == "lowercase") text = utf8::lower(text);
  return LabelInfo{text, icon, numberOr(layout.size.evaluate(ctx), 14), numberOr(layout.priority.evaluate(ctx), 0)};
}

double lineLength(const Ring& line) {
  double length = 0;
  for (size_t i = 2; i + 1 < line.size(); i += 2) length += std::hypot(line[i] - line[i - 2], line[i + 1] - line[i - 1]);
  return length;
}

std::array<double, 2> pointAlong(const Ring& line, double distance) {
  double remaining = distance;
  for (size_t i = 2; i + 1 < line.size(); i += 2) {
    const double ax = line[i - 2], ay = line[i - 1], bx = line[i], by = line[i + 1];
    const double seg = std::hypot(bx - ax, by - ay);
    if (seg >= remaining && seg > 0) {
      const double t = remaining / seg;
      return {ax + (bx - ax) * t, ay + (by - ay) * t};
    }
    remaining -= seg;
  }
  return {static_cast<double>(line[line.size() - 2]), static_cast<double>(line[line.size() - 1])};
}

std::vector<Ring> mergeParts(const std::vector<Ring>& parts) {
  if (parts.size() < 2) return parts;
  auto key = [](int64_t x, int64_t y) { return x * 67108864 + y; };
  std::unordered_map<int64_t, std::vector<size_t>> ends;
  for (size_t i = 0; i < parts.size(); i++) {
    const Ring& p = parts[i];
    if (p.size() < 4) continue;
    ends[key(p[0], p[1])].push_back(i * 2);
    ends[key(p[p.size() - 2], p[p.size() - 1])].push_back(i * 2 + 1);
  }
  std::vector<uint8_t> used(parts.size(), 0);
  auto take = [&](int64_t x, int64_t y) -> int64_t {
    auto it = ends.find(key(x, y));
    if (it == ends.end()) return -1;
    for (size_t end : it->second)
      if (!used[end >> 1]) return static_cast<int64_t>(end);
    return -1;
  };
  std::vector<Ring> out;
  for (size_t i = 0; i < parts.size(); i++) {
    if (used[i]) continue;
    used[i] = 1;
    const Ring& first = parts[i];
    if (first.size() < 4) {
      out.push_back(first);
      continue;
    }
    Ring forward = first;
    while (true) {
      const int64_t end = take(forward[forward.size() - 2], forward[forward.size() - 1]);
      if (end < 0) break;
      used[end >> 1] = 1;
      const Ring& next = parts[end >> 1];
      if ((end & 1) == 0) {
        for (size_t k = 2; k < next.size(); k++) forward.push_back(next[k]);
      } else {
        for (int64_t k = static_cast<int64_t>(next.size()) - 4; k >= 0; k -= 2) {
          forward.push_back(next[k]);
          forward.push_back(next[k + 1]);
        }
      }
    }
    std::vector<Ring> heads;
    int64_t hx = forward[0], hy = forward[1];
    while (true) {
      const int64_t end = take(hx, hy);
      if (end < 0) break;
      used[end >> 1] = 1;
      const Ring& next = parts[end >> 1];
      Ring piece;
      if ((end & 1) == 1) {
        for (size_t k = 0; k + 2 < next.size(); k++) piece.push_back(next[k]);
      } else {
        for (int64_t k = static_cast<int64_t>(next.size()) - 2; k >= 2; k -= 2) {
          piece.push_back(next[k]);
          piece.push_back(next[k + 1]);
        }
      }
      if (piece.empty()) break;
      heads.push_back(piece);
      hx = piece[0];
      hy = piece[1];
    }
    Ring merged;
    for (auto h = heads.rbegin(); h != heads.rend(); ++h) merged.insert(merged.end(), h->begin(), h->end());
    merged.insert(merged.end(), forward.begin(), forward.end());
    out.push_back(std::move(merged));
  }
  return out;
}

void installSymbolBuilder() {
  setSymbolBuilder([](const LayerProgram& program, const FeatureSource& source, const std::vector<size_t>& selected, int z,
                      FeatureTable& table) -> std::shared_ptr<void> {
    auto layout = layoutFor(program, z);
    auto labels = std::make_shared<PackedLabels>();
    struct Street {
      LabelInfo info;
      uint32_t index;
      std::vector<Ring> parts;
    };
    std::vector<std::pair<std::string, Street>> streets;
    std::unordered_map<std::string, size_t> streetIndex;
    const uint32_t extent = source.extent();
    for (size_t f : selected) {
      FeatureContext feature;
      feature.properties = &source.properties(f);
      const auto id = source.id(f);
      feature.id = id ? Value(*id) : Value();
      feature.geometryType = geometryTypeOf(source.type(f));
      EvalContext ctx;
      ctx.zoom = z;
      ctx.feature = &feature;
      const uint32_t index = static_cast<uint32_t>(table.count());
      auto record = [&] {
        FeatureRecord r;
        r.id = source.id(f);
        r.textId = source.textId(f);
        r.source = source.sourceIndex(f);
        r.type = source.type(f);
        const Object& props = source.properties(f);
        for (const auto& key : program.keys)
          for (const auto& [k, v] : props)
            if (k == key) {
              r.properties.emplace_back(k, v);
              break;
            }
        table.features.push_back(std::move(r));
      };
      if (layout->placement != "point" && source.type(f) == 2) {
        auto info = labelInfo(ctx, *layout);
        if (!info) continue;
        auto parts = source.geometry(f);
        if (parts.empty()) continue;
        const std::string key = formatNumber(info->size) + "|" + info->text + "|" + info->icon;
        if (auto it = streetIndex.find(key); it != streetIndex.end()) {
          Street& s = streets[it->second].second;
          s.info.priority = std::max(s.info.priority, info->priority);
          for (auto& p : parts) s.parts.push_back(std::move(p));
          continue;
        }
        streetIndex[key] = streets.size();
        streets.push_back({key, Street{*info, index, std::move(parts)}});
        record();
        continue;
      }
      bool added = false;
      labelCandidates(source, f, index, ctx, *layout, extent, *labels, added);
      if (added) record();
    }
    for (auto& [key, s] : streets) lineLabels(mergeParts(s.parts), s.info, s.index, *layout, extent, *labels);
    if (labels->count() == 0) return nullptr;
    return labels;
  });
}

}
