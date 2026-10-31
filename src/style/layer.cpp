#include "style/layer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

namespace mapkern {

double nowMs() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return duration<double, std::milli>(steady_clock::now() - start).count();
}

namespace {

std::atomic<uint64_t> nextUid{1};

const char* kTypeNames[] = {"custom", "background", "raster", "fill", "line", "circle", "heatmap", "symbol", "extrusion", "hillshade"};

PropertyDef p(OutputKind kind, Value def, bool feature = true) {
  PropertyDef d;
  d.kind = kind;
  d.def = std::move(def);
  d.feature = feature;
  return d;
}

PropertyDef checked(OutputKind kind, Value def, bool feature, Check check) {
  PropertyDef d = p(kind, std::move(def), feature);
  d.check = std::move(check);
  return d;
}

PropertyDef fixedDef(Value def, Check check) {
  PropertyDef d;
  d.kind = OutputKind::Any;
  d.def = std::move(def);
  d.expression = false;
  d.feature = false;
  d.check = std::move(check);
  return d;
}

Check oneOf(std::vector<std::string> values) {
  return [values](const Value& v) -> std::optional<std::string> {
    if (v.isString() && std::find(values.begin(), values.end(), v.asString()) != values.end()) return std::nullopt;
    std::string joined;
    for (size_t i = 0; i < values.size(); i++) joined += (i ? ", " : "") + values[i];
    return "must be one of " + joined;
  };
}

Check numberAtLeast(double mn) {
  return [mn](const Value& v) -> std::optional<std::string> {
    if (v.isNumber() && std::isfinite(v.asNumber()) && v.asNumber() >= mn) return std::nullopt;
    return "must be a number >= " + formatNumber(mn);
  };
}

std::optional<std::string> numberQuad(const Value& v) {
  if (v.isArray() && v.size() == 4) {
    bool ok = true;
    for (const auto& n : v.asArray()) ok = ok && n.isNumber() && std::isfinite(n.asNumber());
    if (ok) return std::nullopt;
  }
  return "must be [top, right, bottom, left] in pixels";
}

std::optional<std::string> numberOrNull(const Value& v) {
  if (v.isNull() || (v.isNumber() && std::isfinite(v.asNumber()))) return std::nullopt;
  return "must be a number";
}

std::optional<std::string> numberPair(const Value& v) {
  if (v.isArray() && v.size() == 2 && v[0].isNumber() && v[1].isNumber() && std::isfinite(v[0].asNumber()) &&
      std::isfinite(v[1].asNumber()))
    return std::nullopt;
  return "must be [number, number]";
}

std::optional<std::string> isBoolean(const Value& v) {
  if (v.isBool()) return std::nullopt;
  return "must be true or false";
}

std::optional<std::string> numberList(const Value& v) {
  if (v.isArray() && v.size() > 0 && v.size() <= 16) {
    bool ok = true;
    for (const auto& n : v.asArray()) ok = ok && n.isNumber() && n.asNumber() >= 0;
    if (ok) return std::nullopt;
  }
  return "must be a list of up to 16 non-negative numbers";
}

const std::vector<std::string> kAnchors = {"center", "top", "bottom", "left", "right", "top-left", "top-right", "bottom-left", "bottom-right"};

std::optional<std::string> anchorList(const Value& v) {
  if (v.isArray() && v.size() > 0) {
    bool ok = true;
    for (const auto& a : v.asArray())
      ok = ok && a.isString() && std::find(kAnchors.begin(), kAnchors.end(), a.asString()) != kAnchors.end();
    if (ok) return std::nullopt;
  }
  std::string joined;
  for (size_t i = 0; i < kAnchors.size(); i++) joined += (i ? ", " : "") + kAnchors[i];
  return "must be a list of anchors: " + joined;
}

std::optional<std::string> stringList(const Value& v) {
  if (v.isArray() && v.size() > 0) {
    bool ok = true;
    for (const auto& s : v.asArray()) ok = ok && s.isString();
    if (ok) return std::nullopt;
  }
  return "must be a list of strings";
}

Value defaultHeat() {
  return *parseJson(R"(["interpolate",["linear"],["heatmap-density"],0,"rgba(47, 111, 219, 0)",0.15,"#2f6fdb",0.35,"#2db3c4",0.55,"#7ccf6b",0.75,"#f2d544",1,"#e4572e"])");
}

using K = OutputKind;

std::vector<PropertyTable> buildPaint() {
  std::vector<PropertyTable> t(10);
  t[1] = {{"color", p(K::Color, "#000000", false)}, {"opacity", p(K::Number, 1, false)}};
  t[2] = {{"opacity", p(K::Number, 1, false)},
          {"brightnessMin", p(K::Number, 0, false)},
          {"brightnessMax", p(K::Number, 1, false)},
          {"contrast", p(K::Number, 0, false)},
          {"saturation", p(K::Number, 0, false)},
          {"hueRotate", p(K::Number, 0, false)},
          {"fadeDuration", fixedDef(200, numberAtLeast(0))},
          {"resampling", fixedDef("linear", oneOf({"linear", "nearest"}))}};
  t[3] = {{"color", p(K::Color, "#000000")},
          {"opacity", p(K::Number, 1)},
          {"outlineColor", p(K::Color, Value())},
          {"pattern", p(K::String, "", false)},
          {"translate", checked(K::Any, Array{0, 0}, false, numberPair)},
          {"translateAnchor", fixedDef("map", oneOf({"map", "viewport"}))},
          {"antialias", checked(K::Boolean, true, false, isBoolean)}};
  t[4] = {{"offset", p(K::Number, 0)},       {"gapWidth", p(K::Number, 0)}, {"color", p(K::Color, "#000000")},
          {"width", p(K::Number, 1)},        {"opacity", p(K::Number, 1)},  {"blur", p(K::Number, 0)},
          {"dash", checked(K::Any, Value(), false, numberList)}};
  t[5] = {{"radius", p(K::Number, 5)},
          {"color", p(K::Color, "#000000")},
          {"opacity", p(K::Number, 1)},
          {"strokeColor", p(K::Color, "#000000")},
          {"strokeWidth", p(K::Number, 0)},
          {"strokeOpacity", p(K::Number, 1)},
          {"blur", p(K::Number, 0)},
          {"translate", checked(K::Any, Array{0, 0}, false, numberPair)},
          {"translateAnchor", fixedDef("map", oneOf({"map", "viewport"}))},
          {"pitchScale", fixedDef("viewport", oneOf({"map", "viewport"}))},
          {"pitchAlignment", fixedDef("viewport", oneOf({"map", "viewport"}))}};
  PropertyDef heat = p(K::Color, defaultHeat(), false);
  heat.transition = false;
  t[6] = {{"radius", p(K::Number, 30)},
          {"weight", p(K::Number, 1)},
          {"intensity", p(K::Number, 1, false)},
          {"color", heat},
          {"opacity", p(K::Number, 1, false)}};
  t[7] = {{"color", p(K::Color, "#000000")},
          {"haloColor", p(K::Color, "rgba(0, 0, 0, 0)")},
          {"haloWidth", p(K::Number, 0)},
          {"haloBlur", p(K::Number, 0)},
          {"opacity", p(K::Number, 1)},
          {"translate", checked(K::Any, Array{0, 0}, false, numberPair)},
          {"translateAnchor", fixedDef("map", oneOf({"map", "viewport"}))},
          {"iconTranslate", checked(K::Any, Array{0, 0}, false, numberPair)},
          {"iconTranslateAnchor", fixedDef("map", oneOf({"map", "viewport"}))},
          {"iconColor", p(K::Color, "#000000")},
          {"iconHaloColor", p(K::Color, "rgba(0, 0, 0, 0)")},
          {"iconHaloWidth", p(K::Number, 0)},
          {"iconOpacity", p(K::Number, 1)}};
  t[8] = {{"color", p(K::Color, "#000000")}, {"height", p(K::Number, 0)}, {"base", p(K::Number, 0)}, {"opacity", p(K::Number, 1, false)}};
  t[9] = {{"exaggeration", p(K::Number, 0.5, false)},
          {"shadowColor", p(K::Color, "#000000", false)},
          {"highlightColor", p(K::Color, "#ffffff", false)},
          {"accentColor", p(K::Color, "#000000", false)},
          {"illuminationDirection", fixedDef(335, numberAtLeast(0))},
          {"illuminationAnchor", fixedDef("viewport", oneOf({"viewport", "map"}))},
          {"method", fixedDef("lambert", oneOf({"lambert", "standard"}))}};
  return t;
}

std::vector<PropertyTable> buildLayout() {
  std::vector<PropertyTable> t(10);
  t[4] = {{"cap", fixedDef("butt", oneOf({"butt", "round", "square"}))},
          {"join", fixedDef("miter", oneOf({"miter", "round", "bevel"}))},
          {"miterLimit", fixedDef(2, numberAtLeast(1))}};
  const auto alignment = oneOf({"auto", "map", "viewport"});
  t[7] = {{"text", p(K::String, "")},
          {"font", checked(K::Any, Array{"sans-serif"}, true, stringList)},
          {"size", p(K::Number, 14)},
          {"weight", checked(K::String, "normal", true, oneOf({"normal", "bold"}))},
          {"italic", checked(K::Boolean, false, true, isBoolean)},
          {"placement", checked(K::String, "point", false, oneOf({"point", "line", "line-center"}))},
          {"anchor", checked(K::String, "center", true, oneOf(kAnchors))},
          {"variableAnchor", fixedDef(Value(), anchorList)},
          {"radialOffset", checked(K::Any, Value(), true, numberOrNull)},
          {"justify", checked(K::String, "center", true, oneOf({"auto", "left", "center", "right"}))},
          {"lineHeight", checked(K::Number, 1.2, false, numberAtLeast(0))},
          {"rotate", p(K::Number, 0)},
          {"rotationAlignment", checked(K::String, "auto", false, alignment)},
          {"pitchAlignment", checked(K::String, "auto", false, alignment)},
          {"ignorePlacement", checked(K::Boolean, false, false, isBoolean)},
          {"iconIgnorePlacement", checked(K::Boolean, false, false, isBoolean)},
          {"maxAngle", checked(K::Number, 45, false, numberAtLeast(0))},
          {"keepUpright", checked(K::Boolean, true, false, isBoolean)},
          {"offset", checked(K::Any, Array{0, 0}, true, numberPair)},
          {"maxWidth", checked(K::Number, 10, true, numberAtLeast(0))},
          {"letterSpacing", checked(K::Number, 0, true, numberAtLeast(-1))},
          {"padding", checked(K::Number, 2, false, numberAtLeast(0))},
          {"spacing", checked(K::Number, 250, false, numberAtLeast(1))},
          {"allowOverlap", checked(K::Boolean, false, false, isBoolean)},
          {"transform", checked(K::String, "none", true, oneOf({"none", "uppercase", "lowercase"}))},
          {"priority", p(K::Number, 0)},
          {"optional", checked(K::Boolean, false, false, isBoolean)},
          {"icon", p(K::String, "")},
          {"iconSize", p(K::Number, 1)},
          {"iconRotate", p(K::Number, 0)},
          {"iconAnchor", checked(K::String, "center", true, oneOf(kAnchors))},
          {"iconOffset", checked(K::Any, Array{0, 0}, true, numberPair)},
          {"iconPadding", checked(K::Number, 2, false, numberAtLeast(0))},
          {"iconAllowOverlap", checked(K::Boolean, false, false, isBoolean)},
          {"iconOptional", checked(K::Boolean, false, false, isBoolean)},
          {"iconRotationAlignment", checked(K::String, "auto", false, alignment)},
          {"iconTextFit", checked(K::String, "none", false, oneOf({"none", "width", "height", "both"}))},
          {"iconTextFitPadding", checked(K::Any, Array{0, 0, 0, 0}, false, numberQuad)}};
  t[8] = {{"hideOutlines", fixedDef(Value(), isBoolean)}};
  return t;
}

const std::vector<PropertyTable>& paintTables() {
  static const auto t = buildPaint();
  return t;
}

const std::vector<PropertyTable>& layoutTables() {
  static const auto t = buildLayout();
  return t;
}

const PropertyDef* findIn(const PropertyTable& table, std::string_view name) {
  for (const auto& [k, d] : table)
    if (k == name) return &d;
  return nullptr;
}

double ease(double t) { return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2; }

const PropertyValue& missingProperty() {
  static const PropertyValue v(compile(Value(), OutputKind::Any, Value()));
  return v;
}

}

std::optional<LayerType> layerTypeFromName(std::string_view name) {
  for (int i = 0; i < 10; i++)
    if (name == kTypeNames[i]) return static_cast<LayerType>(i);
  return std::nullopt;
}

const char* layerTypeName(LayerType type) { return kTypeNames[static_cast<int>(type)]; }

const PropertyTable& paintTable(LayerType type) { return paintTables()[static_cast<int>(type)]; }
const PropertyTable& layoutTable(LayerType type) { return layoutTables()[static_cast<int>(type)]; }
const PropertyDef* findPaint(LayerType type, std::string_view name) { return findIn(paintTable(type), name); }
const PropertyDef* findLayout(LayerType type, std::string_view name) { return findIn(layoutTable(type), name); }

std::optional<TransitionTiming> timing(const Value& spec) {
  if (!spec.isObject()) return std::nullopt;
  const double duration = spec["duration"].numberOr(300);
  const double delay = spec["delay"].numberOr(0);
  if (duration > 0 || delay > 0) return TransitionTiming{duration, delay};
  return std::nullopt;
}

std::optional<std::string> checkTransition(const Value& value) {
  if (!value.isObject()) return "must be { duration, delay }";
  for (const auto& [key, v] : value.asObject()) {
    if (key != "duration" && key != "delay") return "\"" + key + "\" is not part of a transition, use duration and delay";
    if (!v.isNumber() || !std::isfinite(v.asNumber()) || v.asNumber() < 0) return key + " must be a number of milliseconds >= 0";
  }
  return std::nullopt;
}

Value blend(OutputKind kind, const Value& a, const Value& b, double t) {
  if (kind == OutputKind::Number) {
    if (a.isNumber() && b.isNumber()) return a.asNumber() + (b.asNumber() - a.asNumber()) * t;
    return b;
  }
  if (!a.isColor() || !b.isColor()) return b;
  const Color& ca = a.asColor();
  const Color& cb = b.asColor();
  const double alpha = ca[3] + (cb[3] - ca[3]) * t;
  if (alpha <= 0) return Color{0, 0, 0, 0};
  Color out{0, 0, 0, alpha};
  for (int i = 0; i < 3; i++) {
    const double from = ca[i] * ca[3];
    out[i] = (from + (cb[i] * cb[3] - from) * t) / alpha;
  }
  return out;
}

PropertyValue::PropertyValue(OutputKind kind, std::shared_ptr<PropertyValue> prior, PropertyValue target, double begin,
                             double end)
    : target_(target.target_), prior_(std::move(prior)), kind_(kind), begin_(begin), end_(end) {}

bool PropertyValue::settle(double now) {
  if (!prior_) return false;
  if (now >= end_) {
    prior_ = nullptr;
    return false;
  }
  prior_->settle(now);
  return true;
}

Value PropertyValue::evaluate(const EvalContext& ctx, double now) const {
  if (!prior_ || now >= end_) return target_->evaluate(ctx);
  const Value from = prior_->evaluate(ctx, now);
  if (now <= begin_) return from;
  return blend(kind_, from, target_->evaluate(ctx), ease((now - begin_) / (end_ - begin_)));
}

double PropertyValue::number(const EvalContext& ctx, double now) const {
  const Value v = evaluate(ctx, now);
  return v.isNumber() ? v.asNumber() : target_->fallback.numberOr(0);
}

Color PropertyValue::color(const EvalContext& ctx, double now) const {
  const Value v = evaluate(ctx, now);
  if (v.isColor()) return v.asColor();
  return {0, 0, 0, 0};
}

std::string PropertyValue::string(const EvalContext& ctx, double now) const {
  const Value v = evaluate(ctx, now);
  return v.isString() ? v.asString() : target_->fallback.stringOr("");
}

bool PropertyValue::boolean(const EvalContext& ctx, double now) const {
  const Value v = evaluate(ctx, now);
  return v.isBool() ? v.asBool() : target_->fallback.boolOr(false);
}

bool PropertyValue::zoomDependent() const { return target_->zoomDependent || (prior_ && prior_->zoomDependent()); }
bool PropertyValue::featureDependent() const { return target_->featureDependent || (prior_ && prior_->featureDependent()); }
bool PropertyValue::stateDependent() const { return target_->stateDependent || (prior_ && prior_->stateDependent()); }
bool PropertyValue::isNullConstant() const { return constant() && target_->evaluate({}).isNull(); }

bool isTransitionable(const PropertyDef& def) {
  return def.expression && (def.kind == OutputKind::Number || def.kind == OutputKind::Color) && def.transition;
}

const PropertyDef* transitionOf(LayerType type, std::string_view name) {
  constexpr std::string_view suffix = "Transition";
  if (!name.ends_with(suffix)) return nullptr;
  const PropertyDef* def = findPaint(type, name.substr(0, name.size() - suffix.size()));
  return def && isTransitionable(*def) ? def : nullptr;
}

Value literalList(const Value& value) {
  if (value.isArray() && value.size() > 0 && !isOperator(value[0].isString() ? value[0].asString() : "")) {
    bool allStrings = true;
    for (const auto& v : value.asArray()) allStrings = allStrings && v.isString();
    if (allStrings) return Array{Value("literal"), value};
  }
  return value;
}

bool isPlainValue(const Value& value, const PropertyDef& def) {
  if (!value.isArray()) return true;
  return def.kind == OutputKind::Any && !(value[0].isString() && isOperator(value[0].asString()));
}

StyleLayer::StyleLayer(const Value& s)
    : uid(nextUid++),
      id(s["id"].stringOr("")),
      type([&] {
        const std::string name = s["type"].stringOr("");
        auto t = layerTypeFromName(name);
        if (!t) throw StyleError("layer \"" + s["id"].stringOr("") + "\" has unknown type \"" + name + "\"");
        return *t;
      }()),
      spec(s) {
  source = s["source"].stringOr("");
  sourceLayer = s["sourceLayer"].stringOr("");
  minZoom = s["minZoom"].numberOr(0);
  maxZoom = s["maxZoom"].numberOr(24);
  visible = !(s["visible"].isBool() && !s["visible"].asBool());
  const Value& paintSpec = s["paint"];
  const Value& layoutSpec = s["layout"];
  for (const auto& [name, def] : paintTable(type)) setProperty(paint_, "paint", name, def, paintSpec[name]);
  for (const auto& [name, def] : layoutTable(type)) setProperty(layout_, "layout", name, def, layoutSpec[name]);
  if (s.has("filter")) filter = compile(s["filter"], OutputKind::Boolean, false, id + ".filter", {false, true});
}

void StyleLayer::setProperty(std::map<std::string, PropertyValue, std::less<>>& target, const char* group,
                             const std::string& name, const PropertyDef& def, const Value& value) {
  const bool missing = value.isNull();
  if (!def.expression) {
    if (!missing && def.check) {
      if (auto problem = def.check(value)) throw StyleError(id + "." + group + "." + name + " " + *problem);
    }
    fixed_[name] = missing ? def.def : value;
    return;
  }
  if (!missing && def.check && isPlainValue(value, def)) {
    if (auto problem = def.check(value)) throw StyleError(id + "." + group + "." + name + " " + *problem);
  }
  Value fallback = def.kind == OutputKind::Any ? def.def : coerce(def.def, def.kind);
  if (fallback.isNull() && def.kind == OutputKind::Color && def.def.isArray()) fallback = Value();
  try {
    target[name] = PropertyValue(compile(literalList(missing ? def.def : value), def.kind, fallback,
                                         id + "." + group + "." + name, {std::string_view(group) == "paint", false}));
  } catch (const ExpressionError& e) {
    throw StyleError(e.what());
  }
}

bool StyleLayer::usesState() const {
  for (const auto& [k, v] : paint_)
    if (v.stateDependent()) return true;
  return false;
}

bool StyleLayer::usesZoom() const {
  for (const auto& [k, v] : paint_)
    if (v.zoomDependent()) return true;
  return false;
}

const PropertyValue& StyleLayer::paint(std::string_view name) const {
  auto it = paint_.find(name);
  return it == paint_.end() ? missingProperty() : it->second;
}

const PropertyValue& StyleLayer::layout(std::string_view name) const {
  auto it = layout_.find(name);
  return it == layout_.end() ? missingProperty() : it->second;
}

bool StyleLayer::hasPaint(std::string_view name) const { return paint_.find(name) != paint_.end(); }

Value StyleLayer::paintValue(std::string_view name, const EvalContext& ctx) const { return paint(name).evaluate(ctx, clock_); }
Value StyleLayer::layoutValue(std::string_view name, const EvalContext& ctx) const { return layout(name).evaluate(ctx, clock_); }
double StyleLayer::paintNumber(std::string_view name, const EvalContext& ctx) const { return paint(name).number(ctx, clock_); }
Color StyleLayer::paintColor(std::string_view name, const EvalContext& ctx) const { return paint(name).color(ctx, clock_); }
double StyleLayer::layoutNumber(std::string_view name, const EvalContext& ctx) const { return layout(name).number(ctx, clock_); }
std::string StyleLayer::layoutString(std::string_view name, const EvalContext& ctx) const {
  return layout(name).string(ctx, clock_);
}
bool StyleLayer::layoutBool(std::string_view name, const EvalContext& ctx) const { return layout(name).boolean(ctx, clock_); }

const Value& StyleLayer::fixed(std::string_view name) const {
  auto it = fixed_.find(name);
  return it == fixed_.end() ? Value::null() : it->second;
}

void StyleLayer::editSpec(const char* group, const std::string& name, const Value& value) {
  Object root = spec.isObject() ? spec.asObject() : Object{};
  Object section = spec[group].isObject() ? spec[group].asObject() : Object{};
  if (value.isNull()) {
    section.erase(std::remove_if(section.begin(), section.end(), [&](const auto& kv) { return kv.first == name; }), section.end());
  } else {
    section = withKey(std::move(section), name, value);
  }
  spec = Value(withKey(std::move(root), group, Value(std::move(section))));
}

void StyleLayer::setPaint(const std::string& name, const Value& value, const Value& fallbackTransition) {
  if (transitionOf(type, name)) {
    if (!value.isNull()) {
      if (auto problem = checkTransition(value)) throw StyleError(name + " " + *problem);
    }
    editSpec("paint", name, value);
    return;
  }
  const PropertyDef* def = findPaint(type, name);
  if (!def) throw StyleError("\"" + name + "\" is not a paint property of " + layerTypeName(type) + " layers");
  std::optional<PropertyValue> before;
  if (auto it = paint_.find(name); it != paint_.end()) before = it->second;
  setProperty(paint_, "paint", name, *def, value);
  editSpec("paint", name, value);
  auto after = paint_.find(name);
  if (before && after != paint_.end() && isTransitionable(*def) && !before->isNullConstant() && !after->second.isNullConstant()) {
    const Value& own = spec["paint"][name + "Transition"];
    if (auto t = timing(own.isNull() ? fallbackTransition : own)) {
      const double begin = nowMs() + t->delay;
      after->second = PropertyValue(def->kind, std::make_shared<PropertyValue>(*before), after->second, begin, begin + t->duration);
      moving_.insert(name);
    }
  }
  revision++;
}

bool StyleLayer::settle(double now) {
  clock_ = now;
  if (moving_.empty()) return false;
  for (auto it = moving_.begin(); it != moving_.end();) {
    auto p = paint_.find(*it);
    if (p == paint_.end() || !p->second.settle(now)) it = moving_.erase(it);
    else ++it;
  }
  motion++;
  return !moving_.empty();
}

void StyleLayer::setLayout(const std::string& name, const Value& value) {
  const PropertyDef* def = findLayout(type, name);
  if (!def) throw StyleError("\"" + name + "\" is not a layout property of " + layerTypeName(type) + " layers");
  setProperty(layout_, "layout", name, *def, value);
  editSpec("layout", name, value);
  revision++;
}

void StyleLayer::setFilter(const Value& f) {
  if (f.isNull()) filter.reset();
  else filter = compile(f, OutputKind::Boolean, false, id + ".filter", {false, true});
  Object root = spec.asObject();
  if (f.isNull()) root.erase(std::remove_if(root.begin(), root.end(), [](const auto& kv) { return kv.first == "filter"; }), root.end());
  else root = withKey(std::move(root), "filter", f);
  spec = Value(std::move(root));
  revision++;
}

void StyleLayer::setZoomRange(double lo, double hi) {
  if (lo != minZoom || hi != maxZoom) revision++;
  minZoom = lo;
  maxZoom = hi;
  Object root = spec.asObject();
  root = withKey(std::move(root), "minZoom", lo);
  root = withKey(std::move(root), "maxZoom", hi);
  spec = Value(std::move(root));
}

void StyleLayer::setVisible(bool v) {
  if (v != visible) revision++;
  visible = v;
  spec = Value(withKey(spec.asObject(), "visible", v));
}

void StyleLayer::restyle(const Value& next, const Update& update, const Value& fallbackTransition) {
  for (const auto& [name, value] : update.paint)
    if (transitionOf(type, name)) setPaint(name, value, fallbackTransition);
  for (const auto& [name, value] : update.paint)
    if (!transitionOf(type, name)) setPaint(name, value, fallbackTransition);
  for (const auto& [name, value] : update.layout) setLayout(name, value);
  if (update.filter) setFilter(next["filter"]);
  if (update.zoom) setZoomRange(next["minZoom"].numberOr(0), next["maxZoom"].numberOr(24));
  if (update.visible) setVisible(!(next["visible"].isBool() && !next["visible"].asBool()));
  spec = next;
}

}
