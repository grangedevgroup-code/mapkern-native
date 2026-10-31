#include "text/glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "core/url.hpp"
#include "core/utf8.hpp"
#include "formats/protobuf.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

namespace mapkern {

namespace {

constexpr double kServerAscender = 24.2;

bool isGenericFamily(const std::string& name) {
  static const char* generic[] = {"serif", "sans-serif", "monospace", "cursive", "fantasy", "system-ui", "ui-serif", "ui-sans-serif",
                                  "ui-monospace", "ui-rounded", "emoji", "math", "fangsong"};
  std::string lower = name;
  for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char* g : generic)
    if (lower == g) return true;
  return false;
}

bool genericStack(const std::string& stack) {
  bool any = false;
  size_t start = 0;
  while (start <= stack.size()) {
    size_t comma = stack.find(',', start);
    if (comma == std::string::npos) comma = stack.size();
    std::string name = stack.substr(start, comma - start);
    name.erase(0, name.find_first_not_of(' '));
    name.erase(name.find_last_not_of(' ') + 1);
    if (!name.empty()) {
      if (!isGenericFamily(name)) return false;
      any = true;
    }
    start = comma + 1;
  }
  return any;
}

std::vector<std::string> fontCandidates(bool bold, bool italic) {
  std::vector<std::string> out;
#ifdef _WIN32
  const char* windir = std::getenv("WINDIR");
  const std::string fonts = std::string(windir ? windir : "C:/Windows") + "/Fonts/";
  if (bold && italic) out.push_back(fonts + "segoeuiz.ttf");
  if (bold) out.push_back(fonts + "segoeuib.ttf");
  if (italic) out.push_back(fonts + "segoeuii.ttf");
  out.push_back(fonts + "segoeui.ttf");
  out.push_back(fonts + "arial.ttf");
#elif defined(__ANDROID__)
  if (bold) out.push_back("/system/fonts/Roboto-Bold.ttf");
  if (italic) out.push_back("/system/fonts/Roboto-Italic.ttf");
  out.push_back("/system/fonts/Roboto-Regular.ttf");
  out.push_back("/system/fonts/NotoSans-Regular.ttf");
  out.push_back("/system/fonts/DroidSans.ttf");
#elif defined(__APPLE__)
  out.push_back("/System/Library/Fonts/Helvetica.ttc");
  out.push_back("/System/Library/Fonts/HelveticaNeue.ttc");
  out.push_back("/System/Library/Fonts/Core/Helvetica.ttc");
  out.push_back("/System/Library/Fonts/Supplemental/Arial.ttf");
#else
  if (bold) {
    out.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf");
    out.push_back("/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf");
  }
  out.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
  out.push_back("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf");
  out.push_back("/usr/share/fonts/TTF/DejaVuSans.ttf");
  out.push_back("/usr/share/fonts/dejavu/DejaVuSans.ttf");
  out.push_back("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
#endif
  return out;
}

}

std::string FontSpec::key() const {
  std::string k = std::string(italic ? "i" : "n") + (bold ? "700" : "400") + "|";
  for (const auto& f : families) k += f + ",";
  return k;
}

struct LocalFonts::Face {
  std::vector<uint8_t> data;
  stbtt_fontinfo info{};
  bool ok = false;
};

LocalFonts& LocalFonts::get() {
  static LocalFonts fonts;
  return fonts;
}

LocalFonts::Face* LocalFonts::face(bool bold, bool italic) {
  const int key = (bold ? 1 : 0) | (italic ? 2 : 0);
  auto& slot = faces_[key];
  if (slot) return slot->ok ? slot.get() : nullptr;
  slot = std::make_unique<Face>();
  for (const auto& path : fontCandidates(bold, italic)) {
    auto bytes = readFile(path);
    if (!bytes) continue;
    slot->data = std::move(*bytes);
    const int offset = stbtt_GetFontOffsetForIndex(slot->data.data(), 0);
    if (offset >= 0 && stbtt_InitFont(&slot->info, slot->data.data(), offset)) {
      slot->ok = true;
      break;
    }
  }
  return slot->ok ? slot.get() : nullptr;
}

bool LocalFonts::bitmap(bool bold, uint32_t codepoint, double pixels, std::vector<uint8_t>& out, int& width, int& height, int& left, int& top,
                        double& advance, double& kernNext, uint32_t next) {
  std::lock_guard lock(mutex_);
  Face* f = face(bold, false);
  if (!f) f = face(false, false);
  if (!f) return false;
  if (stbtt_FindGlyphIndex(&f->info, static_cast<int>(codepoint)) == 0 && codepoint > 0x20)
    if (Face* other = fallback(codepoint)) f = other;
  const float scale = stbtt_ScaleForMappingEmToPixels(&f->info, static_cast<float>(pixels));
  int adv = 0, lsb = 0;
  stbtt_GetCodepointHMetrics(&f->info, static_cast<int>(codepoint), &adv, &lsb);
  advance = static_cast<double>(adv) * scale;
  kernNext = next ? static_cast<double>(stbtt_GetCodepointKernAdvance(&f->info, static_cast<int>(codepoint), static_cast<int>(next))) * scale : 0;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  stbtt_GetCodepointBitmapBox(&f->info, static_cast<int>(codepoint), scale, scale, &x0, &y0, &x1, &y1);
  width = std::max(0, x1 - x0);
  height = std::max(0, y1 - y0);
  left = x0;
  top = y0;
  out.assign(static_cast<size_t>(width) * height, 0);
  if (width > 0 && height > 0) stbtt_MakeCodepointBitmap(&f->info, out.data(), width, height, width, scale, scale, static_cast<int>(codepoint));
  return true;
}

double LocalFonts::ascent(bool bold, double pixels) {
  std::lock_guard lock(mutex_);
  Face* f = face(bold, false);
  if (!f) f = face(false, false);
  if (!f) return pixels * 0.8;
  int a = 0, d = 0, g = 0;
  stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
  return static_cast<double>(a) * stbtt_ScaleForMappingEmToPixels(&f->info, static_cast<float>(pixels));
}

std::optional<std::vector<uint8_t>> LocalFonts::sdf(const FontSpec& font, uint32_t codepoint, double size, int& width, int& height,
                                                    double& left, double& top, double& advance) {
  std::lock_guard lock(mutex_);
  Face* f = face(font.bold, font.italic);
  if (!f) f = face(false, false);
  if (!f) return std::nullopt;
  if (stbtt_FindGlyphIndex(&f->info, static_cast<int>(codepoint)) == 0 && codepoint > 0x20) {
    f = fallback(codepoint);
    if (!f) {
      width = height = 0;
      left = top = advance = 0;
      return std::vector<uint8_t>{};
    }
  }
  const float scale = stbtt_ScaleForMappingEmToPixels(&f->info, static_cast<float>(size));
  int adv = 0, lsb = 0;
  stbtt_GetCodepointHMetrics(&f->info, static_cast<int>(codepoint), &adv, &lsb);
  advance = static_cast<double>(adv) * scale;
  int w = 0, h = 0, xoff = 0, yoff = 0;
  unsigned char* bitmap =
      stbtt_GetCodepointSDF(&f->info, scale, static_cast<int>(codepoint), kGlyphBuffer, 191, static_cast<float>(255.0 / kSdfRadius / 1.0), &w, &h, &xoff, &yoff);
  if (!bitmap) {
    width = height = 0;
    left = top = 0;
    return std::vector<uint8_t>{};
  }
  std::vector<uint8_t> out(bitmap, bitmap + static_cast<size_t>(w) * h);
  stbtt_FreeSDF(bitmap, nullptr);
  width = w;
  height = h;
  left = xoff;
  int ascent = 0, descent = 0, gap = 0;
  stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &gap);
  top = yoff;
  (void)ascent;
  return out;
}

