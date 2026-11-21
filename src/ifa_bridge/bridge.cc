#include "ifa_bridge/bridge.h"

#include <cstdio>
#include <set>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <vector>

#include "llvm.h"

namespace {

// Pipe write-end shared across fork() by llvm_jit_execute so _CG_wb_result
// can return an exact 64-bit integer back to the parent langwb process.
int g_jit_result_fd = -1;

}  // namespace

extern "C" void _CG_gc_init() { GC_INIT(); }
extern "C" void _CG_set_argv(int /*argc*/, char ** /*argv*/) {}
extern "C" void _CG_event_loop_run() {}

extern "C" void _CG_wb_result(int64_t value) {
  if (g_jit_result_fd >= 0) {
    (void)::write(g_jit_result_fd, &value, sizeof(value));
  }
}

namespace langwb::ifa_bridge {

namespace {

using Env = syntax::BindingEnv<Sym *>;
using NodeSymMap = std::unordered_map<uint64_t, Sym *>;

void collect_call_args(const syntax::SyntaxObj *node,
                       std::vector<const syntax::SyntaxObj *> &out) {
  if (!node) return;
  if (node->symbol == "expr") {
    out.push_back(node);
    return;
  }
  for (const syntax::SyntaxObj *child : node->children) {
    if (child->is_terminal && child->text == ",") continue;
    collect_call_args(child, out);
  }
}

Sym *lower_expr(const syntax::SyntaxObj *node, Code **code, const Env &env,
                NodeSymMap *node_syms) {
  if (!node) return nullptr;

  auto record = [&](Sym *s) -> Sym * {
    if (s && node_syms && node->node_id != 0) {
      (*node_syms)[node->node_id] = s;
    }
    return s;
  };

  // Unwrap single-child non-terminal pass-through nodes.
  if (node->children.size() == 1 && !node->children[0]->is_terminal) {
    return record(lower_expr(node->children[0], code, env, node_syms));
  }

  // Direct terminal (identifier or integer literal).
  if (node->is_terminal) {
    const auto &tok = node->text;
    if (!tok.empty() && (tok[0] >= '0' && tok[0] <= '9')) {
      return record(if1_const(if1, sym_int64, tok.c_str()));
    }
    if (auto bound = env.resolve(tok, node->scopes)) {
      return record(*bound);
    }
    std::fprintf(stderr, "langwb: unbound identifier '%s'\n", tok.c_str());
    return nullptr;
  }

  // Integer literal or hygienic variable reference wrapped in a 1-child expr.
  if (node->children.size() == 1 && node->children[0]->is_terminal) {
    return record(lower_expr(node->children[0], code, env, node_syms));
  }

  // Parenthesized expression: '(' expr ')'
  if (node->children.size() == 3 && node->children[0]->text == "(" &&
      node->children[2]->text == ")") {
    return record(lower_expr(node->children[1], code, env, node_syms));
  }

  // Binary operator: expr op expr
  if (node->children.size() == 3 && node->children[1]->is_terminal) {
    Sym *lhs = lower_expr(node->children[0], code, env, node_syms);
    Sym *rhs = lower_expr(node->children[2], code, env, node_syms);
    if (!lhs || !rhs) return nullptr;
    Sym *op = if1_make_symbol(if1, node->children[1]->text.c_str());
    Sym *res = new_Sym();
    if1_send(if1, code, 4, 1, sym_operator, lhs, op, rhs, res);
    return record(res);
  }

  // Primitive/function call: ident '(' args ')'
  if (node->children.size() == 4 && node->children[0]->is_terminal &&
      node->children[1]->text == "(" && node->children[3]->text == ")") {
    const std::string &callee = node->children[0]->text;
    std::vector<const syntax::SyntaxObj *> arg_nodes;
    collect_call_args(node->children[2], arg_nodes);

    std::vector<Sym *> arg_syms;
    arg_syms.reserve(arg_nodes.size());
    for (const syntax::SyntaxObj *an : arg_nodes) {
      Sym *as = lower_expr(an, code, env, node_syms);
      if (!as) return nullptr;
      arg_syms.push_back(as);
    }

    Sym *prim_sym = if1_make_symbol(if1, callee.c_str());
    Sym *res = new_Sym();
    Code *send = if1_send1(if1, code);
    if1_add_send_arg(if1, send, sym_primitive);
    if1_add_send_arg(if1, send, prim_sym);
    for (Sym *as : arg_syms) {
      if1_add_send_arg(if1, send, as);
    }
    if1_add_send_result(if1, send, res);
    return record(res);
  }

  // Conditional expression: 'if' expr 'then' expr 'else' expr
  if (node->children.size() == 6 && node->children[0]->text == "if" &&
      node->children[2]->text == "then" && node->children[4]->text == "else") {
    Code *cond_code = nullptr;
    Code *then_code = nullptr;
    Code *else_code = nullptr;
    Sym *cond_sym = lower_expr(node->children[1], &cond_code, env, node_syms);
    Sym *then_sym = lower_expr(node->children[3], &then_code, env, node_syms);
    Sym *else_sym = lower_expr(node->children[5], &else_code, env, node_syms);
    if (!cond_sym || !then_sym || !else_sym) return nullptr;

    Sym *res = new_Sym();
    res->is_local = 1;
    if1_if(if1, code, cond_code, cond_sym, then_code, then_sym, else_code, else_sym, res);
    return record(res);
  }

  // Let expression: 'let' ident '=' expr 'in' expr
  if (node->children.size() == 6 && node->children[0]->text == "let" &&
      node->children[2]->text == "=" && node->children[4]->text == "in") {
    const syntax::SyntaxObj *binder = node->children[1];
    Sym *rhs = lower_expr(node->children[3], code, env, node_syms);
    if (!rhs) return nullptr;
    Sym *var = new_Sym(binder->text.c_str());
    var->is_local = 1;
    if1_move(if1, code, rhs, var);

    Env inner = env;
    inner.bind(binder->text, binder->scopes, var);
    return record(lower_expr(node->children[5], code, inner, node_syms));
  }

  std::fprintf(stderr, "langwb: unsupported SyntaxObj shape '%s' with %zu children\n",
               node->symbol.c_str(), node->children.size());
  return nullptr;
}

NodeAttr extract_sym_attr(Sym *s, Fun *top) {
  NodeAttr attr;
  if (!s) return attr;
  attr.analyzed = true;

  if (s->is_constant && s->imm.const_kind == IF1_NUM_KIND_INT) {
    attr.is_monomorphic = true;
    attr.has_const_i64 = true;
    attr.const_i64 = s->imm.v_int64;
    attr.type_names.push_back("int64");
    attr.contour_count = 1;
    return attr;
  }

  if (!top) return attr;

  std::set<std::string> types;
  bool saw_any_value = false;
  bool all_same_const = true;
  int64_t common_const = 0;

  auto inspect_var = [&](Var *v) {
    if (!v) return;
    if (v->type && v->type->name) {
      types.insert(v->type->name);
    }
    bool saw_av = false;
    for (EntrySet *es : top->ess) {
      if (!es) continue;
      AVar *av = make_AVar(v, es);
      if (!av || !av->out || av->out->n == 0) continue;
      saw_av = true;
      attr.contour_count++;
      saw_any_value = true;
      int64_t c = 0;
      if (try_get_const_i64(av, &c)) {
        if (!attr.has_const_i64) {
          common_const = c;
          attr.has_const_i64 = true;
        } else if (common_const != c) {
          all_same_const = false;
        }
      } else {
        all_same_const = false;
      }
      for (int i = 0; i < av->out->n; ++i) {
        CreationSet *cs = av->out->v[i];
        if (cs && cs->sym) {
          if (cs->sym->type && cs->sym->type->name) {
            types.insert(cs->sym->type->name);
          } else if (cs->sym->name) {
            types.insert(cs->sym->name);
          }
        }
      }
    }
    if (!saw_av && v->type) {
      saw_any_value = true;
      if (v->type->is_constant && v->type->imm.const_kind == IF1_NUM_KIND_INT) {
        if (!attr.has_const_i64) {
          common_const = v->type->imm.v_int64;
          attr.has_const_i64 = true;
        } else if (common_const != v->type->imm.v_int64) {
          all_same_const = false;
        }
      } else {
        all_same_const = false;
      }
    }
  };

  for (PNode *pn : top->fa_all_PNodes) {
    if (!pn) continue;
    for (int i = 0; i < pn->lvals.n; ++i) {
      if (pn->lvals[i] && pn->lvals[i]->sym == s) inspect_var(pn->lvals[i]);
    }
    for (int i = 0; i < pn->rvals.n; ++i) {
      if (pn->rvals[i] && pn->rvals[i]->sym == s) inspect_var(pn->rvals[i]);
    }
  }

  for (const auto &t : types) attr.type_names.push_back(t);
  attr.is_monomorphic = (attr.type_names.size() == 1);
  if (!saw_any_value || !all_same_const) {
    attr.has_const_i64 = false;
  } else if (attr.has_const_i64) {
    attr.const_i64 = common_const;
  }
  return attr;
}

void ensure_wb_result_registered(PrimRegistry *reg) {
  if (!reg->has("wb_result")) {
    reg->register_prim("wb_result", PrimitiveHandler{
                                        .transfer = [](PNode *pn, EntrySet *es) {
                                          prim_emit_result(pn, es, make_abstract_type(sym_void));
                                        },
                                        .is_functional = false,
                                        .is_visible = true,
                                    });
  }
}

void capture_root_const_info(Sym *val_sym, CompileInfo *out_info) {
  if (!val_sym || !out_info || !pdb || !pdb->fa) return;
  Fun *top = if1->top ? if1->top->fun : nullptr;
  NodeAttr attr = extract_sym_attr(val_sym, top);
  out_info->has_inferred_const_i64 = attr.has_const_i64;
  out_info->inferred_const_i64 = attr.const_i64;
}

}  // namespace

IFASymbol *WbSymbol::copy() {
  auto *s = new WbSymbol();
  s->filename = filename;
  s->line_no = line_no;
  s->col_no = col_no;
  s->sym = new Sym();
  if1_register_sym(if1, s->sym);
  s->sym->asymbol = s;
  return s;
}

IFAAST *WbAST::copy_tree(ASTCopyContext *context) { return copy_node(context); }

IFAAST *WbAST::copy_node(ASTCopyContext * /*context*/) {
  auto *a = new WbAST();
  a->filename = filename;
  a->line_no = line_no;
  a->col_no = col_no;
  a->sym_ptr = sym_ptr;
  return a;
}

Sym *WbCallbacks::new_Sym(cchar *name) {
  auto *ws = new WbSymbol();
  Sym *s = new Sym();
  if1_register_sym(if1, s, name);
  s->asymbol = ws;
  ws->sym = s;
  return s;
}

bool analyze_syntax(const syntax::SyntaxObj *root, PrimRegistry *prims, AnalysisAttrs *out_attrs) {
  if (!root || !out_attrs) return false;

  auto *cb = new WbCallbacks();
  ifa_init(cb);
  init_default_builtin_types();
  new_builtin_global_variable(sym___main__, "__main__");

  PrimRegistry local_prims;
  PrimRegistry *reg = prims ? prims : &local_prims;
  ensure_wb_result_registered(reg);
  reg->install_into_if1();

  Code *code = nullptr;
  Env env;
  NodeSymMap node_syms;
  Sym *val = lower_expr(root, &code, env, &node_syms);
  if (!val) {
    PrimRegistry::set_active(nullptr);
    ifa_reset();
    return false;
  }

  Sym *prim_name = if1_make_symbol(if1, "wb_result");
  Sym *discard = new_Sym();
  if1_send(if1, &code, 3, 1, sym_primitive, prim_name, val, discard);

  Sym *fn = sym___main__;
  fn->cont = new_Sym();
  fn->cont->var = new Var(fn->cont);
  fn->ret = sym_nil;
  if1_send(if1, &code, 4, 0, sym_primitive, sym_reply, fn->cont, fn->ret);
  if1_closure(if1, fn, code, 1, &fn);

  finalize_types(if1);
  build_type_hierarchy();

  if (ifa_analyze("<langwb-stage0>") < 0) {
    PrimRegistry::set_active(nullptr);
    ifa_reset();
    return false;
  }

  Fun *top = if1->top ? if1->top->fun : nullptr;
  for (const auto &[node_id, sym] : node_syms) {
    out_attrs->set(node_id, extract_sym_attr(sym, top));
  }

  PrimRegistry::set_active(nullptr);
  ifa_reset();
  return true;
}

bool compile_and_jit_i64(const syntax::SyntaxObj *root, int64_t *out_result, PrimRegistry *prims,
                         CompileInfo *out_info) {
  if (!root || !out_result) return false;
  if (out_info) *out_info = CompileInfo{};

  auto *cb = new WbCallbacks();
  ifa_init(cb);
  init_default_builtin_types();
  new_builtin_global_variable(sym___main__, "__main__");

  PrimRegistry local_prims;
  PrimRegistry *reg = prims ? prims : &local_prims;
  ensure_wb_result_registered(reg);
  reg->install_into_if1();

  Code *code = nullptr;
  Env env;
  Sym *val = lower_expr(root, &code, env, nullptr);
  if (!val) {
    PrimRegistry::set_active(nullptr);
    ifa_reset();
    return false;
  }

  Sym *prim_name = if1_make_symbol(if1, "wb_result");
  Sym *discard = new_Sym();
  if1_send(if1, &code, 3, 1, sym_primitive, prim_name, val, discard);

  Sym *fn = sym___main__;
  fn->cont = new_Sym();
  fn->cont->var = new Var(fn->cont);
  fn->ret = sym_nil;
  if1_send(if1, &code, 4, 0, sym_primitive, sym_reply, fn->cont, fn->ret);
  if1_closure(if1, fn, code, 1, &fn);

  finalize_types(if1);
  build_type_hierarchy();

  if (ifa_analyze("<langwb>") < 0) {
    PrimRegistry::set_active(nullptr);
    ifa_reset();
    return false;
  }
  capture_root_const_info(val, out_info);
  ifa_optimize();

  FILE *null_fp = std::fopen("/dev/null", "w");
  llvm_codegen_print_ir(null_fp ? null_fp : stderr, pdb->fa, if1->top->fun, "<langwb>");
  if (null_fp) std::fclose(null_fp);

  int fds[2];
  if (::pipe(fds) != 0) {
    PrimRegistry::set_active(nullptr);
    ifa_reset();
    return false;
  }
  g_jit_result_fd = fds[1];
  char arg0[] = "langwb";
  char *argv[] = {arg0, nullptr};
  int status = llvm_jit_execute(1, argv);
  g_jit_result_fd = -1;
  ::close(fds[1]);

  int64_t result = 0;
  ssize_t nread = ::read(fds[0], &result, sizeof(result));
  ::close(fds[0]);

  PrimRegistry::set_active(nullptr);
  ifa_reset();

  if (status != 0 || nread != static_cast<ssize_t>(sizeof(result))) {
    return false;
  }
  *out_result = result;
  return true;
}

bool compile_and_jit_i64(const parse::CstNode *root, int64_t *out_result, PrimRegistry *prims,
                         CompileInfo *out_info) {
  return compile_and_jit_i64(syntax::SyntaxObj::from_cst(root), out_result, prims, out_info);
}

bool compile_staged_i64(syntax::SyntaxObj *root, const parse::DynamicGrammar &gram,
                        syntax::ScopeAllocator &scopes, const macro::MacroExpander &stage0_macros,
                        const macro::MacroExpander &stage1_macros, PrimRegistry *prims,
                        int64_t *out_result, CompileInfo *out_info) {
  if (!root || !out_result) return false;

  // Stage 0: syntax-only hygienic macro expansion.
  syntax::SyntaxObj *stx0 = stage0_macros.expand(root, gram, scopes);
  if (!stx0) return false;

  // Analysis pass: run IFA on Stage-0 output and snapshot attributes before ifa_reset().
  AnalysisAttrs attrs;
  if (!analyze_syntax(stx0, prims, &attrs)) return false;

  // Stage 1: attribute-driven macro expansion using IFA's AnalysisAttrs.
  syntax::SyntaxObj *stx1 = stage1_macros.expand(stx0, gram, scopes, &attrs);
  if (!stx1) return false;

  // Final pass: re-analyze, optimize, emit LLVM IR, and execute via LLJIT.
  return compile_and_jit_i64(stx1, out_result, prims, out_info);
}

}  // namespace langwb::ifa_bridge
