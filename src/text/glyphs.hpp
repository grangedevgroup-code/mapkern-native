#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/async.hpp"
#include "formats/bytes.hpp"
#include "tiles/cache.hpp"
#include "tiles/fetch.hpp"

namespace mapkern {

inline constexpr double kGlyphSize = 24;
inline constexpr int kGlyphBuffer = 4;
inline constexpr double kSdfRadius = 8;
inline constexpr int kServerBorder = 3;

struct GlyphMetrics {
  int x = 0, y = 0, width = 0, height = 0;
  double left = 0, top = 0, advance = 0;
};

struct FontSpec {
  std::vector<std::string> families;
  bool bold = false;
  bool italic = false;
  std::string stack;
  std::string key() const;
};

struct ServerGlyph {
  std::vector<uint8_t> bitmap;
  int width = 0, height = 0;
  double left = 0, top = 0, advance = 0;
};

std::unordered_map<uint32_t, ServerGlyph> decodeGlyphs(const Bytes& bytes);
std::string glyphUrl(const std::string& tmpl, const std::string& stack, uint32_t start);
bool fromServer(uint32_t codepoint);

class LocalFonts {
 public:
  static LocalFonts& get();
  std::optional<std::vector<uint8_t>> sdf(const FontSpec& font, uint32_t codepoint, double size, int& width, int& height, double& left,
                                          double& top, double& advance);

 private:
  struct Face;
  Face* face(bool bold, bool italic);
  std::mutex mutex_;
  std::map<int, std::unique_ptr<Face>> faces_;
};

class GlyphAtlas {
 public:
  GlyphAtlas();
  int width = 1024;
  int height = 512;
  std::vector<uint8_t> data;
  uint64_t generation = 0;
  bool dirty = true;
  const GlyphMetrics* get(const FontSpec& font, uint32_t cp) const;
  std::optional<GlyphMetrics> rasterize(const FontSpec& font, uint32_t cp);
  std::optional<GlyphMetrics> server(const std::string& stack, uint32_t cp, const ServerGlyph& glyph);
  void reset();

 private:
  struct Shelf {
    int y, height, x;
  };
  std::optional<std::pair<int, int>> allocate(int w, int h);
  std::unordered_map<std::string, std::optional<GlyphMetrics>> glyphs_;
  std::vector<Shelf> shelves_;
  int nextY_ = 0;
  int maxHeight_ = 8192;
};

class GlyphServer {
 public:
  GlyphServer(std::string tmpl, std::shared_ptr<ResponseStore> store, Mailbox* mailbox, std::function<void()> arrived);
  ~GlyphServer();
  const std::string tmpl;
  const ServerGlyph* glyph(const std::string& stack, uint32_t cp, bool& pending);
  bool loading() const { return waiting_ > 0; }

 private:
  void fetch(const std::string& stack, uint32_t start, const std::string& key);
  std::shared_ptr<ResponseStore> store_;
  Mailbox* mailbox_;
  std::function<void()> arrived_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  enum class State { Loading, Missing, Ready };
  struct Range {
    State state = State::Loading;
    std::unordered_map<uint32_t, ServerGlyph> glyphs;
  };
  std::unordered_map<std::string, Range> ranges_;
  int waiting_ = 0;
  CancelToken cancel_;
};

}
