// Exported C++ tier: the logic cases' generated sources (kern_tests --emit-logic) are
// compiled into this binary; every graph must run as native code and give exactly
// the interpreter's results (logic_expected.txt).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "logic_cases.h"

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: logic_native_test <logic_expected.txt>\n"); return 2; }
  std::ifstream in(argv[1]);
  std::stringstream want;
  want << in.rdbuf();
  std::string got;
  int failures = 0;
  for (const logic_cases::Case& c : logic_cases::cases()) {
    logic_cases::Host host;
    kern::logic::Runtime rt(host);
    kern::logic::Instance* inst = rt.add(logic_cases::parse(c), 0, c.name);
    if (std::strcmp(inst->tier(), "native C++") != 0) {
      std::printf("FAIL %s runs in the %s, not as native C++\n", c.name, inst->tier());
      ++failures;
    }
    got += std::string("== ") + c.name + "\n" + logic_cases::play(rt, host, inst);
  }
  if (got != want.str()) {
    std::printf("FAIL native results differ from the interpreter\n--- interpreter\n%s--- native\n%s", want.str().c_str(), got.c_str());
    ++failures;
  }
  std::printf("%s logic_native_test (%zu graphs as generated C++)\n", failures ? "FAIL" : "ok  ", logic_cases::cases().size());
  return failures ? 1 : 0;
}
