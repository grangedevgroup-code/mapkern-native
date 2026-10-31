#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mapkern::utf8 {

inline std::vector<uint32_t> decode(std::string_view s) {
  std::vector<uint32_t> out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0xfffd;
    size_t n = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c >> 5) == 6 && i + 1 < s.size()) {
      cp = ((c & 0x1f) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3f);
      n = 2;
    } else if ((c >> 4) == 14 && i + 2 < s.size()) {
      cp = ((c & 0x0f) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3f) << 6) |
           (static_cast<unsigned char>(s[i + 2]) & 0x3f);
      n = 3;
    } else if ((c >> 3) == 30 && i + 3 < s.size()) {
      cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3f) << 12) |
           ((static_cast<unsigned char>(s[i + 2]) & 0x3f) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3f);
      n = 4;
    }
    out.push_back(cp);
    i += n;
  }
  return out;
}

inline void append(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xc0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xe0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  } else {
    out += static_cast<char>(0xf0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  }
}

inline std::string encode(const std::vector<uint32_t>& cps, size_t from = 0, size_t to = SIZE_MAX) {
  std::string out;
  for (size_t i = from; i < cps.size() && i < to; i++) append(out, cps[i]);
  return out;
}

inline uint32_t toUpper(uint32_t c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  if ((c >= 0xe0 && c <= 0xfe && c != 0xf7)) return c - 32;
  if (c == 0xff) return 0x178;
  if (c >= 0x100 && c <= 0x17f && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17f) {
    const bool oddUpper = (c >= 0x139 && c <= 0x148) || c >= 0x179;
    if (oddUpper) return (c % 2 == 0) ? c - 1 : c;
    return (c % 2 == 1) ? c - 1 : c;
  }
  if (c >= 0x3b1 && c <= 0x3c9 && c != 0x3c2) return c - 32;
  if (c == 0x3c2) return 0x3a3;
  if (c >= 0x430 && c <= 0x44f) return c - 32;
  if (c >= 0x450 && c <= 0x45f) return c - 80;
  return c;
}

inline uint32_t toLower(uint32_t c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c >= 0xc0 && c <= 0xde && c != 0xd7) return c + 32;
  if (c == 0x178) return 0xff;
  if (c >= 0x100 && c <= 0x17f && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17f) {
    const bool oddUpper = (c >= 0x139 && c <= 0x148) || c >= 0x179;
    if (oddUpper) return (c % 2 == 1) ? c + 1 : c;
    return (c % 2 == 0) ? c + 1 : c;
  }
  if (c >= 0x391 && c <= 0x3a9 && c != 0x3a2) return c + 32;
  if (c >= 0x410 && c <= 0x42f) return c + 32;
  if (c >= 0x400 && c <= 0x40f) return c + 80;
  return c;
}

inline std::string upper(std::string_view s) {
  std::string out;
  for (uint32_t c : decode(s)) append(out, toUpper(c));
  return out;
}

inline std::string lower(std::string_view s) {
  std::string out;
  for (uint32_t c : decode(s)) append(out, toLower(c));
  return out;
}

}
