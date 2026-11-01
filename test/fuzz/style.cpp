#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include "core/value.hpp"
#include "style/import.hpp"
#include "style/validate.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  try {
    const auto value = mapkern::parseJson(text);
    if (!value) return 0;
    if (mapkern::isStyleV8(*value)) {
      const mapkern::ImportedStyle imported = mapkern::importStyle(*value, "https://example.com/style.json");
      mapkern::validateStyle(imported.style);
    } else {
      mapkern::validateStyle(*value);
    }
  } catch (const std::exception&) {
  }
  return 0;
}
