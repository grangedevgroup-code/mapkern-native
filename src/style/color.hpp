#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/value.hpp"

namespace mapkern {

std::optional<Color> parseColor(std::string_view input);
Color premultiply(const Color& c, double opacity = 1);
Color mixColor(const Color& a, const Color& b, double t);
std::string colorToCss(const Color& c);
std::optional<Color> toColor(const Value& v);

}
