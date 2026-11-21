#include "ifa_bridge/bridge.h"

#include <cstdio>
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

using Env = std::unordered_map<std::string, Sym *>;

void collect_call_args(const parse::CstNode *node, std::vector<const parse::CstNode *> &out) {
  if (!node) return;
  if (node->symbol == "expr") {
    out.push_back(node);
    return;
  }
  for (const parse::CstNode *child : node->children) {
    if (child->is_terminal && child->text == ",") continue;
    collect_call_args(child, out);
  }
}

Sym *lower_expr(const parse::CstNode *node, Code **code, Env &env) {
  if (!node) return nullptr;

  // Unwrap single-child pass-through nodes (e.g., program -> expr).
  if (node->symbol != "expr" && node->children.size() == 1) {
    return lower_expr(node->children[0], code, env);
  }

  // Integer literal or variable reference.
  if (node->children.size() == 1 && node->children[0]->is_terminal) {
    const auto &tok = node->children[0]->text;
    if (!tok.empty() && (tok[0] >= '0' && tok[0] <= '9')) {
      return if1_const(if1, sym_int64, tok.c_str());
    }
    auto it = env.find(tok);
    if (it != env.end()) {
      return it->second;
    }
    std::fprintf(stderr, "langwb: unbound identifier '%s'\n", tok.c_str());
    return nullptr;
  }

  // Parenthesized expression: '(' expr ')'
  if (node->children.size() == 3 && node->children[0]->text == "(" &&
      node->children[2]->text == ")") {
    return lower_expr(node->children[1], code, env);
  }

  // Binary operator: expr op expr
  if (node->children.size() == 3 && node->children[1]->is_terminal) {
    Sym *lhs = lower_expr(node->children[0], code, env);
    Sym *rhs = lower_expr(node->children[2], code, env);
    if (!lhs || !rhs) return nullptr;
    Sym *op = if1_make_symbol(if1, node->children[1]->text.c_str());
    Sym *res = new_Sym();
    if1_send(if1, code, 4, 1, sym_operator, lhs, op, rhs, res);
    return res;
  }

  // Primitive/function call: ident '(' args ')'
  if (node->children.size() == 4 && node->children[0]->is_terminal &&
      node->children[1]->text == "(" && node->children[3]->text == ")") {
    const std::string &callee = node->children[0]->text;
    std::vector<const parse::CstNode *> arg_nodes;
    collect_call_args(node->children[2], arg_nodes);

    std::vector<Sym *> arg_syms;
    arg_syms.reserve(arg_nodes.size());
    for (const parse::CstNode *an : arg_nodes) {
      Sym *as = lower_expr(an, code, env);
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
    return res;
  }

  // Let expression: 'let' ident '=' expr 'in' expr
  if (node->children.size() == 6 && node->children[0]->text == "let" &&
      node->children[2]->text == "=" && node->children[4]->text == "in") {
    const std::string &var_name = node->children[1]->text;
    Sym *rhs = lower_expr(node->children[3], code, env);
    if (!rhs) return nullptr;
    Sym *var = new_Sym(var_name.c_str());
    var->is_local = 1;
    if1_move(if1, code, rhs, var);

    Env inner = env;
    inner[var_name] = var;
    return lower_expr(node->children[5], code, inner);
  }

  std::fprintf(stderr, "langwb: unsupported CST shape '%s' with %zu children\n",
               node->symbol.c_str(), node->children.size());
  return nullptr;
}

void capture_root_const_info(Sym *val_sym, CompileInfo *out_info) {
  if (!val_sym || !out_info || !pdb || !pdb->fa) return;
  if (val_sym->is_constant && val_sym->imm.const_kind == IF1_NUM_KIND_INT) {
    out_info->has_inferred_const_i64 = true;
    out_info->inferred_const_i64 = val_sym->imm.v_int64;
    return;
  }
  // Inspect the converged AVar for val_sym in __main__'s EntrySet(s).
  Fun *top = if1->top ? if1->top->fun : nullptr;
  if (!top) return;
  for (PNode *pn : top->fa_all_PNodes) {
    if (!pn || pn->rvals.n < 3 || !pn->rvals[1] || !pn->rvals[1]->sym) continue;
    cchar *name = pn->rvals[1]->sym->name ? pn->rvals[1]->sym->name : pn->rvals[1]->sym->constant;
    if (!name || std::string_view(name) != "wb_result") continue;
    Var *v = pn->rvals[2];
    if (v && v->type && v->type->is_constant && v->type->imm.const_kind == IF1_NUM_KIND_INT) {
      out_info->has_inferred_const_i64 = true;
      out_info->inferred_const_i64 = v->type->imm.v_int64;
      return;
    }
    if (v && v->sym && v->sym->is_constant && v->sym->imm.const_kind == IF1_NUM_KIND_INT) {
      out_info->has_inferred_const_i64 = true;
      out_info->inferred_const_i64 = v->sym->imm.v_int64;
      return;
    }
    for (EntrySet *es : top->ess) {
      if (!es) continue;
      AVar *av = make_AVar(v, es);
      int64_t c = 0;
      if (try_get_const_i64(av, &c)) {
        out_info->has_inferred_const_i64 = true;
        out_info->inferred_const_i64 = c;
        return;
      }
    }
  }
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

bool compile_and_jit_i64(const parse::CstNode *root, int64_t *out_result, PrimRegistry *prims,
                         CompileInfo *out_info) {
  if (!root || !out_result) return false;
  if (out_info) *out_info = CompileInfo{};

  auto *cb = new WbCallbacks();
  ifa_init(cb);
  init_default_builtin_types();
  new_builtin_global_variable(sym___main__, "__main__");

  PrimRegistry local_prims;
  PrimRegistry *reg = prims ? prims : &local_prims;
  if (!reg->has("wb_result")) {
    reg->register_prim("wb_result", PrimitiveHandler{
                                        .transfer = [](PNode *pn, EntrySet *es) {
                                          prim_emit_result(pn, es, make_abstract_type(sym_void));
                                        },
                                        .is_functional = false,
                                        .is_visible = true,
                                    });
  }
  reg->install_into_if1();

  Code *code = nullptr;
  Env env;
  Sym *val = lower_expr(root, &code, env);
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

}  // namespace langwb::ifa_bridge
