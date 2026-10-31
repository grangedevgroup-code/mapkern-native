#include <fstream>
#include <sstream>

#include "check.hpp"
#include "core/url.hpp"
#include "core/value.hpp"
#include "style/import.hpp"
#include "style/layer.hpp"
#include "style/validate.hpp"

using namespace mapkern;

static Value J(const char* text) { return parseJson(text).value_or(Value()); }

static std::optional<Value> readJson(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::stringstream ss;
  ss << in.rdbuf();
  return parseJson(ss.str());
}

static void describeDiff(const Value& a, const Value& b, const std::string& path, int& budget) {
  if (budget <= 0 || a == b) return;
  if (a.isObject() && b.isObject()) {
    for (const auto& [k, v] : a.asObject()) describeDiff(v, b[k], path + "." + k, budget);
    for (const auto& [k, v] : b.asObject())
      if (!a.has(k)) describeDiff(Value(), v, path + "." + k, budget);
    return;
  }
  if (a.isArray() && b.isArray() && a.size() == b.size()) {
    for (size_t i = 0; i < a.size(); i++) describeDiff(a[i], b[i], path + "[" + std::to_string(i) + "]", budget);
    return;
  }
  budget--;
  std::printf("    %s: native %s, js %s\n", path.c_str(), a.toJson().substr(0, 200).c_str(), b.toJson().substr(0, 200).c_str());
}

TEST("style: layer defaults and validation") {
  StyleLayer line(J(R"({"id":"roads","type":"line","source":"osm","sourceLayer":"transportation","paint":{"width":["interpolate",["linear"],["zoom"],10,1,16,8]},"layout":{"cap":"round"}})"));
  EvalContext ctx;
  ctx.zoom = 13;
  EXPECT_NEAR(line.paintNumber("width", ctx), 4.5, 1e-9);
  EXPECT(line.fixed("cap") == Value("round"));
  EXPECT(line.fixed("join") == Value("miter"));
  const Color c = line.paintColor("color", ctx);
  EXPECT(c[0] == 0 && c[3] == 1);
  bool threw = false;
  try {
    StyleLayer bad(J(R"({"id":"x","type":"line","source":"s","layout":{"cap":"pointy"}})"));
  } catch (const StyleError& e) {
    threw = std::string(e.what()).find("x.layout.cap must be one of butt, round, square") != std::string::npos;
  }
  EXPECT(threw);
}

TEST("style: paint transitions blend toward the new value") {
  StyleLayer bg(J(R"({"id":"bg","type":"background","paint":{"opacity":0}})"));
  bg.setPaint("opacity", 1, J(R"({"duration":1000})"));
  EXPECT(bg.animating());
  const double start = nowMs();
  bg.settle(start + 500);
  EvalContext ctx;
  const double mid = bg.paintNumber("opacity", ctx);
  EXPECT(mid > 0.2 && mid < 0.8);
  bg.settle(start + 5000);
  EXPECT_NEAR(bg.paintNumber("opacity", ctx), 1, 1e-9);
  EXPECT(!bg.animating());
}

TEST("style: legacy filters convert to expressions") {
  EXPECT(convertFilter(J(R"(["in","class","a","b"])")) == J(R"(["match",["get","class"],["a","b"],true,false])"));
  EXPECT(convertFilter(J(R"(["==","$type","Polygon"])")) == J(R"(["==",["geometry-type"],"Polygon"])"));
  EXPECT(convertFilter(J(R"(["!has","name"])")) == J(R"(["!",["has","name"]])"));
  EXPECT(tokens("{name} ({ref})") == J(R"j(["concat",["to-string",["get","name"]]," (",["to-string",["get","ref"]],")"])j"));
}

TEST("style: font names split into families, weight and slant") {
  const auto f = fontFromNames({"Noto Sans Bold Italic", "Open Sans Regular"});
  EXPECT(f.families.size() == 3 && f.families[0] == "Noto Sans" && f.families[1] == "Open Sans");
  EXPECT(f.bold && f.italic);
}

TEST("style: urls resolve against a base") {
  EXPECT(resolveUrl("../fonts/{fontstack}/{range}.pbf", "https://a.org/styles/liberty/style.json") ==
         "https://a.org/styles/fonts/{fontstack}/{range}.pbf");
  EXPECT(resolveUrl("/x.json", "https://a.org/b/c") == "https://a.org/x.json");
  EXPECT(resolveUrl("//cdn.org/y", "https://a.org/b") == "https://cdn.org/y");
  EXPECT(resolveUrl("https://z.org/q", "https://a.org/b") == "https://z.org/q");
}

TEST("style: OpenFreeMap styles import the same as GL JS") {
  for (const char* name : {"liberty", "bright"}) {
    const auto input = readJson(std::string("../data/") + name + ".json");
    const auto reference = readJson(std::string("../data/") + name + ".imported.json");
    if (!input || !reference) {
      std::printf("    skipped: data/%s.json not present\n", name);
      continue;
    }
    const auto out = importStyle(*input, std::string("https://tiles.openfreemap.org/styles/") + name);
    EXPECT(out.skipped.size() == (*reference)["skipped"].size());
    for (const auto& s : out.skipped) std::printf("    skipped: %s\n", s.c_str());
    EXPECT(out.style["layers"].size() == (*reference)["style"]["layers"].size());
    int budget = 8;
    describeDiff(out.style, (*reference)["style"], name, budget);
    EXPECT(budget == 8);
    EXPECT(validateStyle(out.style).empty());
    for (const auto& layer : out.style["layers"].asArray()) StyleLayer check(layer);
  }
}
