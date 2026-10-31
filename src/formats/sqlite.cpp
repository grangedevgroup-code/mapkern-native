#include "formats/sqlite.hpp"

#include <cstring>

namespace mapkern {

namespace {

constexpr size_t kCachePages = 512;

uint32_t u32(const uint8_t* b) {
  return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) | (static_cast<uint32_t>(b[2]) << 8) | b[3];
}

uint64_t varint(const uint8_t* bytes, size_t size, size_t at, size_t* next) {
  uint64_t value = 0;
  for (int i = 0; i < 8; i++) {
    if (at + i >= size) throw FormatError("SQLite varint runs past the page");
    const uint8_t b = bytes[at + i];
    value = (value << 7) | (b & 0x7f);
    if (b < 0x80) {
      *next = at + i + 1;
      return value;
    }
  }
  if (at + 8 >= size) throw FormatError("SQLite varint runs past the page");
  *next = at + 9;
  return (value << 8) | bytes[at + 8];
}

std::vector<size_t> cellOffsets(const Bytes& page, size_t h) {
  if (h + 8 > page.size()) throw FormatError("SQLite page header is truncated");
  const size_t count = (page[h + 3] << 8) | page[h + 4];
  const uint8_t type = page[h];
  const size_t start = h + (type == 0x02 || type == 0x05 ? 12 : 8);
  if (start + count * 2 > page.size()) throw FormatError("SQLite cell pointers run past the page");
  std::vector<size_t> out(count);
  for (size_t i = 0; i < count; i++) out[i] = (page[start + i * 2] << 8) | page[start + i * 2 + 1];
  return out;
}

int rank(const SqlValue& v) { return static_cast<int>(v.index()); }

int compareValue(const SqlValue& a, const SqlValue& b) {
  if (rank(a) != rank(b)) return rank(a) - rank(b);
  if (auto* x = std::get_if<double>(&a)) {
    const double y = std::get<double>(b);
    return *x < y ? -1 : *x > y ? 1 : 0;
  }
  if (auto* x = std::get_if<std::string>(&a)) return x->compare(std::get<std::string>(b)) < 0 ? -1 : (*x == std::get<std::string>(b) ? 0 : 1);
  if (auto* x = std::get_if<Bytes>(&a)) {
    const Bytes& y = std::get<Bytes>(b);
    const size_t n = std::min(x->size(), y.size());
    for (size_t i = 0; i < n; i++)
      if ((*x)[i] != y[i]) return (*x)[i] - y[i];
    return static_cast<int>(x->size()) - static_cast<int>(y.size());
  }
  return 0;
}

std::string utf16ToUtf8(const uint8_t* p, size_t n, bool little) {
  std::string out;
  for (size_t i = 0; i + 1 < n; i += 2) {
    uint32_t c = little ? (p[i] | (p[i + 1] << 8)) : ((p[i] << 8) | p[i + 1]);
    if (c >= 0xd800 && c < 0xdc00 && i + 3 < n) {
      const uint32_t lo = little ? (p[i + 2] | (p[i + 3] << 8)) : ((p[i + 2] << 8) | p[i + 3]);
      c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
      i += 2;
    }
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) {
      out += static_cast<char>(0xc0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xe0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (c & 0x3f));
    } else {
      out += static_cast<char>(0xf0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (c & 0x3f));
    }
  }
  return out;
}

}

SqliteReader::SqliteReader(std::shared_ptr<ByteSource> source) : source_(std::move(source)) {}

void SqliteReader::open() {
  std::lock_guard lock(mutex_);
  if (opened_) return;
  const Bytes header = source_->read(0, 100);
  if (header.size() < 100 || std::memcmp(header.data(), "SQLite format 3\0", 16) != 0)
    throw FormatError("The file is not an SQLite database");
  const uint32_t size = (header[16] << 8) | header[17];
  pageSize_ = size == 1 ? 65536 : size;
  if (pageSize_ < 512) throw FormatError("SQLite page size is invalid");
  usable_ = pageSize_ - header[20];
  encoding_ = u32(header.data() + 56);
  opened_ = true;
}

std::shared_ptr<const Bytes> SqliteReader::page(uint32_t n) {
  {
    std::lock_guard lock(mutex_);
    if (auto it = cache_.find(n); it != cache_.end()) {
      order_.splice(order_.end(), order_, it->second.second);
      return it->second.first;
    }
  }
  if (n == 0) throw FormatError("SQLite page 0 does not exist");
  auto data = std::make_shared<const Bytes>(source_->read(static_cast<uint64_t>(n - 1) * pageSize_, pageSize_));
  if (data->size() < pageSize_) throw FormatError("Page " + std::to_string(n) + " is past the end of the file");
  std::lock_guard lock(mutex_);
  if (cache_.contains(n)) return cache_[n].first;
  order_.push_back(n);
  cache_[n] = {data, std::prev(order_.end())};
  if (cache_.size() > kCachePages) {
    cache_.erase(order_.front());
    order_.pop_front();
  }
  return data;
}

