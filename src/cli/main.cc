#define EXTERN
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "gc/gc.h"
#include "ifa_bridge/bridge.h"
#include "parse/parse.h"

namespace {

struct SmokeCase {
  std::string_view expr;
  int64_t expected;
};

int run_smoke_test() {
  constexpr std::string_view kGrammar = R"(
    expr: 'let' "[a-zA-Z_][a-zA-Z0-9_]*" '=' expr 'in' expr $right 0
        | expr '+' expr $binary_op_left 1
        | expr '-' expr $binary_op_left 1
        | expr '*' expr $binary_op_left 2
        | expr '/' expr $binary_op_left 2
        | '(' expr ')'
        | "[0-9]+"
        | "[a-zA-Z_][a-zA-Z0-9_]*";
  )";

  langwb::parse::DynamicGrammar g;
  if (!g.compile(kGrammar)) {
    std::fprintf(stderr, "smoke: failed to compile in-memory DParser grammar\n");
    return 1;
  }

  if (g.parse_check("1 + * 3")) {
    std::fprintf(stderr, "smoke: unexpectedly accepted invalid input\n");
    return 1;
  }

  constexpr SmokeCase kCases[] = {
      {"1 + 2 * 3", 7},
      {"(1 + 2) * 3", 9},
      {"let x = 10 in let y = x * 4 + 2 in y - 5", 37},
  };

  for (const auto &tc : kCases) {
    langwb::parse::CstNode *cst = g.parse(tc.expr);
    if (!cst) {
      std::fprintf(stderr, "smoke: failed to parse '%.*s'\n", static_cast<int>(tc.expr.size()),
                   tc.expr.data());
      return 1;
    }
    int64_t got = 0;
    if (!langwb::ifa_bridge::compile_and_jit_i64(cst, &got)) {
      std::fprintf(stderr, "smoke: failed to compile/JIT '%.*s'\n",
                   static_cast<int>(tc.expr.size()), tc.expr.data());
      return 1;
    }
    if (got != tc.expected) {
      std::fprintf(stderr, "smoke: '%.*s' => %lld, expected %lld\n",
                   static_cast<int>(tc.expr.size()), tc.expr.data(),
                   static_cast<long long>(got), static_cast<long long>(tc.expected));
      return 1;
    }
  }

  std::printf("langwb smoke test passed (in-memory DParser -> CST -> IF1 -> IFA -> LLVM LLJIT x3)\n");
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