LocalFonts::Face* LocalFonts::fallback(uint32_t codepoint) {
  if (!fallbacksLoaded_) {
    fallbacksLoaded_ = true;
    std::vector<std::string> paths;
#ifdef _WIN32
    const char* windir = std::getenv("WINDIR");
    const std::string d = std::string(windir ? windir : "C:/Windows") + "/Fonts/";
    for (const char* n : {"seguisym.ttf", "gadugi.ttf", "Nirmala.ttf", "msyh.ttc", "YuGothR.ttc", "malgun.ttf", "ebrima.ttf", "taile.ttf",
                          "himalaya.ttf", "mmrtext.ttf", "msuighur.ttf", "seguihis.ttf", "arialuni.ttf", "tahoma.ttf"})
      paths.push_back(d + n);
#elif defined(__ANDROID__)
    for (const char* n : {"NotoSansCJK-Regular.ttc", "NotoSansSymbols-Regular-Subsetted.ttf", "NotoSansCanadianAboriginal-Regular.ttf",
                          "NotoSansDevanagari-Regular.otf", "NotoSansThai-Regular.ttf", "NotoNaskhArabic-Regular.ttf", "NotoSansHebrew-Regular.ttf",
                          "NotoSansEthiopic-Regular.ttf", "NotoSansArmenian-Regular.otf", "NotoSansGeorgian-Regular.otf", "DroidSansFallback.ttf"})
      paths.push_back(std::string("/system/fonts/") + n);
#elif defined(__APPLE__)
    for (const char* n : {"/System/Library/Fonts/PingFang.ttc", "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
                          "/System/Library/Fonts/AppleSDGothicNeo.ttc", "/System/Library/Fonts/Supplemental/EuphemiaCAS.ttc"})
      paths.push_back(n);
#else
    for (const char* n : {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/noto/NotoSansCanadianAboriginal-Regular.ttf",
                          "/usr/share/fonts/truetype/noto/NotoSansDevanagari-Regular.ttf", "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
                          "/usr/share/fonts/truetype/unifont/unifont.ttf"})
      paths.push_back(n);
#endif
    for (const auto& path : paths) {
      auto bytes = readFile(path);
      if (!bytes) continue;
      auto f = std::make_unique<Face>();
      f->data = std::move(*bytes);
      const int offset = stbtt_GetFontOffsetForIndex(f->data.data(), 0);
      if (offset >= 0 && stbtt_InitFont(&f->info, f->data.data(), offset)) {
        f->ok = true;
        fallbacks_.push_back(std::move(f));
      }
    }
  }
  for (auto& f : fallbacks_)
    if (stbtt_FindGlyphIndex(&f->info, static_cast<int>(codepoint)) != 0) return f.get();
  return nullptr;
}

