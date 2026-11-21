#include "workbench/workbench.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "ifa_bridge/meta_abi.h"
#include "llvm_internal.h"

namespace langwb {

namespace {

void dump_attrs_tree(const syntax::SyntaxObj *node, const ifa_bridge::AnalysisAttrs &attrs,
                     FILE *fp, int indent = 0) {
  if (!node) return;
  const auto *a = attrs.get(node);
  if (a && a->analyzed) {
    for (int i = 0; i < indent; ++i) std::fputc(' ', fp);
    std::fprintf(fp, "%s \"%s\" [id=%llu", node->symbol.c_str(), node->text.c_str(),
                 static_cast<unsigned long long>(node->node_id));
    if (!a->type_names.empty()) {
      std::fprintf(fp, " type=");
      for (size_t i = 0; i < a->type_names.size(); ++i) {
        if (i > 0) std::fputc('|', fp);
        std::fprintf(fp, "%s", a->type_names[i].c_str());
      }
    }
    if (a->has_const_i64) {
      std::fprintf(fp, " const=%lld", static_cast<long long>(a->const_i64));
    }
    if (!a->field_names.empty()) {
      std::fprintf(fp, " fields={");
      for (size_t i = 0; i < a->field_names.size(); ++i) {
        if (i > 0) std::fputc(',', fp);
        std::fprintf(fp, "%s", a->field_names[i].c_str());
      }
      std::fputc('}', fp);
    }
    std::fprintf(fp, "]\n");
  }
  for (const syntax::SyntaxObj *c : node->children) {
    dump_attrs_tree(c, attrs, fp, indent + 2);
  }
}

std::string_view trim_ws(std::string_view sv) {
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
  return sv;
}

bool match_keyword(std::string_view sv, std::string_view kw) {
  if (!sv.starts_with(kw)) return false;
  if (sv.size() == kw.size()) return true;
  unsigned char next = static_cast<unsigned char>(sv[kw.size()]);
  return std::isspace(next);
}

// Finds the next `;` outside of string literals and nested parens/braces.
size_t find_decl_terminator(std::string_view sv) {
  bool in_str = false;
  int depth = 0;
  for (size_t i = 0; i < sv.size(); ++i) {
    char c = sv[i];
    if (in_str) {
      if (c == '\\' && i + 1 < sv.size()) {
        ++i;
      } else if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      in_str = true;
    } else if (c == '(' || c == '{') {
      ++depth;
    } else if (c == ')' || c == '}') {
      if (depth > 0) --depth;
    } else if (c == ';' && depth == 0) {
      return i;
    }
  }
  return std::string_view::npos;
}

bool parse_quoted_string(std::string_view &sv, std::string &out) {
  sv = trim_ws(sv);
  if (sv.empty() || sv.front() != '"') return false;
  sv.remove_prefix(1);
  out.clear();
  while (!sv.empty()) {
    char c = sv.front();
    sv.remove_prefix(1);
    if (c == '\\' && !sv.empty()) {
      char next = sv.front();
      sv.remove_prefix(1);
      out.push_back(next);
    } else if (c == '"') {
      return true;
    } else {
      out.push_back(c);
    }
  }
  return false;
}

bool parse_ident(std::string_view &sv, std::string &out) {
  sv = trim_ws(sv);
  if (sv.empty() || !(std::isalpha(static_cast<unsigned char>(sv[0])) || sv[0] == '_')) {
    return false;
  }
  size_t i = 1;
  while (i < sv.size() && (std::isalnum(static_cast<unsigned char>(sv[i])) || sv[i] == '_')) {
    ++i;
  }
  out.assign(sv.substr(0, i));
  sv.remove_prefix(i);
  return true;
}

}  // namespace

Workbench::Workbench() : cache_(std::make_shared<parse::GrammarCache>()) { init_default_minilang(); }

Workbench::Workbench(const Workbench &parent, bool /*share_cache*/)
    : grammar_def_(parent.grammar_def_),
      cache_(parent.cache_),
      prims_(parent.prims_),
      stage0_(parent.stage0_),
      stage1_(parent.stage1_),
      stats_(parent.stats_) {}

std::string Workbench::strip_comments(std::string_view src) {
  std::string out;
  out.reserve(src.size());
  bool in_comment = false;
  bool in_str = false;
  for (size_t i = 0; i < src.size(); ++i) {
    char c = src[i];
    if (in_comment) {
      if (c == '\n') {
        in_comment = false;
        out.push_back('\n');
      }
      continue;
    }
    if (in_str) {
      out.push_back(c);
      if (c == '\\' && i + 1 < src.size()) {
        out.push_back(src[++i]);
      } else if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      in_str = true;
      out.push_back(c);
    } else if (c == '#') {
      in_comment = true;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool Workbench::starts_with_decl(std::string_view src) {
  src = trim_ws(src);
  return match_keyword(src, "grammar") || match_keyword(src, "macro") ||
         match_keyword(src, "form_macro") || match_keyword(src, "stage1_macro") ||
         match_keyword(src, "stage1_form_macro");
}

bool Workbench::apply_decl(std::string_view decl_src) {
  std::string_view sv = trim_ws(decl_src);
  if (!sv.empty() && sv.back() == ';') sv.remove_suffix(1);
  sv = trim_ws(sv);
  if (sv.empty()) return false;

  // 1. grammar <prod> += "<rhs>" ["<modifier>"]
  if (match_keyword(sv, "grammar")) {
    sv.remove_prefix(7);
    std::string prod;
    if (!parse_ident(sv, prod)) return false;
    sv = trim_ws(sv);
    if (!sv.starts_with("+=")) return false;
    sv.remove_prefix(2);
    std::string rhs;
    if (!parse_quoted_string(sv, rhs)) return false;
    std::string mod;
    sv = trim_ws(sv);
    if (!sv.empty()) {
      if (!parse_quoted_string(sv, mod)) return false;
      if (!trim_ws(sv).empty()) return false;
    }
    grammar_def_.add_rule(std::move(prod), std::move(rhs), std::move(mod));
    ++stats_.decls_applied;
    return true;
  }

  // 2. [stage1_][form_]macro <name> => <minilang-expr>
  bool is_stage1 = false;
  bool is_form = false;
  if (match_keyword(sv, "macro")) {
    sv.remove_prefix(5);
  } else if (match_keyword(sv, "form_macro")) {
    sv.remove_prefix(10);
    is_form = true;
  } else if (match_keyword(sv, "stage1_macro")) {
    sv.remove_prefix(12);
    is_stage1 = true;
  } else if (match_keyword(sv, "stage1_form_macro")) {
    sv.remove_prefix(17);
    is_stage1 = true;
    is_form = true;
  } else {
    return false;
  }

  std::string name;
  if (!parse_ident(sv, name)) return false;
  sv = trim_ws(sv);
  if (!sv.starts_with("=>")) return false;
  sv.remove_prefix(2);
  sv = trim_ws(sv);
  if (sv.empty()) return false;

  std::string body(sv);
  if (!is_stage1 && !is_form) {
    register_guest_stage0_macro(std::move(name), std::move(body));
  } else if (!is_stage1 && is_form) {
    register_guest_stage0_form_macro(std::move(name), std::move(body));
  } else if (is_stage1 && !is_form) {
    register_guest_stage1_macro(std::move(name), std::move(body));
  } else {
    register_guest_stage1_form_macro(std::move(name), std::move(body));
  }
  ++stats_.decls_applied;
  return true;
}

void Workbench::init_default_minilang() {
  using namespace ifa_bridge;
  using namespace macro;
  using namespace syntax;

  // Core minilang grammar.
  grammar_def_.add_rule("expr", "'let' \"[a-zA-Z_][a-zA-Z0-9_]*\" '=' expr 'in' expr", "$right 0");
  grammar_def_.add_rule("expr", "'if' expr 'then' expr 'else' expr", "$right 0");
  grammar_def_.add_rule("expr", "'fn' '(' params ')' '=>' expr", "$right 0");
  grammar_def_.add_rule("expr", "expr '==' expr", "$binary_op_left 1");
  grammar_def_.add_rule("expr", "expr '!=' expr", "$binary_op_left 1");
  grammar_def_.add_rule("expr", "expr '<' expr", "$binary_op_left 2");
  grammar_def_.add_rule("expr", "expr '<=' expr", "$binary_op_left 2");
  grammar_def_.add_rule("expr", "expr '>' expr", "$binary_op_left 2");
  grammar_def_.add_rule("expr", "expr '>=' expr", "$binary_op_left 2");
  grammar_def_.add_rule("expr", "expr '+' expr", "$binary_op_left 3");
  grammar_def_.add_rule("expr", "expr '-' expr", "$binary_op_left 3");
  grammar_def_.add_rule("expr", "expr '*' expr", "$binary_op_left 4");
  grammar_def_.add_rule("expr", "expr '/' expr", "$binary_op_left 4");
  grammar_def_.add_rule("expr", "expr '.' \"[a-zA-Z_][a-zA-Z0-9_]*\"", "$left 5");
  grammar_def_.add_rule("expr", "'{' fields '}'");
  grammar_def_.add_rule("expr", "\"[a-zA-Z_][a-zA-Z0-9_]*\" '(' args ')'");
  grammar_def_.add_rule("expr", "\"[a-zA-Z_][a-zA-Z0-9_]*\" '(' ')'");
  grammar_def_.add_rule("expr", "'(' expr ')'");
  grammar_def_.add_rule("expr", "\"\\\"[^\\\"]*\\\"\"");
  grammar_def_.add_rule("expr", "\"[0-9]+\"");
  grammar_def_.add_rule("expr", "\"[a-zA-Z_][a-zA-Z0-9_]*\"");
  grammar_def_.add_rule("args", "expr (',' expr)*");
  grammar_def_.add_rule("params", "\"[a-zA-Z_][a-zA-Z0-9_]*\" (',' \"[a-zA-Z_][a-zA-Z0-9_]*\")*");
  grammar_def_.add_rule("fields", "field_init (',' field_init)*");
  grammar_def_.add_rule("field_init", "\"[a-zA-Z_][a-zA-Z0-9_]*\" ':' expr");

  // Composable syntax extensions: `unless` and anaphoric `aif`.
  parse::GrammarDef ext;
  ext.add_rule("expr", "unless_expr");
  ext.add_rule("expr", "aif_expr");
  ext.add_rule("unless_expr", "'unless' expr 'then' expr 'else' expr", "$right 0");
  ext.add_rule("aif_expr", "'aif' expr 'then' expr 'else' expr", "$right 0");
  grammar_def_.extend(ext);

  // Builtin frontend-injected primitives.
  prims_.register_prim(
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

  prims_.register_prim(
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
  prims_.register_prim("pow_opt", make_i64_placeholder());
  prims_.register_prim("if_const", make_i64_placeholder());
  prims_.register_prim("sum_fields", make_i64_placeholder());
  register_meta_abi_prims(prims_);

  // Stage-0 macros (hygienic, form-based, anaphoric, and comptime LLJIT).
  stage0_.register_call_macro("with_tmp", [](SyntaxObj *call, MacroContext &ctx) -> SyntaxObj * {
    auto args = MacroExpander::collect_call_args(call->children[2]);
    if (args.size() != 2) return nullptr;
    return ctx.parse_template("expr", "let tmp = __val__ in tmp + __body__",
                              {{"__val__", args[0]}, {"__body__", args[1]}});
  });

  stage0_.register_form_macro("unless_expr", [](SyntaxObj *form, MacroContext &ctx) -> SyntaxObj * {
    if (form->children.size() != 6) return nullptr;
    return ctx.parse_template("expr", "if __c__ then __e__ else __t__",
                              {{"__c__", form->children[1]},
                               {"__t__", form->children[3]},
                               {"__e__", form->children[5]}});
  });

  stage0_.register_form_macro("aif_expr", [](SyntaxObj *form, MacroContext &ctx) -> SyntaxObj * {
    if (form->children.size() != 6) return nullptr;
    SyntaxObj *it_id = ctx.unhygienic_ident("it");
    return ctx.parse_template("expr", "let __it__ = __c__ in if __it__ != 0 then __t__ else __e__",
                              {{"__it__", it_id},
                               {"__c__", form->children[1]},
                               {"__t__", form->children[3]},
                               {"__e__", form->children[5]}});
  });

  stage0_.register_call_macro("comptime", [this](SyntaxObj *call, MacroContext &ctx) -> SyntaxObj * {
    auto args = MacroExpander::collect_call_args(call->children[2]);
    if (args.size() != 1) return nullptr;

    ScopeAllocator ct_scopes;
    SyntaxObj *ct_expanded = stage0_.expand(args[0]->clone(), ctx.grammar(), ct_scopes);
    if (!ct_expanded) return nullptr;

    int64_t val = 0;
    if (!compile_and_jit_i64(ct_expanded, &val, &prims_)) {
      std::fprintf(stderr, "langwb: comptime evaluation failed\n");
      return nullptr;
    }
    ++stats_.comptime_evals;
    if (val < 0) {
      return ctx.parse_template("expr", "(0 - " + std::to_string(-val) + ")");
    }
    return ctx.parse_template("expr", std::to_string(val));
  });

  // Stage-1 attribute-driven macros (controlled by IFA AnalysisAttrs).
  stage1_.register_call_macro("pow_opt", [this](SyntaxObj *call, MacroContext &ctx) -> SyntaxObj * {
    auto args = MacroExpander::collect_call_args(call->children[2]);
    if (args.size() != 2 || !ctx.attrs()) return nullptr;

    int64_t exp_val = 0;
    if (ctx.attrs()->is_const_i64(args[1], &exp_val) && exp_val >= 0 && exp_val <= 16) {
      ++stats_.stage1_unrolls;
      if (exp_val == 0) return ctx.parse_template("expr", "1");
      std::string body = "b";
      for (int64_t i = 1; i < exp_val; ++i) body += " * b";
      return ctx.parse_template("expr", "let b = __base__ in " + body, {{"__base__", args[0]}});
    }
    return nullptr;
  });

  stage1_.register_call_macro("if_const", [](SyntaxObj *call, MacroContext &ctx) -> SyntaxObj * {
    auto args = MacroExpander::collect_call_args(call->children[2]);
    if (args.size() != 3 || !ctx.attrs()) return nullptr;
    return ctx.attrs()->is_const_i64(args[0]) ? args[1]->clone() : args[2]->clone();
  });

  stage1_.register_call_macro(
      "sum_fields", [this](SyntaxObj *call, MacroContext &ctx) -> SyntaxObj * {
        auto args = MacroExpander::collect_call_args(call->children[2]);
        if (args.size() != 1 || !ctx.attrs()) return nullptr;
        const auto *attr = ctx.attrs()->get(args[0]);
        if (!attr || attr->field_names.empty()) return nullptr;

        ++stats_.stage1_reflections;
        std::string sum;
        for (size_t i = 0; i < attr->field_names.size(); ++i) {
          if (i > 0) sum += " + ";
          sum += "r." + attr->field_names[i];
        }
        return ctx.parse_template("expr", "let r = __rec__ in " + sum, {{"__rec__", args[0]}});
      });
}

void Workbench::register_guest_macro_impl(macro::MacroExpander &target, bool is_form,
                                          bool is_stage1, std::string name,
                                          std::string guest_src) {
  if (is_stage1 && !is_form && !prims_.has(name)) {
    prims_.register_prim(name, ifa_bridge::PrimitiveHandler{
                                   .transfer =
                                       [](PNode *pn, EntrySet *es) {
                                         ifa_bridge::prim_emit_result(
                                             pn, es, make_abstract_type(sym_int64));
                                       },
                                   .is_functional = true,
                                   .is_visible = true,
                               });
  }
  auto fn = [this, src = std::move(guest_src)](syntax::SyntaxObj *call,
                                               macro::MacroContext &ctx) -> syntax::SyntaxObj * {
    parse::CstNode *cst = ctx.grammar().parse(src);
    if (!cst) return nullptr;
    syntax::ScopeAllocator m_scopes;
    syntax::SyntaxObj *mstx =
        stage0_.expand(syntax::SyntaxObj::from_cst(cst), ctx.grammar(), m_scopes);
    if (!mstx) return nullptr;

    ifa_bridge::MetaSession session(call, &ctx, &grammar_def_);
    ifa_bridge::ScopedMetaSession guard(&session);
    int64_t handle = 0;
    if (!ifa_bridge::compile_and_run_in_process_i64(mstx, &handle, &prims_)) return nullptr;
    return session.lookup(handle);
  };
  if (is_form) {
    target.register_form_macro(std::move(name), std::move(fn));
  } else {
    target.register_call_macro(std::move(name), std::move(fn));
  }
}

void Workbench::register_guest_stage0_macro(std::string name, std::string guest_src) {
  register_guest_macro_impl(stage0_, false, false, std::move(name), std::move(guest_src));
}

void Workbench::register_guest_stage1_macro(std::string name, std::string guest_src) {
  register_guest_macro_impl(stage1_, false, true, std::move(name), std::move(guest_src));
}

void Workbench::register_guest_stage0_form_macro(std::string symbol_name, std::string guest_src) {
  register_guest_macro_impl(stage0_, true, false, std::move(symbol_name), std::move(guest_src));
}

void Workbench::register_guest_stage1_form_macro(std::string symbol_name, std::string guest_src) {
  register_guest_macro_impl(stage1_, true, true, std::move(symbol_name), std::move(guest_src));
}

std::shared_ptr<parse::DynamicGrammar> Workbench::compiled_grammar() {
  return cache_->compile_or_get(grammar_def_);
}

bool Workbench::eval_i64(std::string_view src, int64_t *out_result, const EvalOptions &opts,
                         ifa_bridge::CompileInfo *out_info) {
  std::string cleaned = strip_comments(src);
  std::string_view rest = trim_ws(cleaned);

  // If the input contains leading in-language grammar/macro declarations,
  // execute them in a hermetic per-evaluation Workbench clone (sharing the
  // GrammarCache) so file-local extensions don't leak across evaluations.
  if (starts_with_decl(rest)) {
    Workbench session(*this, /*share_cache=*/true);
    while (starts_with_decl(rest)) {
      size_t semi = find_decl_terminator(rest);
      if (semi == std::string_view::npos) {
        std::fprintf(stderr, "langwb: missing ';' at end of declaration\n");
        return false;
      }
      std::string_view decl = rest.substr(0, semi);
      if (!session.apply_decl(decl)) {
        std::fprintf(stderr, "langwb: invalid declaration '%.*s'\n", static_cast<int>(decl.size()),
                     decl.data());
        return false;
      }
      rest = trim_ws(rest.substr(semi + 1));
    }
    if (rest.empty()) {
      std::fprintf(stderr, "langwb: expected expression after declarations\n");
      return false;
    }
    bool ok = session.eval_i64(rest, out_result, opts, out_info);
    stats_.comptime_evals += (session.stats_.comptime_evals - stats_.comptime_evals);
    stats_.stage1_unrolls += (session.stats_.stage1_unrolls - stats_.stage1_unrolls);
    stats_.stage1_reflections += (session.stats_.stage1_reflections - stats_.stage1_reflections);
    stats_.decls_applied += (session.stats_.decls_applied - stats_.decls_applied);
    return ok;
  }

  auto gram = compiled_grammar();
  if (!gram) return false;

  parse::CstNode *cst = gram->parse(rest);
  if (!cst) return false;
  if (opts.dump_cst) {
    std::fprintf(opts.diag_fp, "=== CST ===\n");
    parse::dump_cst(cst, opts.diag_fp);
  }

  syntax::ScopeAllocator scopes;
  syntax::SyntaxObj *stx = syntax::SyntaxObj::from_cst(cst);
  syntax::SyntaxObj *stx0 = stage0_.expand(stx, *gram, scopes);
  if (!stx0) return false;

  syntax::SyntaxObj *final_stx = stx0;
  if (opts.staged) {
    ifa_bridge::AnalysisAttrs attrs;
    if (!ifa_bridge::analyze_syntax(stx0, &prims_, &attrs)) return false;
    if (opts.dump_attrs) {
      std::fprintf(opts.diag_fp, "=== IFA AnalysisAttrs ===\n");
      dump_attrs_tree(stx0, attrs, opts.diag_fp);
    }
    final_stx = stage1_.expand(stx0, *gram, scopes, &attrs);
    if (!final_stx) return false;
  }

  if (opts.dump_expanded) {
    std::fprintf(opts.diag_fp, "=== Expanded SyntaxObj ===\n");
    syntax::dump_syntax(final_stx, opts.diag_fp);
  }

  return ifa_bridge::compile_and_jit_i64(final_stx, out_result, &prims_, out_info);
}

}  // namespace langwb