std::vector<std::shared_ptr<const Bytes>> SqliteReader::pages(uint32_t first, uint32_t count) {
  {
    std::lock_guard lock(mutex_);
    if (count <= 1 || cache_.contains(first)) count = 1;
  }
  if (count <= 1) return {page(first)};
  const Bytes data = source_->read(static_cast<uint64_t>(first - 1) * pageSize_, static_cast<size_t>(count) * pageSize_);
  std::vector<std::shared_ptr<const Bytes>> out;
  for (size_t i = 0; i + pageSize_ <= data.size(); i += pageSize_)
    out.push_back(std::make_shared<const Bytes>(data.begin() + static_cast<ptrdiff_t>(i), data.begin() + static_cast<ptrdiff_t>(i + pageSize_)));
  if (out.empty()) throw FormatError("Page " + std::to_string(first) + " is past the end of the file");
  return out;
}

std::vector<SchemaRow> SqliteReader::schema() {
  std::vector<SchemaRow> rows;
  scan(1, [&](int64_t, std::vector<SqlValue>& v) {
    SchemaRow r;
    auto str = [&](size_t i) -> std::string {
      if (i < v.size())
        if (auto* s = std::get_if<std::string>(&v[i])) return *s;
      return "";
    };
    r.type = str(0);
    r.name = str(1);
    r.table = str(2);
    if (v.size() > 3)
      if (auto* d = std::get_if<double>(&v[3])) r.root = static_cast<uint32_t>(*d);
    r.sql = str(4);
    rows.push_back(std::move(r));
  });
  return rows;
}

void SqliteReader::scan(uint32_t root, const std::function<void(int64_t, std::vector<SqlValue>&)>& visit) {
  open();
  std::function<void(uint32_t, int)> walk = [&](uint32_t n, int depth) {
    if (depth > 64) throw FormatError("The table is nested too deeply");
    auto p = page(n);
    const size_t h = n == 1 ? 100 : 0;
    const uint8_t type = (*p)[h];
    const auto cells = cellOffsets(*p, h);
    if (type == 0x05) {
      for (size_t c : cells) walk(u32(p->data() + c), depth + 1);
      walk(u32(p->data() + h + 8), depth + 1);
    } else if (type == 0x0d) {
      for (size_t c : cells) {
        int64_t rowid;
        auto values = tableLeafCell(*p, c, &rowid);
        visit(rowid, values);
      }
    } else {
      throw FormatError("Page " + std::to_string(n) + " is not a table page");
    }
  };
  walk(root, 0);
}

std::optional<std::vector<SqlValue>> SqliteReader::row(uint32_t root, int64_t rowid) {
  open();
  uint32_t n = root;
  for (int depth = 0; depth < 64; depth++) {
    auto p = page(n);
    const size_t h = n == 1 ? 100 : 0;
    const uint8_t type = (*p)[h];
    const auto cells = cellOffsets(*p, h);
    if (type == 0x05) {
      uint32_t next = u32(p->data() + h + 8);
      for (size_t c : cells) {
        size_t after;
        const int64_t key = static_cast<int64_t>(varint(p->data(), p->size(), c + 4, &after));
        if (rowid <= key) {
          next = u32(p->data() + c);
          break;
        }
      }
      n = next;
    } else if (type == 0x0d) {
      for (size_t c : cells) {
        size_t afterSize, after;
        varint(p->data(), p->size(), c, &afterSize);
        const int64_t key = static_cast<int64_t>(varint(p->data(), p->size(), afterSize, &after));
        if (key == rowid) {
          int64_t id;
          return tableLeafCell(*p, c, &id);
        }
      }
      return std::nullopt;
    } else {
      throw FormatError("Page " + std::to_string(n) + " is not a table page");
    }
  }
  throw FormatError("The table is nested too deeply");
}

std::optional<std::vector<SqlValue>> SqliteReader::lookup(uint32_t root, const std::vector<SqlValue>& key) {
  open();
  uint32_t n = root;
  for (int depth = 0; depth < 64; depth++) {
    auto p = page(n);
    const size_t h = n == 1 ? 100 : 0;
    const uint8_t type = (*p)[h];
    const auto cells = cellOffsets(*p, h);
    if (type == 0x02) {
      uint32_t next = u32(p->data() + h + 8);
      for (size_t c : cells) {
        auto values = indexCell(*p, c + 4);
        const int order = compareKey(values, key);
        if (order == 0) return values;
        if (order > 0) {
          next = u32(p->data() + c);
          break;
        }
      }
      n = next;
    } else if (type == 0x0a) {
      for (size_t c : cells) {
        auto values = indexCell(*p, c);
        if (compareKey(values, key) == 0) return values;
      }
      return std::nullopt;
    } else {
      throw FormatError("Page " + std::to_string(n) + " is not an index page");
    }
  }
  throw FormatError("The index is nested too deeply");
}

std::vector<SqlValue> SqliteReader::tableLeafCell(const Bytes& page, size_t cell, int64_t* rowid) {
  size_t afterSize, start;
  const uint64_t size = varint(page.data(), page.size(), cell, &afterSize);
  *rowid = static_cast<int64_t>(varint(page.data(), page.size(), afterSize, &start));
  return record(payload(page, start, size, usable_ - 35));
}

