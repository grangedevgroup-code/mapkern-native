#include "render/paint_table.hpp"

#include <algorithm>

namespace mapkern {

namespace {

GeometryType geometryTypeOf(int t) {
  switch (t) {
    case 1: return GeometryType::Point;
    case 2: return GeometryType::LineString;
    case 3: return GeometryType::Polygon;
    default: return GeometryType::Unknown;
  }
}

void write(float* data, size_t at, const Value& v) {
  if (v.isNumber()) {
    data[at] = static_cast<float>(v.asNumber());
    data[at + 1] = data[at + 2] = data[at + 3] = 0;
  } else if (v.isColor()) {
    const auto& c = v.asColor();
    for (int i = 0; i < 4; i++) data[at + i] = static_cast<float>(c[i]);
  }
}

}

const Object* FeatureStates::get(const std::string& sourceLayer, double id) const {
  auto it = states_.find(sourceLayer);
  if (it == states_.end()) return nullptr;
  auto f = it->second.find(id);
  return f == it->second.end() ? nullptr : &f->second;
}

void FeatureStates::set(const std::string& sourceLayer, double id, const Object& state) {
  Object& current = states_[sourceLayer][id];
  for (const auto& [k, v] : state) current = withKey(std::move(current), k, v);
  version_++;
}

void FeatureStates::remove(const std::string& sourceLayer, double id, const std::string& key) {
  auto it = states_.find(sourceLayer);
  if (it == states_.end()) return;
  if (key.empty()) {
    it->second.erase(id);
  } else if (auto f = it->second.find(id); f != it->second.end()) {
    std::erase_if(f->second, [&](const auto& kv) { return kv.first == key; });
  }
  version_++;
}

void FeatureStates::clear(const std::string& sourceLayer) {
  if (sourceLayer.empty()) states_.clear();
  else states_.erase(sourceLayer);
  version_++;
}

PaintSlot PaintTable::slot(const std::string& name) const {
  auto it = slots_.find(name);
  return it == slots_.end() ? PaintSlot{} : it->second;
}

void PaintTable::update(const StyleLayer& layer, const FeatureTable& features, const std::vector<std::string>& names, double zoom,
                        const FeatureStates* states, uint64_t stateVersion) {
  const bool layoutChanged = layer.revision != revision_;
  if (layoutChanged) {
    revision_ = layer.revision;
    slots_.clear();
    names_.clear();
    stride_ = 0;
    hasComposite_ = false;
    hasState_ = false;
    for (const auto& name : names) {
      const PropertyValue& p = layer.paint(name);
      if (!p.featureDependent()) continue;
      const bool composite = p.zoomDependent();
      slots_[name] = {stride_, composite};
      names_.push_back(name);
      stride_ += composite ? 2 : 1;
      hasComposite_ = hasComposite_ || composite;
      hasState_ = hasState_ || p.stateDependent();
    }
    evaluatedZoom_ = NAN;
  }
  if (stride_ == 0) {
    release();
    return;
  }
  const double base = std::floor(zoom);
  const bool zoomStale = hasComposite_ && base != evaluatedZoom_;
  const bool stateStale = hasState_ && states && stateVersion != stateVersion_;
  const bool moved = layer.animating() && layer.motion != motion_;
  if (!layoutChanged && !zoomStale && !stateStale && !moved && texture_.valid()) return;
  evaluatedZoom_ = base;
  stateVersion_ = stateVersion;
  motion_ = layer.motion;
  const size_t total = std::max<size_t>(1, features.count() * stride_);
  const int width = static_cast<int>(std::min<size_t>(total, 2048));
  const int height = static_cast<int>((total + width - 1) / width);
  std::vector<float> data(static_cast<size_t>(width) * height * 4, 0.0f);
  for (size_t f = 0; f < features.count(); f++) evaluate(layer, features, f, base, states, data.data());
  texture_.release();
  texture_ = Gpu::get().floatTexture(width, height, data.data());
  width_ = width;
}

void PaintTable::evaluate(const StyleLayer& layer, const FeatureTable& features, size_t f, double base, const FeatureStates* states,
                          float* data) {
  const FeatureRecord& rec = features.features[f];
  FeatureContext feature;
  feature.properties = &rec.properties;
  feature.id = !rec.textId.empty() ? Value(rec.textId) : rec.id ? Value(*rec.id) : Value();
  feature.geometryType = geometryTypeOf(rec.type);
  EvalContext ctx;
  ctx.feature = &feature;
  if (states && rec.id) ctx.state = states->get(layer.sourceLayer, *rec.id);
  for (const auto& name : names_) {
    const PaintSlot s = slots_[name];
    const size_t at = (f * stride_ + s.slot) * 4;
    ctx.zoom = base;
    write(data, at, layer.paintValue(name, ctx));
    if (s.composite) {
      ctx.zoom = base + 1;
      write(data, at + 4, layer.paintValue(name, ctx));
    }
  }
}

}
