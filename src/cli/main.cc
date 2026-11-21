#define EXTERN
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "gc/gc.h"
#include "ifa_bridge/attrs.h"
#include "ifa_bridge/bridge.h"
#include "ifa_bridge/prim_registry.h"
#include "llvm_internal.h"
#include "macro/expander.h"
#include "parse/grammar_def.h"
#include "parse/parse.h"
#include "syntax/syntax.h"

namespace {

struct SmokeCase {
  std::string_view expr;
  int64_t expected;
  bool expect_const_inferred = false;
};

langwb::parse::GrammarDef make_core_grammar() {
  langwb::parse::GrammarDef g;
  g.add_rule("expr", "'let' \"[a-zA-Z_][a-zA-Z0-9_]*\" '=' expr 'in' expr", "$right 0");
  g.add_rule("expr", "'if' expr 'then' expr 'else' expr", "$right 0");
  g.add_rule("expr", "'fn' '(' params ')' '=>' expr", "$right 0");
  g.add_rule("expr", "expr '==' expr", "$binary_op_left 1");
  g.add_rule("expr", "expr '!=' expr", "$binary_op_left 1");
  g.add_rule("expr", "expr '<' expr", "$binary_op_left 2");
  g.add_rule("expr", "expr '<=' expr", "$binary_op_left 2");
  g.add_rule("expr", "expr '>' expr", "$binary_op_left 2");
  g.add_rule("expr", "expr '>=' expr", "$binary_op_left 2");
  g.add_rule("expr", "expr '+' expr", "$binary_op_left 3");
  g.add_rule("expr", "expr '-' expr", "$binary_op_left 3");
  g.add_rule("expr", "expr '*' expr", "$binary_op_left 4");
  g.add_rule("expr", "expr '/' expr", "$binary_op_left 4");
  g.add_rule("expr", "expr '.' \"[a-zA-Z_][a-zA-Z0-9_]*\"", "$left 5");
  g.add_rule("expr", "'{' fields '}'");
  g.add_rule("expr", "\"[a-zA-Z_][a-zA-Z0-9_]*\" '(' args ')'");
  g.add_rule("expr", "'(' expr ')'");
  g.add_rule("expr", "\"[0-9]+\"");
  g.add_rule("expr", "\"[a-zA-Z_][a-zA-Z0-9_]*\"");
  g.add_rule("args", "expr (',' expr)*");
  g.add_rule("params", "\"[a-zA-Z_][a-zA-Z0-9_]*\" (',' \"[a-zA-Z_][a-zA-Z0-9_]*\")*");
  g.add_rule("fields", "field_init (',' field_init)*");
  g.add_rule("field_init", "\"[a-zA-Z_][a-zA-Z0-9_]*\" ':' expr");
  return g;
}

void register_demo_primitives(langwb::ifa_bridge::PrimRegistry &reg) {
  using namespace langwb::ifa_bridge;

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

  reg.register_prim(
      "runtime_val",
      PrimitiveHandler{
          .transfer =
              [](PNode *pn, EntrySet *es) {
                prim_emit_result(pn, es, make_abstract_type(sym_int64));
              },
          .emit_llvm =
              [](PNode *pn, Fun * /*f*/) {
                llvm::Value *v = llvm_emit_get_var(pn->rvals[2]);
                if (pn->lvals.n > 0) llvm_emit_put_result(pn->lvals[0], v);
              },
          .is_functional = true,
          .is_visible = true,
      });

  // Stage-0 analysis-only placeholders for Stage-1 attribute-driven macros.
  auto make_i64_placeholder = []() {
    return PrimitiveHandler{
        .transfer =
            [](PNode *pn, EntrySet *es) {
              prim_emit_result(pn, es, make_abstract_type(sym_int64));
            },
        .is_functional = true,
        .is_visible = true,
    };
  };
  reg.register_prim("pow_opt", make_i64_placeholder());
  reg.register_prim("if_const", make_i64_placeholder());
  reg.register_prim("sum_fields", make_i64_placeholder());
}

void register_stage0_macros(langwb::macro::MacroExpander &expander) {
  using namespace langwb::macro;
  using namespace langwb::syntax;

  expander.register_call_macro("with_tmp", [](SyntaxObj *call, MacroContext &ctx) -> SyntaxObj * {
    auto args = MacroExpander::collect_call_args(call->children[2]);
    if (args.size() != 2) return nullptr;
    return ctx.parse_template("expr", "let tmp = __val__ in tmp + __body__",
                              {{"__val__", args[0]}, {"__body__", args[1]}});
  });

  expander.register_form_macro("unless_expr", [](SyntaxObj *form, MacroContext &ctx) -> SyntaxObj * {
    if (form->children.size() != 6) return nullptr;
    return ctx.parse_template("expr", "if __c__ then __e__ else __t__",
                              {{"__c__", form->children[1]},
                               {"__t__", form->children[3]},
                               {"__e__", form->children[5]}});
  });

  expander.register_form_macro("aif_expr", [](SyntaxObj *form, MacroContext &ctx) -> SyntaxObj * {
    if (form->children.size() != 6) return nullptr;
    SyntaxObj *it_id = ctx.unhygienic_ident("it");
    return ctx.parse_template("expr", "let __it__ = __c__ in if __it__ != 0 then __t__ else __e__",
                              {{"__it__", it_id},
                               {"__c__", form->children[1]},
                               {"__t__", form->children[3]},
                               {"__e__", form->children[5]}});
  });
}

void register_stage1_macros(langwb::macro::MacroExpander &expander, int *unroll_count,
                            int *reflect_count) {
  using namespace langwb::macro;
  using namespace langwb::syntax;

  expander.register_call_macro(
      "pow_opt", [unroll_count](SyntaxObj *call, MacroContext &ctx) -> SyntaxObj * {
        auto args = MacroExpander::collect_call_args(call->children[2]);
        if (args.size() != 2 || !ctx.attrs()) return nullptr;

        int64_t exp_val = 0;
        if (ctx.attrs()->is_const_i64(args[1], &exp_val) && exp_val >= 0 && exp_val <= 16) {
          if (unroll_count) ++(*unroll_count);
          if (exp_val == 0) return ctx.parse_template("expr", "1");
          std::string body = "b";
          for (int64_t i = 1; i < exp_val; ++i) body += " * b";
          return ctx.parse_template("expr", "let b = __base__ in " + body, {{"__base__", args[0]}});
        }
        return nullptr;
      });

  expander.register_call_macro("if_const", [](SyntaxObj *call, MacroContext &ctx) -> SyntaxObj * {
    auto args = MacroExpander::collect_call_args(call->children[2]);
    if (args.size() != 3 || !ctx.attrs()) return nullptr;
    return ctx.attrs()->is_const_i64(args[0]) ? args[1]->clone() : args[2]->clone();
  });

  // Stage-1 compile-time record reflection macro: sum_fields(rec_expr)
  // Queries IFA's inferred Type_RECORD field_names (even through polymorphic
  // functions!) and hygienically synthesizes `let r = rec_expr in r.f0 + r.f1 + ...`.
  expander.register_call_macro(
      "sum_fields", [reflect_count](SyntaxObj *call, MacroContext &ctx) -> SyntaxObj * {
        auto args = MacroExpander::collect_call_args(call->children[2]);
        if (args.size() != 1 || !ctx.attrs()) return nullptr;
        const auto *attr = ctx.attrs()->get(args[0]);
        if (!attr || attr->field_names.empty()) return nullptr;

        if (reflect_count) ++(*reflect_count);
        std::string sum;
        for (size_t i = 0; i < attr->field_names.size(); ++i) {
          if (i > 0) sum += " + ";
          sum += "r." + attr->field_names[i];
        }
        return ctx.parse_template("expr", "let r = __rec__ in " + sum, {{"__rec__", args[0]}});
      });
}

int run_smoke_test() {
  langwb::parse::GrammarDef grammar_def = make_core_grammar();

  langwb::parse::GrammarDef ext;
  ext.add_rule("expr", "unless_expr");
  ext.add_rule("expr", "aif_expr");
  ext.add_rule("unless_expr", "'unless' expr 'then' expr 'else' expr", "$right 0");
  ext.add_rule("aif_expr", "'aif' expr 'then' expr 'else' expr", "$right 0");
  grammar_def.extend(ext);

  langwb::parse::GrammarCache cache;
  auto g = cache.compile_or_get(grammar_def);
  auto g_cached = cache.compile_or_get(grammar_def);
  if (!g || g != g_cached || cache.size() != 1) {
    std::fprintf(stderr, "smoke: GrammarCache failed to compile/cache extended grammar\n");
    return 1;
  }

  if (g->parse_check("1 + * 3")) {
    std::fprintf(stderr, "smoke: unexpectedly accepted invalid input\n");
    return 1;
  }

  langwb::ifa_bridge::PrimRegistry prims;
  register_demo_primitives(prims);

  langwb::macro::MacroExpander stage0;
  register_stage0_macros(stage0);

  int unroll_count = 0;
  int reflect_count = 0;
  langwb::macro::MacroExpander stage1;
  register_stage1_macros(stage1, &unroll_count, &reflect_count);

  constexpr SmokeCase kCases[] = {
      {"1 + 2 * 3", 7, true},
      {"(1 + 2) * 3", 9, true},
      {"let x = 10 in let y = x * 4 + 2 in y - 5", 37, true},
      {"clamp(15, 0, 10)", 10, true},
      {"clamp(0 - 5, 0, 10)", 0, true},
      {"let x = 7 in clamp(x * 2, 1, 10) + clamp(3, 1, 10)", 13, true},
      {"let tmp = 100 in with_tmp(7, tmp + 1)", 108, true},
      {"unless 1 > 2 then 42 else 99", 42, false},
      {"unless 5 > 2 then 42 else 99", 99, false},
      {"aif 10 + 5 then it * 2 else 0", 30, false},
      {"aif 3 - 3 then it * 2 else 77", 77, false},
      // Lambdas & polymorphic function calls (IFA contour splitting)
      {"let add = fn(a, b) => a + b in add(19, 23)", 42, true},
      {"let id = fn(x) => x in id(10) + id(32)", 42, false},
      // Records & field access
      {"let p = { x: 15, y: 27 } in p.x + p.y", 42, false},
      // Polymorphic function passing a record!
      {"let id = fn(v) => v in let p = id({ a: 11, b: 31 }) in p.a + p.b", 42, false},
  };

  bool trace = std::getenv("LANGWB_TRACE") != nullptr;

  for (const auto &tc : kCases) {
    if (trace) std::fprintf(stderr, "CASE: %.*s\n", static_cast<int>(tc.expr.size()), tc.expr.data());
    langwb::parse::CstNode *cst = g->parse(tc.expr);
    if (!cst) {
      std::fprintf(stderr, "smoke: failed to parse '%.*s'\n", static_cast<int>(tc.expr.size()),
                   tc.expr.data());
      return 1;
    }

    langwb::syntax::ScopeAllocator scopes;
    langwb::syntax::SyntaxObj *stx = langwb::syntax::SyntaxObj::from_cst(cst);
    langwb::syntax::SyntaxObj *expanded = stage0.expand(stx, *g, scopes);
    if (!expanded) {
      std::fprintf(stderr, "smoke: failed to expand macros for '%.*s'\n",
                   static_cast<int>(tc.expr.size()), tc.expr.data());
      return 1;
    }

    int64_t got = 0;
    langwb::ifa_bridge::CompileInfo info;
    if (!langwb::ifa_bridge::compile_and_jit_i64(expanded, &got, &prims, &info)) {
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

  // Phase 3 staged cases: IFA-driven metaprogramming & record reflection!
  constexpr SmokeCase kStagedCases[] = {
      {"let e = clamp(3, 0, 5) in let x = 4 in pow_opt(x, e)", 64, true},
      {"let k = 2 + 3 in if_const(k, 100, 200)", 100, true},
      {"let r = runtime_val(5) in if_const(r, 100, 200)", 200, true},
      // IFA infers record fields through polymorphic `id`, then Stage-1 `sum_fields` reflects them!
      {"let id = fn(v) => v in let p = id({ x: 10, y: 20, z: 12 }) in sum_fields(p)", 42, false},
  };

  for (const auto &tc : kStagedCases) {
    langwb::parse::CstNode *cst = g->parse(tc.expr);
    if (!cst) {
      std::fprintf(stderr, "smoke: failed to parse staged '%.*s'\n",
                   static_cast<int>(tc.expr.size()), tc.expr.data());
      return 1;
    }

    langwb::syntax::ScopeAllocator scopes;
    langwb::syntax::SyntaxObj *stx = langwb::syntax::SyntaxObj::from_cst(cst);
    int64_t got = 0;
    langwb::ifa_bridge::CompileInfo info;
    if (!langwb::ifa_bridge::compile_staged_i64(stx, *g, scopes, stage0, stage1, &prims, &got,
                                                &info)) {
      std::fprintf(stderr, "smoke: failed staged compile/JIT '%.*s'\n",
                   static_cast<int>(tc.expr.size()), tc.expr.data());
      return 1;
    }
    if (got != tc.expected) {
      std::fprintf(stderr, "smoke: staged '%.*s' => %lld, expected %lld\n",
                   static_cast<int>(tc.expr.size()), tc.expr.data(),
                   static_cast<long long>(got), static_cast<long long>(tc.expected));
      return 1;
    }
    if (tc.expect_const_inferred &&
        (!info.has_inferred_const_i64 || info.inferred_const_i64 != tc.expected)) {
      std::fprintf(stderr, "smoke: staged '%.*s' final IFA did not infer constant %lld\n",
                   static_cast<int>(tc.expr.size()), tc.expr.data(),
                   static_cast<long long>(tc.expected));
      return 1;
    }
  }

  if (unroll_count != 1 || reflect_count != 1) {
    std::fprintf(stderr, "smoke: expected unroll_count=1 reflect_count=1, got %d / %d\n",
                 unroll_count, reflect_count);
    return 1;
  }

  std::printf(
      "langwb smoke test passed (extensible grammar + hygienic/anaphoric macros + lambdas/records "
      "+ custom prim/tfn + IFA-driven unroll & record reflection + LLVM LLJIT x19)\n");
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