std::vector<SqlValue> SqliteReader::indexCell(const Bytes& page, size_t cell) {
  size_t start;
  const uint64_t size = varint(page.data(), page.size(), cell, &start);
  const uint64_t limit = (static_cast<uint64_t>(usable_ - 12) * 64) / 255 - 23;
  return record(payload(page, start, size, limit));
}

Bytes SqliteReader::payload(const Bytes& page, size_t start, uint64_t size, uint64_t maxLocal) {
  if (size > (1u << 30)) throw FormatError("SQLite record is too large");
  if (size <= maxLocal) {
    if (start + size > page.size()) throw FormatError("SQLite record runs past the page");
    return Bytes(page.begin() + static_cast<ptrdiff_t>(start), page.begin() + static_cast<ptrdiff_t>(start + size));
  }
  const uint64_t minLocal = (static_cast<uint64_t>(usable_ - 12) * 32) / 255 - 23;
  const uint64_t k = minLocal + (size - minLocal) % (usable_ - 4);
  const uint64_t local = k <= maxLocal ? k : minLocal;
  if (start + local + 4 > page.size()) throw FormatError("SQLite record runs past the page");
  Bytes out(static_cast<size_t>(size));
  std::memcpy(out.data(), page.data() + start, static_cast<size_t>(local));
  uint64_t filled = local;
  uint32_t next = u32(page.data() + start + local);
  const uint64_t chunk = usable_ - 4;
  int guard = 0;
  while (filled < size) {
    if (next == 0 || ++guard > 1000000) throw FormatError("A record runs past the end of its overflow pages");
    const auto run = pages(next, static_cast<uint32_t>((size - filled + chunk - 1) / chunk));
    uint32_t current = next;
    for (const auto& p : run) {
      const uint64_t take = std::min(chunk, size - filled);
      std::memcpy(out.data() + filled, p->data() + 4, static_cast<size_t>(take));
      filled += take;
      next = u32(p->data());
      if (filled >= size || next != current + 1) break;
      current = next;
    }
  }
  return out;
}

std::vector<SqlValue> SqliteReader::record(const Bytes& payload) const {
  size_t first;
  const uint64_t headerSize = varint(payload.data(), payload.size(), 0, &first);
  std::vector<uint64_t> types;
  size_t p = first;
  while (p < headerSize && p < payload.size()) {
    size_t next;
    types.push_back(varint(payload.data(), payload.size(), p, &next));
    p = next;
  }
  std::vector<SqlValue> values;
  values.reserve(types.size());
  size_t o = static_cast<size_t>(headerSize);
  auto need = [&](size_t n) {
    if (o + n > payload.size()) throw FormatError("SQLite record is truncated");
  };
  auto be = [&](size_t n) {
    need(n);
    int64_t v = static_cast<int8_t>(payload[o]);
    for (size_t i = 1; i < n; i++) v = (v << 8) | payload[o + i];
    o += n;
    return static_cast<double>(v);
  };
  for (const uint64_t t : types) {
    switch (t) {
      case 0: values.emplace_back(std::monostate{}); break;
      case 1: values.emplace_back(be(1)); break;
      case 2: values.emplace_back(be(2)); break;
      case 3: values.emplace_back(be(3)); break;
      case 4: values.emplace_back(be(4)); break;
      case 5: values.emplace_back(be(6)); break;
      case 6: values.emplace_back(be(8)); break;
      case 7: {
        need(8);
        uint64_t bits = 0;
        for (int i = 0; i < 8; i++) bits = (bits << 8) | payload[o + i];
        double d;
        std::memcpy(&d, &bits, 8);
        o += 8;
        values.emplace_back(d);
        break;
      }
      case 8: values.emplace_back(0.0); break;
      case 9: values.emplace_back(1.0); break;
      default:
        if (t >= 12) {
          const size_t n = static_cast<size_t>(t % 2 == 0 ? (t - 12) / 2 : (t - 13) / 2);
          need(n);
          if (t % 2 == 0) {
            values.emplace_back(Bytes(payload.begin() + static_cast<ptrdiff_t>(o), payload.begin() + static_cast<ptrdiff_t>(o + n)));
          } else if (encoding_ == 2 || encoding_ == 3) {
            values.emplace_back(utf16ToUtf8(payload.data() + o, n, encoding_ == 2));
          } else {
            values.emplace_back(std::string(reinterpret_cast<const char*>(payload.data() + o), n));
          }
          o += n;
        } else {
          values.emplace_back(std::monostate{});
        }
    }
  }
  return values;
}

int compareKey(const std::vector<SqlValue>& record, const std::vector<SqlValue>& key) {
  for (size_t i = 0; i < key.size(); i++) {
    const SqlValue a = i < record.size() ? record[i] : SqlValue{};
    const int order = compareValue(a, key[i]);
    if (order != 0) return order;
  }
  return 0;
}

}