std::unordered_map<uint32_t, ServerGlyph> decodeGlyphs(const Bytes& bytes) {
  std::unordered_map<uint32_t, ServerGlyph> out;
  pbf::Reader top(bytes.data(), bytes.size());
  while (!top.done()) {
    const uint32_t tag = top.tag();
    if ((tag >> 3) != 1) {
      top.skip(static_cast<int>(tag & 7));
      continue;
    }
    pbf::Reader stack = top.sub();
    while (!stack.done()) {
      const uint32_t t = stack.tag();
      if ((t >> 3) != 3) {
        stack.skip(static_cast<int>(t & 7));
        continue;
      }
      pbf::Reader g = stack.sub();
      int64_t id = -1;
      ServerGlyph glyph;
      std::string_view bitmap;
      while (!g.done()) {
        const uint32_t gt = g.tag();
        switch (gt >> 3) {
          case 1: id = g.varint(); break;
          case 2: bitmap = g.bytes(); break;
          case 3: glyph.width = static_cast<int>(g.varint()); break;
          case 4: glyph.height = static_cast<int>(g.varint()); break;
          case 5: glyph.left = g.svarint(); break;
          case 6: glyph.top = g.svarint(); break;
          case 7: glyph.advance = g.varint(); break;
          default: g.skip(static_cast<int>(gt & 7));
        }
      }
      if (id < 0) continue;
      const size_t need = static_cast<size_t>(glyph.width + kServerBorder * 2) * (glyph.height + kServerBorder * 2);
      if (glyph.width > 0 && glyph.height > 0 && bitmap.size() >= need) glyph.bitmap.assign(bitmap.begin(), bitmap.begin() + static_cast<ptrdiff_t>(need));
      glyph.top += kServerAscender;
      out[static_cast<uint32_t>(id)] = std::move(glyph);
    }
  }
  return out;
}

