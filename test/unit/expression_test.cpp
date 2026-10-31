#include <cmath>
#include <numbers>

#include "check.hpp"
#include "core/value.hpp"
#include "style/expression.hpp"

using namespace mapkern;

static Value J(const char* text) {
  std::string error;
  auto v = parseJson(text, &error);
  if (!v) std::printf("bad json in test: %s (%s)\n", text, error.c_str());
  return v.value_or(Value());
}

struct Ctx {
  Object props;
  FeatureContext feature;
  Object state;
  EvalContext eval;
  Ctx(const char* properties, double zoom = 10) {
    props = J(properties).asObject();
    feature.properties = &props;
    feature.id = 7;
    feature.geometryType = GeometryType::Point;
    eval.zoom = zoom;
    eval.feature = &feature;
  }
};

static Value run(const char* expression, const EvalContext& ctx) {
  return compile(J(expression), OutputKind::Any, Value(), "test", {true, false}).evaluate(ctx);
}

static Value run(const char* expression) {
  Ctx c("{}");
  return run(expression, c.eval);
}

TEST("expression: feature data and lookups") {
  Ctx c(R"({"name":"Rainier","height":4392,"tags":["peak","volcano"]})");
  EXPECT(run(R"(["get","name"])", c.eval) == Value("Rainier"));
  EXPECT(run(R"(["has","height"])", c.eval) == Value(true));
  EXPECT(run(R"(["has","missing"])", c.eval) == Value(false));
  EXPECT(run(R"(["id"])", c.eval) == Value(7));
  EXPECT(run(R"(["geometry-type"])", c.eval) == Value("Point"));
  EXPECT(run(R"(["at",1,["get","tags"]])", c.eval) == Value("volcano"));
  EXPECT(run(R"(["at",5,["get","tags"]])", c.eval).isNull());
  EXPECT(run(R"(["index-of","volcano",["get","tags"]])", c.eval) == Value(1));
  EXPECT(run(R"(["index-of","ni",["get","name"]])", c.eval) == Value(3));
  EXPECT(run(R"(["split","a,b,c",","])") == J(R"(["a","b","c"])"));
  EXPECT(run(R"(["length",["get","tags"]])", c.eval) == Value(2));
}

TEST("expression: decisions, matching and steps") {
  Ctx c(R"({"kind":"trail","width":3})");
  EXPECT(run(R"(["match",["get","kind"],["road","street"],1,"trail",2,0])", c.eval) == Value(2));
  EXPECT(run(R"(["case",[">",["get","width"],5],"wide","narrow"])", c.eval) == Value("narrow"));
  EXPECT(run(R"(["coalesce",["get","missing"],"fallback"])", c.eval) == Value("fallback"));
  EXPECT(run(R"(["step",["zoom"],"low",8,"mid",12,"high"])", c.eval) == Value("mid"));
  EXPECT(run(R"(["all",["==",["get","kind"],"trail"],["<",["get","width"],4]])", c.eval) == Value(true));
  EXPECT(run(R"(["any",["==",["get","kind"],"road"],["!",true]])", c.eval) == Value(false));
  EXPECT(run(R"(["in","b",["literal",["a","b"]]])", c.eval) == Value(true));
}

TEST("expression: variables keep a value for reuse") {
  Ctx c(R"({"a":3,"b":4})");
  EXPECT(run(R"(["let","x",["*",["get","a"],2],"y",1,["+",["var","x"],["var","y"]]])", c.eval) == Value(7));
  bool threw = false;
  try {
    compile(J(R"(["var","nope"])"), OutputKind::Any, Value());
  } catch (const ExpressionError&) {
    threw = true;
  }
  EXPECT(threw);
}

TEST("expression: type assertions") {
  Ctx c(R"({"n":"12","m":5,"list":[1,2,3]})");
  EXPECT(run(R"(["number",["get","n"],["get","m"]])", c.eval) == Value(5));
  EXPECT(run(R"(["string",["get","m"],"x"])", c.eval) == Value("x"));
  EXPECT(run(R"(["boolean",["get","m"]])", c.eval).isNull());
  EXPECT(run(R"(["array","number",3,["get","list"]])", c.eval) == J("[1,2,3]"));
  EXPECT(run(R"(["array","string",["get","list"]])", c.eval).isNull());
  EXPECT(run(R"(["object",["literal",{"a":1}]])") == J(R"({"a":1})"));
}

