#include <cstdio>
#include <string_view>

#include "gc/gc.h"
#include "ifa_bridge/bridge.h"
#include "parse/parse.h"

namespace {

int run_smoke_test() {
  constexpr std::string_view kGrammar = R"(
    expr: expr '+' expr $binary_op_left 1
        | expr '*' expr $binary_op_left 2
        | "[0-9]+";
  )";

  langwb::parse::DynamicGrammar g;
  if (!g.compile(kGrammar)) {
    std::fprintf(stderr, "smoke: failed to compile in-memory DParser grammar\n");
    return 1;
  }
  if (!g.parse_check("1 + 2 * 3")) {
    std::fprintf(stderr, "smoke: failed to parse valid input\n");
    return 1;
  }
  if (g.parse_check("1 + * 3")) {
    std::fprintf(stderr, "smoke: unexpectedly accepted invalid input\n");
    return 1;
  }
  if (!langwb::ifa_bridge::smoke_test_ifa()) {
    std::fprintf(stderr, "smoke: failed IFA init/reset\n");
    return 1;
  }

  std::printf("langwb smoke test passed (in-memory DParser + IFA init/reset)\n");
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  GC_INIT();
  if (argc > 1 && std::string_view(argv[1]) == "--smoke") {
    return run_smoke_test();
  }
  std::printf("langwb — programming language workbench\n");
  std::printf("usage: langwb --smoke\n");
  return 0;
}