std::string glyphUrl(const std::string& tmpl, const std::string& stack, uint32_t start) {
  std::string encoded;
  size_t s = 0;
  bool first = true;
  while (s <= stack.size()) {
    size_t comma = stack.find(',', s);
    if (comma == std::string::npos) comma = stack.size();
    std::string name = stack.substr(s, comma - s);
    name.erase(0, name.find_first_not_of(' '));
    name.erase(name.find_last_not_of(' ') + 1);
    if (!first) encoded += ',';
    encoded += percentEncode(name);
    first = false;
    s = comma + 1;
  }
  std::string url = replaceAll(tmpl, "{fontstack}", encoded);
  return replaceAll(url, "{range}", std::to_string(start) + "-" + std::to_string(start + 255));
}

bool fromServer(uint32_t cp) {
  if (cp > 0xffff) return false;
  const uint32_t local[][2] = {{0x1100, 0x11ff}, {0x2e80, 0x2fdf}, {0x2ff0, 0x303f}, {0x3040, 0x30ff}, {0x3100, 0x318f}, {0x31a0, 0x31ff},
                               {0x3200, 0x9fff}, {0xa960, 0xa97f}, {0xac00, 0xd7ff}, {0xf900, 0xfaff}, {0xfe30, 0xfe4f}, {0xff00, 0xffef}};
  for (const auto& r : local)
    if (cp >= r[0] && cp <= r[1]) return false;
  return true;
}

GlyphAtlas::GlyphAtlas() { data.assign(static_cast<size_t>(width) * height, 0); }

void GlyphAtlas::reset() {
  glyphs_.clear();
  shelves_.clear();
  nextY_ = 0;
  width = 1024;
  height = 512;
  data.assign(static_cast<size_t>(width) * height, 0);
  dirty = true;
  generation++;
}

const GlyphMetrics* GlyphAtlas::get(const FontSpec& font, uint32_t cp) const {
  auto it = glyphs_.find(font.key() + "|" + std::to_string(cp));
  if (it == glyphs_.end() || !it->second) return nullptr;
  return &*it->second;
}

std::optional<std::pair<int, int>> GlyphAtlas::allocate(int w, int h) {
  if (w > width) return std::nullopt;
  for (auto& shelf : shelves_) {
    if (h <= shelf.height && h >= shelf.height * 0.6 && shelf.x + w <= width) {
      const auto spot = std::make_pair(shelf.x, shelf.y);
      shelf.x += w + 1;
      return spot;
    }
  }
  const int shelfHeight = (h + 3) / 4 * 4;
  while (nextY_ + shelfHeight > height) {
    if (height * 2 > maxHeight_) return std::nullopt;
    data.resize(static_cast<size_t>(width) * height * 2, 0);
    height *= 2;
    dirty = true;
  }
  shelves_.push_back({nextY_, shelfHeight, w + 1});
  const auto spot = std::make_pair(0, nextY_);
  nextY_ += shelfHeight + 1;
  return spot;
}

