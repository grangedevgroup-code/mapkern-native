#include "worker/vector_task.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <regex>
#include <set>

#include "formats/inflate.hpp"
#include "style/import.hpp"

namespace mapkern {

namespace {

std::mutex symbolMutex;
SymbolBuilder symbolBuilder;

class MvtSource : public FeatureSource {
 public:
  explicit MvtSource(std::shared_ptr<VectorLayer> layer, std::optional<std::string> promoteKey)
      : layer_(std::move(layer)), promote_(std::move(promoteKey)) {
    const size_t n = layer_->size();
    features_.reserve(n);
    for (size_t i = 0; i < n; i++) features_.push_back(layer_->feature(i));
    props_.resize(n);
    ids_.resize(n);
    texts_.resize(n);
    if (promote_) {
      for (size_t i = 0; i < n; i++) {
        const Value* raw = features_[i].property(*promote_);
        if (!raw) continue;
        ids_[i] = featureKey(*raw);
        if (raw->isString() && ids_[i]) texts_[i] = raw->asString();
      }
    } else {
      for (size_t i = 0; i < n; i++) ids_[i] = features_[i].id;
    }
  }
  const std::string& name() const override { return layer_->name; }
  uint32_t extent() const override { return layer_->extent; }
  size_t size() const override { return features_.size(); }
  int type(size_t i) const override { return features_[i].type; }
  std::optional<double> id(size_t i) const override { return ids_[i]; }
  std::string textId(size_t i) const override { return texts_[i]; }
  const Object& properties(size_t i) const override {
    if (!props_[i]) props_[i] = features_[i].properties();
    return *props_[i];
  }
  std::vector<Ring> geometry(size_t i) const override { return features_[i].geometry(); }

