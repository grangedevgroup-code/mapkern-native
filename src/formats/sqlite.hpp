#pragma once

#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "formats/bytes.hpp"

namespace mapkern {

using SqlValue = std::variant<std::monostate, double, std::string, Bytes>;

struct SchemaRow {
  std::string type;
  std::string name;
  std::string table;
  uint32_t root = 0;
  std::string sql;
};

class SqliteReader {
 public:
  explicit SqliteReader(std::shared_ptr<ByteSource> source);
  void open();
  std::vector<SchemaRow> schema();
  void scan(uint32_t root, const std::function<void(int64_t rowid, std::vector<SqlValue>& values)>& visit);
  std::optional<std::vector<SqlValue>> row(uint32_t root, int64_t rowid);
  std::optional<std::vector<SqlValue>> lookup(uint32_t root, const std::vector<SqlValue>& key);

 private:
  std::shared_ptr<const Bytes> page(uint32_t n);
  std::vector<std::shared_ptr<const Bytes>> pages(uint32_t first, uint32_t count);
  std::vector<SqlValue> tableLeafCell(const Bytes& page, size_t cell, int64_t* rowid);
  std::vector<SqlValue> indexCell(const Bytes& page, size_t cell);
  Bytes payload(const Bytes& page, size_t start, uint64_t size, uint64_t maxLocal);
  std::vector<SqlValue> record(const Bytes& payload) const;

  std::shared_ptr<ByteSource> source_;
  uint32_t pageSize_ = 0;
  uint32_t usable_ = 0;
  uint32_t encoding_ = 1;
  bool opened_ = false;
  std::mutex mutex_;
  std::list<uint32_t> order_;
  std::unordered_map<uint32_t, std::pair<std::shared_ptr<const Bytes>, std::list<uint32_t>::iterator>> cache_;
};

int compareKey(const std::vector<SqlValue>& record, const std::vector<SqlValue>& key);

}
