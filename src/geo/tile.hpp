#pragma once

#include <cstdint>
#include <string>

namespace mapkern {

inline constexpr int kMaxTileZoom = 24;

struct TileId {
  int z = 0;
  int x = 0;
  int y = 0;
  bool operator==(const TileId&) const = default;
};

struct TileSlot {
  int z = 0;
  int x = 0;
  int y = 0;
  int wrap = 0;
  TileId id() const { return {z, x, y}; }
  bool operator==(const TileSlot&) const = default;
};

struct Rect {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

inline uint64_t tileKey(int z, int x, int y) {
  return (static_cast<uint64_t>(z) << 50) | (static_cast<uint64_t>(y) << 25) | static_cast<uint64_t>(x);
}
inline uint64_t tileKey(TileId t) { return tileKey(t.z, t.x, t.y); }
inline TileId tileFromKey(uint64_t k) {
  return {static_cast<int>(k >> 50), static_cast<int>(k & 0x1ffffff), static_cast<int>((k >> 25) & 0x1ffffff)};
}

inline uint64_t slotKey(const TileSlot& s) {
  return tileKey(s.z, s.x, s.y) ^ (static_cast<uint64_t>(static_cast<uint32_t>(s.wrap) & 0x3fff) << 50 << 5);
}

inline bool isValidTile(int z, int x, int y) {
  if (z < 0 || z > kMaxTileZoom) return false;
  const int64_t n = int64_t{1} << z;
  return x >= 0 && x < n && y >= 0 && y < n;
}

inline TileId parentTile(TileId t, int levels = 1) {
  const int d = levels < t.z ? levels : t.z;
  return {t.z - d, t.x >> d, t.y >> d};
}

inline bool isAncestor(TileId ancestor, TileId t) {
  if (ancestor.z > t.z) return false;
  const int d = t.z - ancestor.z;
  return (t.x >> d) == ancestor.x && (t.y >> d) == ancestor.y;
}

inline Rect tileBounds(int z, int x, int y, int wrap = 0) {
  const double n = static_cast<double>(int64_t{1} << z);
  return {wrap + x / n, y / n, wrap + (x + 1) / n, (y + 1) / n};
}

inline Rect subRect(TileId ancestor, TileId t) {
  const int d = t.z - ancestor.z;
  const double n = static_cast<double>(int64_t{1} << d);
  const double ox = t.x - static_cast<double>(ancestor.x) * n;
  const double oy = t.y - static_cast<double>(ancestor.y) * n;
  return {ox / n, oy / n, (ox + 1) / n, (oy + 1) / n};
}

inline int tmsY(int z, int y) { return static_cast<int>((int64_t{1} << z) - 1 - y); }

inline std::string quadkey(int z, int x, int y) {
  std::string key;
  for (int i = z; i > 0; i--) {
    const int mask = 1 << (i - 1);
    int digit = 0;
    if (x & mask) digit += 1;
    if (y & mask) digit += 2;
    key += static_cast<char>('0' + digit);
  }
  return key;
}

}
