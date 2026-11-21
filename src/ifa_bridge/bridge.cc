#include "ifa_bridge/bridge.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <unistd.h>

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

Sym *lower_expr(const parse::CstNode *node, Code **code, Env &env) {
  if (!node) return nullptr;

  // Unwrap single-child pass-through nodes (e.g., program -> expr).
  if (node->symbol != "expr" && node->children.size() == 1) {
    return lower_expr(node->children[0], code, env);
  }

  // Integer literal: single terminal child matching digits.
  if (node->children.size() == 1 && node->children[0]->is_terminal) {
    const auto &tok = node->children[0]->text;
    if (!tok.empty() && (tok[0] >= '0' && tok[0] <= '9')) {
      return if1_const(if1, sym_int64, tok.c_str());
    }
    // Variable reference.
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

bool compile_and_jit_i64(const parse::CstNode *root, int64_t *out_result) {
  if (!root || !out_result) return false;

  auto *cb = new WbCallbacks();
  ifa_init(cb);
  init_default_builtin_types();
  new_builtin_global_variable(sym___main__, "__main__");

  // Register our visible host-reporting primitive so DCE preserves the value
  // and LLVM emits a direct call to _CG_wb_result(int64_t).
  cchar *wb_result_name = if1_cannonicalize_string(if1, "wb_result");
  RegisteredPrim *rp = prim_reg(wb_result_name, return_nil_transfer_function, nullptr, nullptr);
  rp->is_functional = 0;
  rp->is_visible = 1;

  Code *code = nullptr;
  Env env;
  Sym *val = lower_expr(root, &code, env);
  if (!val) {
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
    ifa_reset();
    return false;
  }
  ifa_optimize();

  FILE *null_fp = std::fopen("/dev/null", "w");
  llvm_codegen_print_ir(null_fp ? null_fp : stderr, pdb->fa, if1->top->fun, "<langwb>");
  if (null_fp) std::fclose(null_fp);

  int fds[2];
  if (::pipe(fds) != 0) {
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

  ifa_reset();

  if (status != 0 || nread != static_cast<ssize_t>(sizeof(result))) {
    return false;
  }
  *out_result = result;
  return true;
}

}  // namespace langwb::ifa_bridge
