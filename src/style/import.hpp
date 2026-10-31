#pragma once

#include <string>
#include <vector>

#include "core/value.hpp"

namespace mapkern {

struct ImportedStyle {
  Value style;
  std::vector<std::string> skipped;
};

struct ImportedLayer {
  Value layer;
  std::vector<std::string> skipped;
};

struct ImportedSource {
  Value source;
  std::vector<std::string> skipped;
};

struct NamedFont {
  std::vector<std::string> families;
  bool bold = false;
  bool italic = false;
};

bool isStyleV8(const Value& value);
ImportedStyle importStyle(const Value& input, const std::string& base = "");
ImportedLayer importLayer(const Value& raw, const Value& sources, bool serverFonts);
ImportedSource importSource(const std::string& id, const Value& raw, const std::string& base = "");
Value convertFilter(const Value& filter);
Value tokens(const std::string& text);
NamedFont fontFromNames(const std::vector<std::string>& names);
std::string serverGlyphs(const Value& glyphs);

}