 private:
  std::shared_ptr<VectorLayer> layer_;
  std::optional<std::string> promote_;
  std::vector<VectorFeature> features_;
  mutable std::vector<std::optional<Object>> props_;
  std::vector<std::optional<double>> ids_;
  std::vector<std::string> texts_;
};

GeometryType geometryType(int t) {
  switch (t) {
    case 1: return GeometryType::Point;
    case 2: return GeometryType::LineString;
    case 3: return GeometryType::Polygon;
    default: return GeometryType::Unknown;
  }
}

struct View {
  const FeatureSource* source;
  size_t index;
  FeatureContext ctx;
  std::optional<WorldGeometry> world;
};

void makeContext(View& v, int z, int tx, int ty) {
  v.ctx.properties = &v.source->properties(v.index);
  const auto id = v.source->id(v.index);
  const std::string text = v.source->textId(v.index);
  v.ctx.id = !text.empty() ? Value(text) : id ? Value(*id) : Value();
  v.ctx.geometryType = geometryType(v.source->type(v.index));
  View* self = &v;
  v.ctx.geometry = [self, z, tx, ty]() -> const WorldGeometry* {
    const int t = self->source->type(self->index);
    if (t < 1 || t > 3) return nullptr;
    if (!self->world) {
      WorldGeometry g;
      g.type = t;
      const double n = std::exp2(z);
      const double extent = self->source->extent();
      for (const auto& part : self->source->geometry(self->index)) {
        std::vector<double> out(part.size());
        for (size_t i = 0; i + 1 < part.size(); i += 2) {
          out[i] = (tx + part[i] / extent) / n;
          out[i + 1] = (ty + part[i + 1] / extent) / n;
        }
        g.parts.push_back(std::move(out));
      }
      self->world = std::move(g);
    }
    return &*self->world;
  };
}

void addFeature(FeatureTable& table, const FeatureSource& source, size_t index, const std::vector<std::string>& keys) {
  FeatureRecord r;
  r.id = source.id(index);
  r.textId = source.textId(index);
  r.source = source.sourceIndex(index);
  r.type = source.type(index);
  const Object& props = source.properties(index);
  for (const auto& key : keys)
    for (const auto& [k, v] : props)
      if (k == key) {
        r.properties.emplace_back(k, v);
        break;
      }
  table.features.push_back(std::move(r));
}

Geometry geometryOf(Growable& vertices, std::vector<uint32_t>& indices, uint32_t count) {
  Geometry g;
  g.vertices = std::move(vertices.bytes);
  g.indices = std::move(indices);
  g.vertexCount = count;
  return g;
}

std::optional<BucketData> buildBucket(const LayerProgram& program, const FeatureSource& source, const std::vector<View*>& selected, int z) {
  BucketData bucket;
  bucket.layerId = program.id;
  bucket.type = program.type;
  bucket.extent = source.extent();
  FeatureTable& features = bucket.features;
  switch (program.type) {
    case BucketType::Fill: {
      FillBuilder fill;
      for (View* f : selected) {
        const uint32_t index = static_cast<uint32_t>(features.count());
        const auto polygons = classifyRings(source.geometry(f->index));
        if (polygons.empty()) continue;
        for (const auto& rings : polygons) fill.addPolygon(rings, index);
        addFeature(features, source, f->index, program.keys);
      }
      if (fill.vertexCount == 0) return std::nullopt;
      const uint32_t vc = fill.vertexCount;
      if (!fill.edges.empty()) {
        Geometry outline;
        outline.indices = std::move(fill.edges);
        outline.vertexCount = vc;
        bucket.outline = std::move(outline);
      }
      bucket.main = geometryOf(fill.vertices, fill.indices, vc);
      return bucket;
    }
    case BucketType::Extrusion: {
      ExtrusionBuilder extrusion(source.extent());
      for (View* f : selected) {
        const uint32_t index = static_cast<uint32_t>(features.count());
        const auto polygons = classifyRings(source.geometry(f->index));
        if (polygons.empty()) continue;
        for (const auto& rings : polygons) extrusion.addPolygon(rings, index);
        addFeature(features, source, f->index, program.keys);
      }
      if (extrusion.vertexCount == 0) return std::nullopt;
      bucket.main = geometryOf(extrusion.vertices, extrusion.indices, extrusion.vertexCount);
      return bucket;
    }
    case BucketType::Line: {
      LineBuilder lines(program.line);
      for (View* f : selected) {
        const uint32_t index = static_cast<uint32_t>(features.count());
        const auto parts = source.geometry(f->index);
        if (parts.empty()) continue;
        const bool closed = source.type(f->index) == 3;
        for (const auto& part : parts) lines.addLine(part, index, closed);
        addFeature(features, source, f->index, program.keys);
      }
      if (lines.vertexCount == 0) return std::nullopt;
      bucket.main = geometryOf(lines.vertices, lines.indices, lines.vertexCount);
      return bucket;
    }
    case BucketType::Circle:
    case BucketType::Heatmap: {
      CircleBuilder circles;
      for (View* f : selected) {
        const uint32_t index = static_cast<uint32_t>(features.count());
        if (source.type(f->index) != 1) continue;
        bool added = false;
        for (const auto& part : source.geometry(f->index))
          for (size_t i = 0; i + 1 < part.size(); i += 2) {
            circles.addPoint(part[i], part[i + 1], index);
            added = true;
          }
        if (added) addFeature(features, source, f->index, program.keys);
      }
      if (circles.vertexCount == 0) return std::nullopt;
      bucket.main = geometryOf(circles.vertices, circles.indices, circles.vertexCount);
      return bucket;
    }
    case BucketType::Symbol: {
      SymbolBuilder builder;
      {
        std::lock_guard lock(symbolMutex);
        builder = symbolBuilder;
      }
      if (!builder) return std::nullopt;
      std::vector<size_t> indices;
      for (View* f : selected) indices.push_back(f->index);
      bucket.labels = builder(program, source, indices, z, features);
      if (!bucket.labels) return std::nullopt;
      return bucket;
    }
  }
  return std::nullopt;
}

}

void setSymbolBuilder(SymbolBuilder builder) {
  std::lock_guard lock(symbolMutex);
  symbolBuilder = std::move(builder);
}

std::optional<LayerProgram> layerProgram(const StyleLayer& layer) {
  LayerProgram p;
  switch (layer.type) {
    case LayerType::Fill: p.type = BucketType::Fill; break;
    case LayerType::Line: p.type = BucketType::Line; break;
    case LayerType::Circle: p.type = BucketType::Circle; break;
    case LayerType::Heatmap: p.type = BucketType::Heatmap; break;
    case LayerType::Symbol: p.type = BucketType::Symbol; break;
    case LayerType::Extrusion: p.type = BucketType::Extrusion; break;
    default: return std::nullopt;
  }
  p.id = layer.id;
  if (!layer.sourceLayer.empty()) p.sourceLayer = layer.sourceLayer;
  p.minZoom = layer.minZoom;
  p.maxZoom = layer.maxZoom;
  std::set<std::string> keys;
  for (const auto& [name, def] : paintTable(layer.type)) {
    if (!def.expression) continue;
    const auto& c = layer.paint(name).compiled();
    if (c.featureDependent && c.properties)
      for (const auto& k : *c.properties) keys.insert(k);
  }
  for (const auto& [name, def] : layoutTable(layer.type)) {
    if (!def.expression) continue;
    const auto& c = layer.layout(name).compiled();
    if (c.featureDependent && c.properties)
      for (const auto& k : *c.properties) keys.insert(k);
  }
  p.keys.assign(keys.begin(), keys.end());
  if (p.type == BucketType::Line) {
    p.line.cap = layer.fixed("cap").stringOr("butt");
    p.line.join = layer.fixed("join").stringOr("miter");
    p.line.miterLimit = layer.fixed("miterLimit").numberOr(2);
  }
  if (p.type == BucketType::Symbol) p.symbolLayout = layer.spec["layout"];
  if (layer.spec.has("filter")) p.filter = compile(layer.spec["filter"], OutputKind::Boolean, false, layer.id + ".filter", {false, true});
  if (layer.spec.has("sortKey")) p.sortKey = compile(layer.spec["sortKey"], OutputKind::Number, 0, layer.id + ".sortKey");
  Object sig{{"id", layer.id},
             {"type", layerTypeName(layer.type)},
             {"sourceLayer", layer.sourceLayer},
             {"filter", layer.spec["filter"]},
             {"minZoom", layer.minZoom},
             {"maxZoom", layer.maxZoom},
             {"sortKey", layer.spec["sortKey"]},
             {"cap", p.line.cap},
             {"join", p.line.join},
             {"miter", p.line.miterLimit},
             {"symbol", p.symbolLayout}};
  Array keyList;
  for (const auto& k : p.keys) keyList.emplace_back(k);
  sig.emplace_back("keys", keyList);
  p.signature = Value(sig).toJson();
  return p;
}

VectorTileResult buildBuckets(const std::vector<std::shared_ptr<FeatureSource>>& layers, int z, int x, int y,
                              const std::vector<LayerProgram>& programs, bool overzoom) {
  const auto started = std::chrono::steady_clock::now();
  VectorTileResult result;
  for (const auto& l : layers) result.layers.push_back(l->name());
  std::unordered_map<const FeatureSource*, std::vector<View>> views;
  for (const auto& program : programs) {
    if (program.maxZoom <= z) continue;
    if (!overzoom && program.minZoom >= z + 1) continue;
    const FeatureSource* source = nullptr;
    if (program.sourceLayer) {
      for (const auto& l : layers)
        if (l->name() == *program.sourceLayer) {
          source = l.get();
          break;
        }
    } else if (!layers.empty()) {
      source = layers.front().get();
    }
    if (!source) continue;
    auto it = views.find(source);
    if (it == views.end()) {
      std::vector<View> list(source->size());
      for (size_t i = 0; i < list.size(); i++) {
        list[i].source = source;
        list[i].index = i;
      }
      it = views.emplace(source, std::move(list)).first;
      for (auto& v : it->second) makeContext(v, z, x, y);
    }
    std::vector<View*> selected;
    for (auto& v : it->second) {
      const int t = source->type(v.index);
      if ((program.type == BucketType::Fill || program.type == BucketType::Extrusion) && t != 3) continue;
      if (program.type == BucketType::Line && t != 2 && t != 3) continue;
      if (program.filter) {
        EvalContext ctx;
        ctx.zoom = z;
        ctx.feature = &v.ctx;
        if (!program.filter->boolean(ctx)) continue;
      }
      selected.push_back(&v);
    }
    if (selected.empty()) continue;
    if (program.sortKey) {
      std::vector<std::pair<double, View*>> keyed;
      for (View* v : selected) {
        EvalContext ctx;
        ctx.zoom = z;
        ctx.feature = &v->ctx;
        keyed.emplace_back(program.sortKey->number(ctx), v);
      }
      std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
      for (size_t i = 0; i < keyed.size(); i++) selected[i] = keyed[i].second;
    }
    if (auto bucket = buildBucket(program, *source, selected, z)) {
      result.featureCount += bucket->features.count();
      result.buckets.push_back(std::move(*bucket));
    }
  }
  result.decodeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return result;
}

VectorTileResult buildVectorTile(const Bytes& bytes, int z, int x, int y, const std::vector<LayerProgram>& programs, bool overzoom,
                                 const PromoteId& promote) {
  auto data = std::make_shared<const Bytes>(decompressAuto(bytes));
  const VectorTile tile = decodeVectorTile(data);
  std::vector<std::shared_ptr<FeatureSource>> layers;
  for (const auto& [name, layer] : tile) {
    std::optional<std::string> key;
    if (auto* s = std::get_if<std::string>(&promote)) key = *s;
    else if (auto* m = std::get_if<std::vector<std::pair<std::string, std::string>>>(&promote))
      for (const auto& [ln, k] : *m)
        if (ln == name) key = k;
    layers.push_back(std::make_shared<MvtSource>(layer, key));
  }
  std::sort(layers.begin(), layers.end(), [](const auto& a, const auto& b) { return a->name() < b->name(); });
  return buildBuckets(layers, z, x, y, programs, overzoom);
}

}