TEST("expression: math") {
  EXPECT_NEAR(run(R"(["+",1,2,3])").asNumber(), 6, 1e-9);
  EXPECT_NEAR(run(R"(["-",10,4])").asNumber(), 6, 1e-9);
  EXPECT_NEAR(run(R"(["%",10,4])").asNumber(), 2, 1e-9);
  EXPECT_NEAR(run(R"(["^",2,10])").asNumber(), 1024, 1e-9);
  EXPECT_NEAR(run(R"(["asin",1])").asNumber(), std::numbers::pi / 2, 1e-9);
  EXPECT_NEAR(run(R"(["ln2"])").asNumber(), std::numbers::ln2, 1e-12);
  EXPECT_NEAR(run(R"(["max",3,9,4])").asNumber(), 9, 1e-9);
  EXPECT_NEAR(run(R"(["round",-2.5])").asNumber(), -2, 1e-9);
}

TEST("expression: interpolation along curves") {
  EvalContext z;
  z.zoom = 15;
  EXPECT_NEAR(run(R"(["interpolate",["linear"],["zoom"],10,0,20,100])", z).asNumber(), 50, 1e-9);
  z.zoom = 1;
  EXPECT_NEAR(run(R"(["interpolate",["exponential",2],["zoom"],0,0,2,3])", z).asNumber(), 1, 1e-9);
  z.zoom = 0.5;
  EXPECT_NEAR(run(R"(["interpolate",["cubic-bezier",0.42,0,0.58,1],["zoom"],0,0,1,1])", z).asNumber(), 0.5, 1e-3);
}

