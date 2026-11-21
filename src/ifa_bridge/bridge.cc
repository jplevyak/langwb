#include "ifa_bridge/bridge.h"

#include <cstdio>
#include <set>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <utility>
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

using RecordFieldMap = std::unordered_map<std::string, std::vector<std::string>>;

struct LowerCtx {
  Sym *enclosing_fn = nullptr;
  NodeSymMap *node_syms = nullptr;
  RecordFieldMap *record_fields = nullptr;
  int anon_counter = 0;
};

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

void collect_params(const syntax::SyntaxObj *node, std::vector<const syntax::SyntaxObj *> &out) {
  if (!node) return;
  if (node->is_terminal) {
    if (node->text != ",") out.push_back(node);
    return;
  }
  for (const syntax::SyntaxObj *child : node->children) {
    collect_params(child, out);
  }
}

void collect_fields(const syntax::SyntaxObj *node,
                    std::vector<std::pair<std::string, const syntax::SyntaxObj *>> &out) {
  if (!node) return;
  if (node->symbol == "field_init" && node->children.size() == 3) {
    out.emplace_back(node->children[0]->text, node->children[2]);
    return;
  }
  for (const syntax::SyntaxObj *child : node->children) {
    if (child->is_terminal && child->text == ",") continue;
    collect_fields(child, out);
  }
}

Sym *new_local(const LowerCtx &lctx, cchar *name = nullptr) {
  Sym *s = new_Sym(name);
  s->is_local = 1;
  s->nesting_depth = LOCALLY_NESTED;
  s->in = lctx.enclosing_fn;
  return s;
}

