#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "core/value.hpp"
#include "style/expression.hpp"

namespace mapkern {

enum class LayerType { Custom, Background, Raster, Fill, Line, Circle, Heatmap, Symbol, Extrusion, Hillshade };

std::optional<LayerType> layerTypeFromName(std::string_view name);
const char* layerTypeName(LayerType type);

using Check = std::function<std::optional<std::string>(const Value&)>;

struct PropertyDef {
  OutputKind kind = OutputKind::Any;
  Value def;
  bool expression = true;
  bool feature = true;
  bool transition = true;
  Check check;
};

using PropertyTable = std::vector<std::pair<std::string, PropertyDef>>;

const PropertyTable& paintTable(LayerType type);
const PropertyTable& layoutTable(LayerType type);
const PropertyDef* findPaint(LayerType type, std::string_view name);
const PropertyDef* findLayout(LayerType type, std::string_view name);

class StyleError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct TransitionTiming {
  double duration = 0;
  double delay = 0;
};

std::optional<TransitionTiming> timing(const Value& spec);
std::optional<std::string> checkTransition(const Value& value);
Value blend(OutputKind kind, const Value& a, const Value& b, double t);

class PropertyValue {
 public:
  PropertyValue() = default;
  explicit PropertyValue(Compiled c) : target_(std::make_shared<Compiled>(std::move(c))) {}
  PropertyValue(OutputKind kind, std::shared_ptr<PropertyValue> prior, PropertyValue target, double begin, double end);

  Value evaluate(const EvalContext& ctx, double now) const;
  double number(const EvalContext& ctx, double now) const;
  Color color(const EvalContext& ctx, double now) const;
  std::string string(const EvalContext& ctx, double now) const;
  bool boolean(const EvalContext& ctx, double now) const;

  bool moving() const { return prior_ != nullptr; }
  bool settle(double now);
  const Compiled& compiled() const { return *target_; }
  bool constant() const { return !prior_ && target_->constant; }
  bool zoomDependent() const;
  bool featureDependent() const;
  bool stateDependent() const;
  bool isNullConstant() const;

 private:
  std::shared_ptr<Compiled> target_;
  std::shared_ptr<PropertyValue> prior_;
  OutputKind kind_ = OutputKind::Any;
  double begin_ = 0;
  double end_ = 0;
};

class StyleLayer {
 public:
  explicit StyleLayer(const Value& spec);

  const uint64_t uid;
  const std::string id;
  const LayerType type;
  Value spec;
  std::string source;
  std::string sourceLayer;
  double minZoom = 0;
  double maxZoom = 24;
  bool visible = true;
  std::optional<Compiled> filter;
  uint64_t revision = 0;
  uint64_t motion = 0;

  bool usesState() const;
  bool usesZoom() const;
  bool isVisible(double zoom) const { return visible && (minZoom <= 0 || zoom >= minZoom) && zoom < maxZoom; }

  const PropertyValue& paint(std::string_view name) const;
  const PropertyValue& layout(std::string_view name) const;
  bool hasPaint(std::string_view name) const;
  Value paintValue(std::string_view name, const EvalContext& ctx) const;
  Value layoutValue(std::string_view name, const EvalContext& ctx) const;
  double paintNumber(std::string_view name, const EvalContext& ctx) const;
  Color paintColor(std::string_view name, const EvalContext& ctx) const;
  double layoutNumber(std::string_view name, const EvalContext& ctx) const;
  std::string layoutString(std::string_view name, const EvalContext& ctx) const;
  bool layoutBool(std::string_view name, const EvalContext& ctx) const;
  const Value& fixed(std::string_view name) const;

  void setPaint(const std::string& name, const Value& value, const Value& fallbackTransition = Value());
  void setLayout(const std::string& name, const Value& value);
  void setFilter(const Value& filter);
  void setZoomRange(double minZoom, double maxZoom);
  void setVisible(bool visible);
  bool animating() const { return !moving_.empty(); }
  bool settle(double now);
  double clock() const { return clock_; }
  Value serialize() const { return spec; }

  struct Update {
    std::vector<std::pair<std::string, Value>> paint;
    std::vector<std::pair<std::string, Value>> layout;
    bool filter = false;
    bool zoom = false;
    bool visible = false;
  };
  void restyle(const Value& next, const Update& update, const Value& fallbackTransition = Value());

 private:
  void setProperty(std::map<std::string, PropertyValue, std::less<>>& target, const char* group, const std::string& name,
                   const PropertyDef& def, const Value& value);
  void editSpec(const char* group, const std::string& name, const Value& value);

  std::map<std::string, PropertyValue, std::less<>> paint_;
  std::map<std::string, PropertyValue, std::less<>> layout_;
  std::map<std::string, Value, std::less<>> fixed_;
  std::set<std::string> moving_;
  double clock_ = 0;
};

bool isTransitionable(const PropertyDef& def);
const PropertyDef* transitionOf(LayerType type, std::string_view name);
Value literalList(const Value& value);
bool isPlainValue(const Value& value, const PropertyDef& def);
double nowMs();

}
