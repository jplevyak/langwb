#define EXTERN
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "gc/gc.h"
#include "ifa_bridge/bridge.h"
#include "ifa_bridge/prim_registry.h"
#include "llvm_internal.h"
#include "parse/parse.h"

namespace {

struct SmokeCase {
  std::string_view expr;
  int64_t expected;
  bool expect_const_inferred = false;
};

void register_demo_primitives(langwb::ifa_bridge::PrimRegistry &reg) {
  using namespace langwb::ifa_bridge;

  // Custom frontend-injected primitive: clamp(x, lo, hi)
  // - Transfer function: constant-folds into IFA's AType lattice when all 3
  //   operands are known constant ints, otherwise yields sym_int64.
  // - LLVM emitter: emits inline icmp + select instructions (zero runtime call).
  reg.register_prim(
      "clamp",
      PrimitiveHandler{
          .transfer =
              [](PNode *pn, EntrySet *es) {
                int64_t x = 0, lo = 0, hi = 0;
                if (try_get_const_i64(prim_arg_avar(pn, es, 0), &x) &&
                    try_get_const_i64(prim_arg_avar(pn, es, 1), &lo) &&
                    try_get_const_i64(prim_arg_avar(pn, es, 2), &hi)) {
                  prim_emit_result(pn, es, make_const_i64_type(std::clamp(x, lo, hi)));
                } else {
                  prim_emit_result(pn, es, make_abstract_type(sym_int64));
                }
              },
          .emit_c =
              [](FILE *fp, PNode *pn, Fun * /*f*/) {
                if (pn->lvals.n > 0 && pn->lvals[0]->live) {
                  std::fprintf(fp, "%s = (%s < %s ? %s : (%s > %s ? %s : %s));\n",
                               pn->lvals[0]->cg_string, pn->rvals[2]->cg_string,
                               pn->rvals[3]->cg_string, pn->rvals[3]->cg_string,
                               pn->rvals[2]->cg_string, pn->rvals[4]->cg_string,
                               pn->rvals[4]->cg_string, pn->rvals[2]->cg_string);
                }
              },
          .emit_llvm =
              [](PNode *pn, Fun * /*f*/) {
                llvm::Value *x = llvm_emit_get_var(pn->rvals[2]);
                llvm::Value *lo = llvm_emit_get_var(pn->rvals[3]);
                llvm::Value *hi = llvm_emit_get_var(pn->rvals[4]);
                llvm::Value *lt_lo = Builder->CreateICmpSLT(x, lo, "clamp.lt");
                llvm::Value *clamped_lo = Builder->CreateSelect(lt_lo, lo, x, "clamp.lo");
                llvm::Value *gt_hi = Builder->CreateICmpSGT(clamped_lo, hi, "clamp.gt");
                llvm::Value *res = Builder->CreateSelect(gt_hi, hi, clamped_lo, "clamp.res");
                if (pn->lvals.n > 0) {
                  llvm_emit_put_result(pn->lvals[0], res);
                }
              },
          .is_functional = true,
          .is_visible = true,
      });
}

int run_smoke_test() {
  constexpr std::string_view kGrammar = R"(
    expr: 'let' "[a-zA-Z_][a-zA-Z0-9_]*" '=' expr 'in' expr $right 0
        | expr '+' expr $binary_op_left 1
        | expr '-' expr $binary_op_left 1
        | expr '*' expr $binary_op_left 2
        | expr '/' expr $binary_op_left 2
        | "[a-zA-Z_][a-zA-Z0-9_]*" '(' args ')'
        | '(' expr ')'
        | "[0-9]+"
        | "[a-zA-Z_][a-zA-Z0-9_]*";
    args: expr (',' expr)*;
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

  langwb::ifa_bridge::PrimRegistry prims;
  register_demo_primitives(prims);

  constexpr SmokeCase kCases[] = {
      {"1 + 2 * 3", 7, true},
      {"(1 + 2) * 3", 9, true},
      {"let x = 10 in let y = x * 4 + 2 in y - 5", 37, true},
      {"clamp(15, 0, 10)", 10, true},
      {"clamp(0 - 5, 0, 10)", 0, true},
      {"let x = 7 in clamp(x * 2, 1, 10) + clamp(3, 1, 10)", 13, true},
  };

  for (const auto &tc : kCases) {
    langwb::parse::CstNode *cst = g.parse(tc.expr);
    if (!cst) {
      std::fprintf(stderr, "smoke: failed to parse '%.*s'\n", static_cast<int>(tc.expr.size()),
                   tc.expr.data());
      return 1;
    }
    int64_t got = 0;
    langwb::ifa_bridge::CompileInfo info;
    if (!langwb::ifa_bridge::compile_and_jit_i64(cst, &got, &prims, &info)) {
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
    if (tc.expect_const_inferred &&
        (!info.has_inferred_const_i64 || info.inferred_const_i64 != tc.expected)) {
      std::fprintf(stderr, "smoke: '%.*s' IFA did not infer expected constant %lld\n",
                   static_cast<int>(tc.expr.size()), tc.expr.data(),
                   static_cast<long long>(tc.expected));
      return 1;
    }
  }

  std::printf(
      "langwb smoke test passed (in-memory DParser -> CST -> IF1 -> custom prim/tfn -> IFA const "
      "prop -> inline LLVM LLJIT x6)\n");
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
