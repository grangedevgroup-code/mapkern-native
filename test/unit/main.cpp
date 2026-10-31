#include <cstdio>
#include <cstring>

#include "check.hpp"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  for (const auto& c : check::cases()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    const int before = check::failures();
    c.run();
    run++;
    std::printf("%s %s\n", check::failures() == before ? "ok  " : "FAIL", c.name);
  }
  std::printf("%d tests, %d failures\n", run, check::failures());
  return check::failures() == 0 ? 0 : 1;
}
