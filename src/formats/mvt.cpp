#include "formats/mvt.hpp"

#include <cmath>

#include "formats/protobuf.hpp"

namespace mapkern {

namespace {

constexpr size_t kMaxPoints = size_t{1} << 22;

Value readValue(pbf::Reader r) {
  Value value("");
  while (!r.done()) {
    const uint32_t tag = r.tag();
    switch (tag >> 3) {
      case 1: value = r.string(); break;
      case 2: value = static_cast<double>(r.float32()); break;
      case 3: value = r.float64(); break;
      case 4: value = static_cast<double>(r.int64()); break;
      case 5: value = static_cast<double>(r.varint64()); break;
      case 6: value = static_cast<double>(r.svarint64()); break;
      case 7: value = r.boolean(); break;
      default: r.skip(static_cast<int>(tag & 7));
    }
  }
  return value;
}

uint32_t imul(uint32_t a, uint32_t b) { return a * b; }

double textHash(const std::string& text) {
  uint32_t a = 0x811c9dc5u, b = 0x9e3779b9u;
  for (uint32_t c : text) {
    a = imul(a ^ (c & 0xff), 0x01000193u);
    b = imul(b ^ (c & 0xff), 0x5bd1e995u);
    b ^= b >> 15;
  }
  a ^= a >> 16;
  a = imul(a, 0x85ebca6bu);
  a ^= a >> 13;
  b ^= b >> 16;
  b = imul(b, 0xc2b2ae35u);
  b ^= b >> 16;
  return static_cast<double>(b >> 11) * 4294967296.0 + a;
}

}

VectorLayer::VectorLayer(std::shared_ptr<const std::vector<uint8_t>> data, size_t start, size_t end) : data_(std::move(data)) {
  pbf::Reader r(data_->data(), start, end);
  while (!r.done()) {
    const uint32_t tag = r.tag();
    const uint32_t field = tag >> 3;
    const int wire = static_cast<int>(tag & 7);
    if (field == 1 && wire == pbf::kBytes) {
      name = r.string();
    } else if (field == 2 && wire == pbf::kBytes) {
      const size_t e = r.lengthEnd();
      features_.emplace_back(r.pos, e);
      r.pos = e;
    } else if (field == 3 && wire == pbf::kBytes) {
      keys_.push_back(r.string());
    } else if (field == 4 && wire == pbf::kBytes) {
      values_.push_back(readValue(r.sub()));
    } else if (field == 5) {
      extent = r.varint();
    } else if (field == 15) {
      version = r.varint();
    } else {
      r.skip(wire);
    }
  }
  if (extent == 0 || extent > (1u << 20)) throw FormatError("Layer " + name + " has an invalid extent " + std::to_string(extent));
}

VectorFeature VectorLayer::feature(size_t index) const {
  if (index >= features_.size()) throw FormatError("Feature index out of range");
  const auto [start, end] = features_[index];
  VectorFeature f;
  f.buf = data_->data();
  f.extent = extent;
  f.keys = &keys_;
  f.values = &values_;
  pbf::Reader r(f.buf, start, end);
  while (!r.done()) {
    const uint32_t tag = r.tag();
    const uint32_t field = tag >> 3;
    if (field == 1) {
      f.id = static_cast<double>(r.varint64());
    } else if (field == 2) {
      f.tagEnd = r.lengthEnd();
      f.tagStart = r.pos;
      f.hasTags = true;
      r.pos = f.tagEnd;
    } else if (field == 3) {
      const uint32_t t = r.varint();
      f.type = t >= 1 && t <= 3 ? static_cast<int>(t) : 0;
    } else if (field == 4) {
      f.geomEnd = r.lengthEnd();
      f.geomStart = r.pos;
      f.hasGeometry = true;
      r.pos = f.geomEnd;
    } else {
      r.skip(static_cast<int>(tag & 7));
    }
  }
  return f;
}

Object VectorFeature::properties() const {
  Object props;
  if (!hasTags) return props;
  pbf::Reader t(buf, tagStart, tagEnd);
  while (!t.done()) {
    const uint32_t k = t.varint();
    if (t.done()) break;
    const uint32_t v = t.varint();
    if (k < keys->size() && v < values->size()) {
      const std::string& key = (*keys)[k];
      bool replaced = false;
      for (auto& [name, value] : props)
        if (name == key) {
          value = (*values)[v];
          replaced = true;
          break;
        }
      if (!replaced) props.emplace_back(key, (*values)[v]);
    }
  }
  return props;
}

const Value* VectorFeature::property(std::string_view key) const {
  if (!hasTags) return nullptr;
  const Value* found = nullptr;
  pbf::Reader t(buf, tagStart, tagEnd);
  while (!t.done()) {
    const uint32_t k = t.varint();
    if (t.done()) break;
    const uint32_t v = t.varint();
    if (k < keys->size() && v < values->size() && (*keys)[k] == key) found = &(*values)[v];
  }
  return found;
}

std::vector<Ring> VectorFeature::geometry() const {
  if (!hasGeometry) return {};
  return decodeGeometry(buf, geomStart, geomEnd, type);
}

std::vector<Ring> decodeGeometry(const uint8_t* buf, size_t start, size_t end, int type) {
  pbf::Reader r(buf, start, end);
  std::vector<Ring> parts;
  Ring current;
  int32_t x = 0, y = 0;
  size_t total = 0;
  while (!r.done()) {
    const uint32_t cmd = r.varint();
    const uint32_t id = cmd & 7;
    const uint32_t count = cmd >> 3;
    if (id == 1 || id == 2) {
      if (count == 0) continue;
      total += count;
      if (total > kMaxPoints) throw FormatError("Feature geometry is too large");
      for (uint32_t i = 0; i < count; i++) {
        if (r.done()) throw FormatError("Geometry command runs past the end of the feature");
        x += r.svarint();
        if (r.done()) throw FormatError("Geometry command runs past the end of the feature");
        y += r.svarint();
        if (id == 1 && type != 1) {
          if (!current.empty()) parts.push_back(std::move(current));
          current.clear();
        }
        current.push_back(x);
        current.push_back(y);
      }
    } else if (id == 7) {
      if (current.size() >= 2) {
        current.push_back(current[0]);
        current.push_back(current[1]);
      }
    } else {
      throw FormatError("Unknown geometry command " + std::to_string(id));
    }
  }
  if (!current.empty()) parts.push_back(std::move(current));
  return parts;
}

VectorTile decodeVectorTile(std::shared_ptr<const std::vector<uint8_t>> bytes) {
  VectorTile layers;
  pbf::Reader r(bytes->data(), bytes->size());
  while (!r.done()) {
    const uint32_t tag = r.tag();
    if ((tag >> 3) == 3 && (tag & 7) == pbf::kBytes) {
      const size_t e = r.lengthEnd();
      auto layer = std::make_shared<VectorLayer>(bytes, r.pos, e);
      r.pos = e;
      layers.emplace(layer->name, std::move(layer));
    } else {
      r.skip(static_cast<int>(tag & 7));
    }
  }
  return layers;
}

double ringArea(const Ring& ring) {
  double sum = 0;
  const size_t n = ring.size();
  if (n < 2) return 0;
  for (size_t i = 0, j = n - 2; i < n; j = i, i += 2)
    sum += static_cast<double>(ring[j] - ring[i]) * static_cast<double>(ring[i + 1] + ring[j + 1]);
  return sum / 2;
}

std::vector<std::vector<Ring>> classifyRings(const std::vector<Ring>& rings) {
  std::vector<std::vector<Ring>> polygons;
  int exteriorSign = 0;
  bool hasCurrent = false;
  for (const auto& ring : rings) {
    if (ring.size() < 8) continue;
    const double area = ringArea(ring);
    if (area == 0) continue;
    const int sign = area > 0 ? 1 : -1;
    if (exteriorSign == 0) exteriorSign = sign;
    if (sign == exteriorSign || !hasCurrent) {
      polygons.push_back({ring});
      hasCurrent = true;
    } else {
      polygons.back().push_back(ring);
    }
  }
  return polygons;
}

std::optional<double> numericId(const Value& value) {
  if (value.isNumber()) return std::isfinite(value.asNumber()) ? std::optional<double>(value.asNumber()) : std::nullopt;
  if (value.isString()) {
    const std::string& s = value.asString();
    if (s.empty() || s.size() > 15) return std::nullopt;
    for (char c : s)
      if (c < '0' || c > '9') return std::nullopt;
    return std::stod(s);
  }
  return std::nullopt;
}

std::optional<double> featureKey(const Value& value) {
  auto n = numericId(value);
  if (n || !value.isString() || value.asString().empty()) return n;
  return -1 - textHash(value.asString());
}

}