TEST("expression: colors in rgb, hsl, lab and hcl") {
  EXPECT(run(R"(["to-rgba",["rgb",255,128,0]])") == J("[255,128,0,1]"));
  const Value red = run(R"(["hsl",0,100,50])");
  EXPECT(red.isColor() && std::abs(red.asColor()[0] - 1) < 1e-9 && std::abs(red.asColor()[1]) < 1e-9);
  const Value hsla = run(R"(["to-hsla","#00ff00"])");
  EXPECT(std::lround(hsla[0].asNumber()) == 120 && std::lround(hsla[1].asNumber()) == 100);
  EvalContext z;
  z.zoom = 0.5;
  const Value lab = run(R"(["interpolate-lab",["linear"],["zoom"],0,"#000000",1,"#ffffff"])", z);
  EXPECT(lab.isColor() && lab.asColor()[0] > 0.4 && lab.asColor()[0] < 0.5);
  z.zoom = 0;
  const Value hcl = compile(J(R"(["interpolate-hcl",["linear"],["zoom"],0,"#ff0000",1,"#0000ff"])"), OutputKind::Color, Value()).evaluate(z);
  EXPECT(hcl.isColor() && std::abs(hcl.asColor()[0] - 1) < 0.01);
}

TEST("expression: number formatting follows the locale") {
  EXPECT(run(R"(["number-format",1234.5678,{"locale":"en-US","max-fraction-digits":1}])") == Value("1,234.6"));
  EXPECT(run(R"(["number-format",3.5,{"locale":"en-US","currency":"USD"}])") == Value("$3.50"));
  EXPECT(run(R"(["number-format",1234.5,{"locale":"de-DE","min-fraction-digits":2}])") == Value("1.234,50"));
}

TEST("expression: feature state only in paint") {
  const Value hover = J(R"(["case",["boolean",["feature-state","hover"],false],"#ff0000","#0000ff"])");
  auto paint = compile(hover, OutputKind::Any, Value(), "paint", {true, false});
  EXPECT(paint.stateDependent);
  EXPECT(paint.featureDependent);
  Ctx c("{}");
  c.state = J(R"({"hover":true})").asObject();
  c.eval.state = &c.state;
  EXPECT(paint.evaluate(c.eval) == Value("#ff0000"));
  c.eval.state = nullptr;
  EXPECT(paint.evaluate(c.eval) == Value("#0000ff"));
  bool threw = false;
  try {
    compile(hover, OutputKind::Any, Value(), "filter", {false, false});
  } catch (const ExpressionError& e) {
    threw = std::string(e.what()).find("only works in paint properties") != std::string::npos;
  }
  EXPECT(threw);
}

TEST("expression: strings") {
  Ctx c(R"({"name":"Mount Rainier"})");
  EXPECT(run(R"(["upcase",["get","name"]])", c.eval) == Value("MOUNT RAINIER"));
  EXPECT(run(R"(["concat",["get","name"]," (",4392," m)"])", c.eval) == Value("Mount Rainier (4392 m)"));
  EXPECT(run(R"(["slice",["get","name"],6])", c.eval) == Value("Rainier"));
  EXPECT(run(R"(["to-string",3])") == Value("3"));
  EXPECT(run(R"(["to-number","12.5"])") == Value(12.5));
  EXPECT(run(R"(["upcase","straße é"])") == Value("STRAßE É"));
}

TEST("expression: errors carry their path") {
  auto throwsWith = [](const char* expr, const char* text, const char* path = "value") {
    try {
      compile(J(expr), OutputKind::Any, Value(), path);
    } catch (const ExpressionError& e) {
      return std::string(e.what()).find(text) != std::string::npos;
    }
    return false;
  };
  EXPECT(throwsWith(R"(["match",["get","a"],1])", "layers[2].paint.color", "layers[2].paint.color"));
  EXPECT(throwsWith(R"(["interpolate",["linear"],["zoom"],5,1,2,0])", "stops must increase"));
  EXPECT(throwsWith(R"(["number-format",1,5])", "options object"));
  EXPECT(throwsWith(R"(["nonsense",1])", "unknown expression \"nonsense\""));
  EvalContext z;
  EXPECT(compile(J(R"(["in","b",["a","b"]])"), OutputKind::Boolean, false).evaluate(z) == Value(true));
  EXPECT(compile(J("[0,1]"), OutputKind::Any, Value()).evaluate(z) == J("[0,1]"));
}

TEST("expression: within") {
  const Value square = J(R"({"type":"Polygon","coordinates":[[[0,0],[10,0],[10,10],[0,10],[0,0]],[[4,4],[6,4],[6,6],[4,6],[4,4]]]})");
  Array within{Value("within"), square};
  auto c = compile(Value(within), OutputKind::Boolean, false, "filter", {false, true});
  auto world = [](double lng, double lat) {
    const double s = std::sin(lat * std::numbers::pi / 180);
    return std::array<double, 2>{(lng + 180) / 360, 0.5 - std::log((1 + s) / (1 - s)) / (4 * std::numbers::pi)};
  };
  auto test = [&](int type, std::vector<std::array<double, 2>> pts) {
    WorldGeometry g;
    g.type = type;
    g.parts.emplace_back();
    for (auto& p : pts) {
      auto w = world(p[0], p[1]);
      g.parts[0].push_back(w[0]);
      g.parts[0].push_back(w[1]);
    }
    Object props;
    FeatureContext f;
    f.properties = &props;
    f.geometry = [&g]() { return &g; };
    EvalContext ctx;
    ctx.feature = &f;
    return c.boolean(ctx);
  };
  EXPECT(test(1, {{2, 2}}));
  EXPECT(!test(1, {{5, 5}}));
  EXPECT(!test(1, {{12, 2}}));
  EXPECT(test(2, {{1, 1}, {3, 8}}));
  EXPECT(!test(2, {{1, 5}, {9, 5}}));
}

TEST("json: round trip and errors") {
  const Value v = J(R"({"a":[1,2.5,"xé",true,null],"b":{"c":-3e2}})");
  EXPECT(v["a"][2] == Value("x\xc3\xa9"));
  EXPECT(v["b"]["c"] == Value(-300));
  EXPECT(*parseJson(v.toJson()) == v);
  EXPECT(!parseJson("{\"a\":}").has_value());
  EXPECT(!parseJson("[1,2").has_value());
}
