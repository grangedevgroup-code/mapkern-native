#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include "core/value.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  try {
    std::string error;
    if (const auto value = mapkern::parseJson(text, &error)) {
      const std::string again = value->toJson();
      mapkern::parseJson(again, &error);
    }
  } catch (const std::exception&) {
  }
  return 0;
}
