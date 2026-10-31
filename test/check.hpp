#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace check {

struct Case {
  const char* name;
  std::function<void()> run;
};

inline std::vector<Case>& cases() {
  static std::vector<Case> all;
  return all;
}

inline int& failures() {
  static int count = 0;
  return count;
}

struct Register {
  Register(const char* name, std::function<void()> run) { cases().push_back({name, std::move(run)}); }
};

inline void fail(const char* file, int line, const std::string& what) {
  std::printf("  FAIL %s:%d %s\n", file, line, what.c_str());
  failures()++;
}

}

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST(name)                                                                  \
  static void CHECK_CAT(test_, __LINE__)();                                         \
  static check::Register CHECK_CAT(reg_, __LINE__)(name, CHECK_CAT(test_, __LINE__)); \
  static void CHECK_CAT(test_, __LINE__)()

#define EXPECT(cond) \
  do {               \
    if (!(cond)) check::fail(__FILE__, __LINE__, #cond); \
  } while (0)

#define EXPECT_NEAR(a, b, eps)                                                                    \
  do {                                                                                            \
    const double va_ = (a), vb_ = (b);                                                            \
    if (!(std::abs(va_ - vb_) <= (eps)))                                                          \
      check::fail(__FILE__, __LINE__, std::string(#a " = ") + std::to_string(va_) + ", expected " + std::to_string(vb_)); \
  } while (0)
