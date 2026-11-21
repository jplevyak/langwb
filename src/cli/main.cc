#define EXTERN
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include "gc/gc.h"
#include "ifa_bridge/bridge.h"
#include "workbench/workbench.h"

namespace {

struct SmokeCase {
  std::string_view expr;
  int64_t expected;
  bool staged = false;
  bool expect_const_inferred = false;
};

int run_smoke_test() {
  langwb::Workbench wb;

  auto g1 = wb.compiled_grammar();
  auto g2 = wb.compiled_grammar();
  if (!g1 || g1 != g2 || wb.grammar_cache_size() != 1) {
    std::fprintf(stderr, "smoke: GrammarCache failed to compile/cache extended grammar\n");
    return 1;
  }

  if (g1->parse_check("1 + * 3")) {
    std::fprintf(stderr, "smoke: unexpectedly accepted invalid input\n");
    return 1;
  }

  bool trace = std::getenv("LANGWB_TRACE") != nullptr;

  constexpr SmokeCase kCases[] = {
      // Phase 0: arithmetic + let
      {"1 + 2 * 3", 7, false, true},
      {"(1 + 2) * 3", 9, false, true},
      {"let x = 10 in let y = x * 4 + 2 in y - 5", 37, false, true},
      // Phase 1: custom primitive + transfer function + inline LLVM emitter
      {"clamp(15, 0, 10)", 10, false, true},
      {"clamp(0 - 5, 0, 10)", 0, false, true},
      {"let x = 7 in clamp(x * 2, 1, 10) + clamp(3, 1, 10)", 13, false, true},
      // Phase 2: hygienic, syntax-extension, and anaphoric macros
      {"let tmp = 100 in with_tmp(7, tmp + 1)", 108, false, true},
      {"unless 1 > 2 then 42 else 99", 42, false, false},
      {"unless 5 > 2 then 42 else 99", 99, false, false},
      {"aif 10 + 5 then it * 2 else 0", 30, false, false},
      {"aif 3 - 3 then it * 2 else 77", 77, false, false},
      // Minilang core: lambdas, polymorphic calls, records, field access
      {"let add = fn(a, b) => a + b in add(19, 23)", 42, false, true},
      {"let id = fn(x) => x in id(10) + id(32)", 42, false, false},
      {"let p = { x: 15, y: 27 } in p.x + p.y", 42, false, false},
      {"let id = fn(v) => v in let p = id({ a: 11, b: 31 }) in p.a + p.b", 42, false, false},
      // Phase 3: IFA-driven Stage-1 metaprogramming & record reflection
      {"let e = clamp(3, 0, 5) in let x = 4 in pow_opt(x, e)", 64, true, true},
      {"let k = 2 + 3 in if_const(k, 100, 200)", 100, true, true},
      {"let r = runtime_val(5) in if_const(r, 100, 200)", 200, true, true},
      {"let id = fn(v) => v in let p = id({ x: 10, y: 20, z: 12 }) in sum_fields(p)", 42, true,
       false},
      // Phase 4: in-language compile-time execution via IFA + LLVM LLJIT
      {"comptime(let sq = fn(x) => x * x in sq(6) + 6)", 42, false, true},
      // Comptime LLJIT feeding into IFA constant propagation -> Stage-1 pow_opt unroll!
      {"let n = comptime(let sq = fn(x) => x * x in sq(3) - 6) in pow_opt(2, n)", 8, true, true},
  };

  for (const auto &tc : kCases) {
    if (trace) std::fprintf(stderr, "CASE: %.*s\n", static_cast<int>(tc.expr.size()), tc.expr.data());
    langwb::EvalOptions opts;
    opts.staged = tc.staged;
    int64_t got = 0;
    langwb::ifa_bridge::CompileInfo info;
    if (!wb.eval_i64(tc.expr, &got, opts, &info)) {
      std::fprintf(stderr, "smoke: failed '%.*s'\n", static_cast<int>(tc.expr.size()),
                   tc.expr.data());
      return 1;
    }
    if (got != tc.expected) {
      std::fprintf(stderr, "smoke: '%.*s' => %lld, expected %lld\n",
                   static_cast<int>(tc.expr.size()), tc.expr.data(),
                   static_cast<long long>(got), static_cast<long long>(tc.expected));
      return 1;
    }
    if (tc.expect_const_inferred &&
        (!info.has_inferred_const_i64 || info.inferred_const_i64 != tc.expected)) {
      std::fprintf(stderr, "smoke: '%.*s' IFA did not infer constant %lld\n",
                   static_cast<int>(tc.expr.size()), tc.expr.data(),
                   static_cast<long long>(tc.expected));
      return 1;
    }
  }

  const auto &st = wb.stats();
  if (st.comptime_evals != 2 || st.stage1_unrolls != 2 || st.stage1_reflections != 1) {
    std::fprintf(stderr,
                 "smoke: unexpected stats comptime=%d (exp 2) unrolls=%d (exp 2) reflections=%d "
                 "(exp 1)\n",
                 st.comptime_evals, st.stage1_unrolls, st.stage1_reflections);
    return 1;
  }

  std::printf(
      "langwb smoke test passed (Phases 0-4: extensible grammar + hygienic/anaphoric macros + "
      "lambdas/records + custom prim/tfn + IFA-driven metaprogramming + comptime LLJIT x21)\n");
  return 0;
}

int run_repl(langwb::Workbench &wb) {
  std::printf("langwb REPL (minilang + IFA + LLVM LLJIT)\n");
  std::printf("commands: :cst <expr>, :expand <expr>, :attrs <expr>, :quit\n");
  std::string line;
  while (true) {
    std::printf("langwb> ");
    std::fflush(stdout);
    if (!std::getline(std::cin, line)) break;

    std::string_view sv(line);
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r'))
      sv.remove_suffix(1);
    if (sv.empty()) continue;
    if (sv == ":quit" || sv == ":q") break;

    langwb::EvalOptions opts;
    opts.staged = true;
    opts.diag_fp = stdout;
    if (sv.starts_with(":cst ")) {
      opts.dump_cst = true;
      sv.remove_prefix(5);
    } else if (sv.starts_with(":expand ")) {
      opts.dump_expanded = true;
      sv.remove_prefix(8);
    } else if (sv.starts_with(":attrs ")) {
      opts.dump_attrs = true;
      sv.remove_prefix(7);
    }

    int64_t val = 0;
    if (wb.eval_i64(sv, &val, opts)) {
      std::printf("=> %lld\n", static_cast<long long>(val));
    } else {
      std::fprintf(stderr, "error: evaluation failed\n");
    }
  }
  return 0;
}

void print_usage() {
  std::printf("langwb — programming language workbench (DParser + IFA + LLVM LLJIT)\n");
  std::printf("usage:\n");
  std::printf("  langwb --smoke\n");
  std::printf("  langwb --repl\n");
  std::printf("  langwb [-e <expr> | <file.ml>] [--dump-cst] [--dump-expanded] [--dump-attrs]\n");
}

}  // namespace

