#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>

#include "formats/sqlite.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  try {
    mapkern::SqliteReader reader(std::make_shared<mapkern::MemorySource>(std::make_shared<const mapkern::Bytes>(data, data + size)));
    reader.open();
    int rows = 0;
    for (const auto& table : reader.schema()) {
      reader.scan(table.root, [&](int64_t, std::vector<mapkern::SqlValue>&) { rows++; });
      reader.row(table.root, 1);
      if (rows > 100000) break;
    }
  } catch (const std::exception&) {
  }
  return 0;
}