Sym *lower_expr(const syntax::SyntaxObj *node, Code **code, const Env &env, LowerCtx &lctx) {
  if (!node) return nullptr;

  auto record = [&](Sym *s) -> Sym * {
    if (s && lctx.node_syms && node->node_id != 0) {
      (*lctx.node_syms)[node->node_id] = s;
    }
    return s;
  };

  // Unwrap single-child non-terminal pass-through nodes.
  if (node->children.size() == 1 && !node->children[0]->is_terminal) {
    return record(lower_expr(node->children[0], code, env, lctx));
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
    return record(lower_expr(node->children[0], code, env, lctx));
  }

  // Parenthesized expression: '(' expr ')'
  if (node->children.size() == 3 && node->children[0]->text == "(" &&
      node->children[2]->text == ")") {
    return record(lower_expr(node->children[1], code, env, lctx));
  }

  // Record literal: '{' fields '}'
  if (node->children.size() == 3 && node->children[0]->text == "{" &&
      node->children[2]->text == "}") {
    std::vector<std::pair<std::string, const syntax::SyntaxObj *>> fields;
    collect_fields(node->children[1], fields);

    std::string rec_name = "__wb_rec_" + std::to_string(++lctx.anon_counter);
    Sym *rec_type = new_Sym(rec_name.c_str());
    rec_type->type_kind = Type_RECORD;
    rec_type->implements.add(sym_object);
    rec_type->specializes.add(sym_object);

    std::vector<Sym *> field_vals;
    field_vals.reserve(fields.size());
    std::vector<std::string> field_names_copy;
    field_names_copy.reserve(fields.size());
    for (const auto &[fname, fexpr] : fields) {
      Sym *fsym = new_Sym(fname.c_str());
      fsym->in = rec_type;
      rec_type->has.add(fsym);
      field_names_copy.push_back(fname);

      Sym *val = lower_expr(fexpr, code, env, lctx);
      if (!val) return nullptr;
      field_vals.push_back(val);
    }
    if (lctx.record_fields) {
      (*lctx.record_fields)[rec_name] = std::move(field_names_copy);
    }

    Sym *obj = new_local(lctx);
    if1_send(if1, code, 3, 1, sym_primitive, sym_new, rec_type, obj);
    for (size_t i = 0; i < fields.size(); ++i) {
      Sym *setter_res = new_local(lctx);
      if1_send(if1, code, 5, 1, sym_operator, obj, sym_setter,
               if1_make_symbol(if1, fields[i].first.c_str()), field_vals[i], setter_res);
    }
    return record(obj);
  }

  // Field access: expr '.' ident
  if (node->children.size() == 3 && node->children[1]->is_terminal &&
      node->children[1]->text == ".") {
    Sym *obj = lower_expr(node->children[0], code, env, lctx);
    if (!obj) return nullptr;
    Sym *res = new_local(lctx);
    if1_send(if1, code, 4, 1, sym_operator, obj, sym_period,
             if1_make_symbol(if1, node->children[2]->text.c_str()), res);
    return record(res);
  }

  // Binary operator: expr op expr
  if (node->children.size() == 3 && node->children[1]->is_terminal) {
    Sym *lhs = lower_expr(node->children[0], code, env, lctx);
    Sym *rhs = lower_expr(node->children[2], code, env, lctx);
    if (!lhs || !rhs) return nullptr;
    Sym *op = if1_make_symbol(if1, node->children[1]->text.c_str());
    Sym *res = new_local(lctx);
    if1_send(if1, code, 4, 1, sym_operator, lhs, op, rhs, res);
    return record(res);
  }

  // Call expression: ident '(' args ')'
  // Resolves `ident` hygienically in `env`: if bound, emits a user function/closure
  // SEND; otherwise emits a primitive SEND.
  if (node->children.size() == 4 && node->children[0]->is_terminal &&
      node->children[1]->text == "(" && node->children[3]->text == ")") {
    const syntax::SyntaxObj *callee_node = node->children[0];
    std::vector<const syntax::SyntaxObj *> arg_nodes;
    collect_call_args(node->children[2], arg_nodes);

    std::vector<Sym *> arg_syms;
    arg_syms.reserve(arg_nodes.size());
    for (const syntax::SyntaxObj *an : arg_nodes) {
      Sym *as = lower_expr(an, code, env, lctx);
      if (!as) return nullptr;
      arg_syms.push_back(as);
    }

    Sym *res = new_local(lctx);
    Code *send = if1_send1(if1, code);
    if (auto bound = env.resolve(callee_node->text, callee_node->scopes)) {
      if1_add_send_arg(if1, send, *bound);
    } else {
      Sym *prim_sym = if1_make_symbol(if1, callee_node->text.c_str());
      if1_add_send_arg(if1, send, sym_primitive);
      if1_add_send_arg(if1, send, prim_sym);
    }
    for (Sym *as : arg_syms) {
      if1_add_send_arg(if1, send, as);
    }
    if1_add_send_result(if1, send, res);
    return record(res);
  }

  // Lambda / closure expression: 'fn' '(' params ')' '=>' expr
  if (node->children.size() == 6 && node->children[0]->text == "fn" &&
      node->children[1]->text == "(" && node->children[3]->text == ")" &&
      node->children[4]->text == "=>") {
    std::vector<const syntax::SyntaxObj *> param_nodes;
    collect_params(node->children[2], param_nodes);

    std::string fn_name = "__wb_fn_" + std::to_string(++lctx.anon_counter);
    Sym *fn = new_Sym(fn_name.c_str());
    fn->cont = new_Sym();
    fn->cont->nesting_depth = LOCALLY_NESTED;
    fn->ret = new_Sym();
    fn->ret->is_local = 1;
    fn->ret->nesting_depth = LOCALLY_NESTED;
    fn->ret->in = fn;

    Env inner = env;
    std::vector<Sym *> closure_args;
    closure_args.reserve(1 + param_nodes.size());
    closure_args.push_back(fn);
    for (const syntax::SyntaxObj *pn : param_nodes) {
      Sym *ps = new_Sym(pn->text.c_str());
      ps->is_local = 1;
      ps->nesting_depth = LOCALLY_NESTED;
      ps->in = fn;
      closure_args.push_back(ps);
      inner.bind(pn->text, pn->scopes, ps);
    }

    Code *fn_code = nullptr;
    Sym *saved_enclosing = lctx.enclosing_fn;
    lctx.enclosing_fn = fn;
    Sym *body_sym = lower_expr(node->children[5], &fn_code, inner, lctx);
    lctx.enclosing_fn = saved_enclosing;
    if (!body_sym) return nullptr;

    if1_move(if1, &fn_code, body_sym, fn->ret);
    if1_send(if1, &fn_code, 4, 0, sym_primitive, sym_reply, fn->cont, fn->ret);
    if1_closure(if1, fn, fn_code, static_cast<int>(closure_args.size()), closure_args.data());
    return record(fn);
  }

  // Conditional expression: 'if' expr 'then' expr 'else' expr
  if (node->children.size() == 6 && node->children[0]->text == "if" &&
      node->children[2]->text == "then" && node->children[4]->text == "else") {
    Code *cond_code = nullptr;
    Code *then_code = nullptr;
    Code *else_code = nullptr;
    Sym *cond_sym = lower_expr(node->children[1], &cond_code, env, lctx);
    Sym *then_sym = lower_expr(node->children[3], &then_code, env, lctx);
    Sym *else_sym = lower_expr(node->children[5], &else_code, env, lctx);
    if (!cond_sym || !then_sym || !else_sym) return nullptr;

    Sym *res = new_local(lctx);
    if1_if(if1, code, cond_code, cond_sym, then_code, then_sym, else_code, else_sym, res);
    return record(res);
  }

  // Let expression: 'let' ident '=' expr 'in' expr
  if (node->children.size() == 6 && node->children[0]->text == "let" &&
      node->children[2]->text == "=" && node->children[4]->text == "in") {
    const syntax::SyntaxObj *binder = node->children[1];
    Sym *rhs = lower_expr(node->children[3], code, env, lctx);
    if (!rhs) return nullptr;
    Sym *bound = rhs;
    if (!rhs->is_fun) {
      bound = new_local(lctx, binder->text.c_str());
      if1_move(if1, code, rhs, bound);
    }

    Env inner = env;
    inner.bind(binder->text, binder->scopes, bound);
    return record(lower_expr(node->children[5], code, inner, lctx));
  }

  std::fprintf(stderr, "langwb: unsupported SyntaxObj shape '%s' with %zu children\n",
               node->symbol.c_str(), node->children.size());
  return nullptr;
}