int main(int argc, char **argv) {
  GC_INIT();
  if (argc <= 1) {
    print_usage();
    return 0;
  }

  langwb::EvalOptions opts;
  opts.staged = true;
  std::string eval_expr;
  std::string file_path;
  bool want_repl = false;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--smoke") {
      return run_smoke_test();
    } else if (arg == "--repl") {
      want_repl = true;
    } else if (arg == "--dump-cst") {
      opts.dump_cst = true;
    } else if (arg == "--dump-expanded") {
      opts.dump_expanded = true;
    } else if (arg == "--dump-attrs") {
      opts.dump_attrs = true;
    } else if (arg == "-e" && i + 1 < argc) {
      eval_expr = argv[++i];
    } else if (arg == "-h" || arg == "--help") {
      print_usage();
      return 0;
    } else if (!arg.starts_with("-")) {
      file_path = std::string(arg);
    } else {
      std::fprintf(stderr, "langwb: unknown option '%.*s'\n", static_cast<int>(arg.size()),
                   arg.data());
      return 1;
    }
  }

  langwb::Workbench wb;
  if (want_repl) {
    return run_repl(wb);
  }

  std::string source = eval_expr;
  if (source.empty() && !file_path.empty()) {
    std::ifstream ifs(file_path);
    if (!ifs) {
      std::fprintf(stderr, "langwb: cannot open '%s'\n", file_path.c_str());
      return 1;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    source = oss.str();
  }

  if (source.empty()) {
    print_usage();
    return 1;
  }

  int64_t result = 0;
  if (!wb.eval_i64(source, &result, opts)) {
    std::fprintf(stderr, "langwb: evaluation failed\n");
    return 1;
  }
  std::printf("%lld\n", static_cast<long long>(result));
  return 0;
}
