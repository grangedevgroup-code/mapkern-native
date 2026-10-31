#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mapkern {

using Color = std::array<double, 4>;

class Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

class Value {
 public:
  enum class Type : uint8_t { Null, Bool, Number, String, Array, Object, Color };

  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool b) : v_(b) {}
  template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
  Value(T n) : v_(static_cast<double>(n)) {}
  Value(double n) : v_(n) {}
  Value(float n) : v_(static_cast<double>(n)) {}
  Value(const char* s) : v_(std::string(s)) {}
  Value(std::string s) : v_(std::move(s)) {}
  Value(std::string_view s) : v_(std::string(s)) {}
  Value(Array a) : v_(std::make_shared<const mapkern::Array>(std::move(a))) {}
  Value(Object o) : v_(std::make_shared<const mapkern::Object>(std::move(o))) {}
  Value(Color c) : v_(c) {}

  Type type() const { return static_cast<Type>(v_.index()); }
  bool isNull() const { return v_.index() == 0; }
  bool isBool() const { return v_.index() == 1; }
  bool isNumber() const { return v_.index() == 2; }
  bool isString() const { return v_.index() == 3; }
  bool isArray() const { return v_.index() == 4; }
  bool isObject() const { return v_.index() == 5; }
  bool isColor() const { return v_.index() == 6; }

  bool asBool() const { return std::get<1>(v_); }
  double asNumber() const { return std::get<2>(v_); }
  const std::string& asString() const { return std::get<3>(v_); }
  const mapkern::Array& asArray() const { return *std::get<4>(v_); }
  const mapkern::Object& asObject() const { return *std::get<5>(v_); }
  const Color& asColor() const { return std::get<6>(v_); }

  bool boolOr(bool fallback) const { return isBool() ? asBool() : fallback; }
  double numberOr(double fallback) const { return isNumber() ? asNumber() : fallback; }
  std::string stringOr(std::string fallback) const { return isString() ? asString() : std::move(fallback); }

  const Value* find(std::string_view key) const;
  const Value& operator[](std::string_view key) const;
  const Value& operator[](size_t index) const;
  size_t size() const;
  bool has(std::string_view key) const { return find(key) != nullptr; }

  bool operator==(const Value& other) const;
  bool sameIdentity(const Value& other) const;

  std::string toJson() const;
  std::string toDisplayString() const;

  static const Value& null();

 private:
  std::variant<std::monostate, bool, double, std::string, std::shared_ptr<const mapkern::Array>,
               std::shared_ptr<const mapkern::Object>, Color>
      v_;
};

std::optional<Value> parseJson(std::string_view text, std::string* error = nullptr);
std::string formatNumber(double v);
Object withKey(Object object, std::string key, Value value);

}