NodeAttr extract_sym_attr(Sym *s, Fun *top, const RecordFieldMap *record_fields = nullptr) {
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
  std::vector<std::string> fields;
  std::set<std::string> seen_fields;
  bool saw_any_value = false;
  bool all_same_const = true;
  int64_t common_const = 0;

  auto collect_record_fields = [&](Sym *ty) {
    if (!ty || ty->type_kind != Type_RECORD) return;
    auto add_from = [&](Sym *rec) {
      for (int i = 0; i < rec->has.n; ++i) {
        if (rec->has[i] && rec->has[i]->name && seen_fields.insert(rec->has[i]->name).second) {
          fields.emplace_back(rec->has[i]->name);
        }
      }
    };
    add_from(ty);
    // Cloned records may have unread fields pruned from `ty->has`; fall back
    // to the original lowered record field list recorded during lowering.
    if (fields.empty() && ty->name && record_fields) {
      if (auto it = record_fields->find(ty->name); it != record_fields->end()) {
        for (const auto &fn : it->second) {
          if (seen_fields.insert(fn).second) fields.push_back(fn);
        }
      }
    }
  };

  auto inspect_var = [&](Var *v, Fun *owner_fun) {
    if (!v) return;
    if (v->type) {
      if (v->type->name) types.insert(v->type->name);
      collect_record_fields(v->type);
    }
    bool saw_av = false;
    for (EntrySet *es : owner_fun->ess) {
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
          collect_record_fields(cs->sym);
          collect_record_fields(cs->sym->type);
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

  // Search across all Funs (so attributes inside closures are also captured).
  for (Fun *f : pdb->fa->funs) {
    if (!f) continue;
    for (PNode *pn : f->fa_all_PNodes) {
      if (!pn) continue;
      for (int i = 0; i < pn->lvals.n; ++i) {
        if (pn->lvals[i] && pn->lvals[i]->sym == s) inspect_var(pn->lvals[i], f);
      }
      for (int i = 0; i < pn->rvals.n; ++i) {
        if (pn->rvals[i] && pn->rvals[i]->sym == s) inspect_var(pn->rvals[i], f);
      }
    }
  }

  for (const auto &t : types) attr.type_names.push_back(t);
  attr.field_names = std::move(fields);
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
  if1->partial_default = Partial_NEVER;
  init_default_builtin_types();
  new_builtin_global_variable(sym___main__, "__main__");

  PrimRegistry local_prims;
  PrimRegistry *reg = prims ? prims : &local_prims;
  ensure_wb_result_registered(reg);
  reg->install_into_if1();

  Code *code = nullptr;
  Env env;
  NodeSymMap node_syms;
  RecordFieldMap record_fields;
  LowerCtx lctx{.enclosing_fn = sym___main__, .node_syms = &node_syms, .record_fields = &record_fields};
  Sym *val = lower_expr(root, &code, env, lctx);
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
  if1->top = fn;

  finalize_types(if1);
  build_type_hierarchy();

  if (ifa_analyze("<langwb-stage0>") < 0) {
    PrimRegistry::set_active(nullptr);
    ifa_reset();
    return false;
  }

  Fun *top = if1->top ? if1->top->fun : nullptr;
  for (const auto &[node_id, sym] : node_syms) {
    out_attrs->set(node_id, extract_sym_attr(sym, top, &record_fields));
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
  if1->partial_default = Partial_NEVER;
  init_default_builtin_types();
  new_builtin_global_variable(sym___main__, "__main__");

  PrimRegistry local_prims;
  PrimRegistry *reg = prims ? prims : &local_prims;
  ensure_wb_result_registered(reg);
  reg->install_into_if1();

  Code *code = nullptr;
  Env env;
  LowerCtx lctx{.enclosing_fn = sym___main__, .node_syms = nullptr};
  Sym *val = lower_expr(root, &code, env, lctx);
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
  if1->top = fn;

  finalize_types(if1);
  build_type_hierarchy();
  if (std::getenv("LANGWB_DUMP_IF1")) {
    ifa_verbose = 3;
    if1_write(stderr, if1);
  }

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
