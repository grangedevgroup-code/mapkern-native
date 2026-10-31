#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/value.hpp"

namespace mapkern {

enum class GeometryType { Point, LineString, Polygon, Unknown };

const char* geometryTypeName(GeometryType t);

struct WorldGeometry {
  int type = 0;
  std::vector<std::vector<double>> parts;
};

struct FeatureContext {
  const Object* properties = nullptr;
  Value id;
  GeometryType geometryType = GeometryType::Unknown;
  std::function<const WorldGeometry*()> geometry;
};

struct EvalContext {
  double zoom = 0;
  const FeatureContext* feature = nullptr;
  const Object* state = nullptr;
  const std::function<bool(const std::string&)>* images = nullptr;
  const Value* accumulated = nullptr;
  double density = 0;
};

enum class OutputKind { Number, Color, String, Boolean, Any };

struct CompileFlags {
  bool state = false;
  bool geometry = false;
};

class ExpressionError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

using EvalFn = std::function<Value(const EvalContext&)>;

class Compiled {
 public:
  Value evaluate(const EvalContext& ctx) const;
  double number(const EvalContext& ctx) const;
  Color color(const EvalContext& ctx) const;
  std::string string(const EvalContext& ctx) const;
  bool boolean(const EvalContext& ctx) const;

  bool zoomDependent = false;
  bool featureDependent = false;
  bool stateDependent = false;
  bool constant = true;
  std::shared_ptr<const std::unordered_set<std::string>> properties;

  Value constantValue;
  EvalFn fn;
  OutputKind kind = OutputKind::Any;
  Value fallback;
};

bool isExpression(const Value& value);
bool isOperator(std::string_view name);
Compiled compile(const Value& value, OutputKind kind, Value fallback, const std::string& path = "value",
                 CompileFlags flags = {});
Value coerce(const Value& v, OutputKind kind);

}