std::optional<GlyphMetrics> GlyphAtlas::rasterize(const FontSpec& font, uint32_t cp) {
  const std::string key = font.key() + "|" + std::to_string(cp);
  if (auto it = glyphs_.find(key); it != glyphs_.end()) return it->second;
  int w = 0, h = 0;
  double left = 0, top = 0, advance = 0;
  auto sdf = LocalFonts::get().sdf(font, cp, kGlyphSize, w, h, left, top, advance);
  if (!sdf) {
    glyphs_[key] = std::nullopt;
    return std::nullopt;
  }
  GlyphMetrics m;
  m.advance = advance;
  if (w > 0 && h > 0) {
    const auto spot = allocate(w, h);
    if (!spot) return std::nullopt;
    for (int y = 0; y < h; y++) std::memcpy(&data[static_cast<size_t>(spot->second + y) * width + spot->first], &(*sdf)[static_cast<size_t>(y) * w], w);
    dirty = true;
    m.x = spot->first;
    m.y = spot->second;
    m.width = w;
    m.height = h;
    m.left = left;
    m.top = top;
  }
  glyphs_[key] = m;
  return m;
}

std::optional<GlyphMetrics> GlyphAtlas::server(const std::string& stack, uint32_t cp, const ServerGlyph& glyph) {
  const std::string key = "s|" + stack + "|" + std::to_string(cp);
  if (auto it = glyphs_.find(key); it != glyphs_.end()) return it->second;
  GlyphMetrics m;
  m.advance = glyph.advance;
  if (glyph.bitmap.empty()) {
    glyphs_[key] = m;
    return m;
  }
  const int bw = glyph.width + kServerBorder * 2, bh = glyph.height + kServerBorder * 2;
  const auto spot = allocate(bw, bh);
  if (!spot) return std::nullopt;
  for (int y = 0; y < bh; y++)
    std::memcpy(&data[static_cast<size_t>(spot->second + y) * width + spot->first], &glyph.bitmap[static_cast<size_t>(y) * bw], bw);
  dirty = true;
  m.x = spot->first;
  m.y = spot->second;
  m.width = bw;
  m.height = bh;
  m.left = glyph.left - kServerBorder;
  m.top = -glyph.top - kServerBorder;
  glyphs_[key] = m;
  return m;
}

GlyphServer::GlyphServer(std::string t, std::shared_ptr<ResponseStore> store, Mailbox* mailbox, std::function<void()> arrived)
    : tmpl(std::move(t)), store_(std::move(store)), mailbox_(mailbox), arrived_(std::move(arrived)) {}

GlyphServer::~GlyphServer() {
  *alive_ = false;
  cancel_.cancel();
}

const ServerGlyph* GlyphServer::glyph(const std::string& stack, uint32_t cp, bool& pending) {
  pending = false;
  if (genericStack(stack)) return nullptr;
  const uint32_t start = cp / 256 * 256;
  const std::string key = stack + "|" + std::to_string(start);
  auto it = ranges_.find(key);
  if (it == ranges_.end()) {
    fetch(stack, start, key);
    pending = true;
    return nullptr;
  }
  if (it->second.state == State::Loading) {
    pending = true;
    return nullptr;
  }
  if (it->second.state == State::Missing) return nullptr;
  auto g = it->second.glyphs.find(cp);
  return g == it->second.glyphs.end() ? nullptr : &g->second;
}

void GlyphServer::fetch(const std::string& stack, uint32_t start, const std::string& key) {
  ranges_[key] = Range{};
  waiting_++;
  auto alive = alive_;
  Mailbox* box = mailbox_;
  fetchBytes({glyphUrl(tmpl, stack, start), {}}, cancel_, 30000, ResourceKind::Glyphs, store_, [this, alive, box, key](FetchOutcome out) {
    std::unordered_map<uint32_t, ServerGlyph> glyphs;
    bool ok = false;
    if (auto* r = std::get_if<FetchResult>(&out); r && r->data) {
      try {
        glyphs = decodeGlyphs(*r->data);
        ok = true;
      } catch (...) {
      }
    }
    box->post([this, alive, key, ok, glyphs = std::move(glyphs)]() mutable {
      if (!*alive) return;
      auto& range = ranges_[key];
      range.state = ok ? State::Ready : State::Missing;
      range.glyphs = std::move(glyphs);
      waiting_--;
      if (arrived_) arrived_();
    });
  });
}

}
