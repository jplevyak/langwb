#include "ifa_bridge/bridge.h"

#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

#include "llvm.h"

namespace {

// Pipe write-end shared across fork() by llvm_jit_execute, or direct in-process
// result slot used by llvm_jit_execute_in_process (Phase-1 macros).
int g_jit_result_fd = -1;
bool g_in_process_mode = false;
bool g_in_process_has_result = false;
int64_t g_in_process_result = 0;

}  // namespace

extern "C" void _CG_gc_init() { GC_INIT(); }
extern "C" void _CG_set_argv(int /*argc*/, char ** /*argv*/) {}
extern "C" void _CG_event_loop_run() {}

extern "C" void _CG_wb_result(int64_t value) {
  if (g_in_process_mode) {
    g_in_process_result = value;
    g_in_process_has_result = true;
    return;
  }
  if (g_jit_result_fd >= 0) {
    (void)::write(g_jit_result_fd, &value, sizeof(value));
  }
}

namespace langwb::ifa_bridge {

namespace {

using Env = syntax::BindingEnv<Sym *>;
using ResolveEnv = syntax::BindingEnv<const syntax::SyntaxObj *>;
using NodeSymMap = std::unordered_map<uint64_t, Sym *>;

using RecordFieldMap = std::unordered_map<std::string, std::vector<std::string>>;

struct ResolutionInfo {
  std::unordered_map<uint64_t, const syntax::SyntaxObj *> ref_to_binder;
  std::unordered_map<uint64_t, std::string> prim_calls;
};

struct LowerCtx {
  Sym *enclosing_fn = nullptr;
  NodeSymMap *node_syms = nullptr;
  RecordFieldMap *record_fields = nullptr;
  const ResolutionInfo *resolved = nullptr;
  int anon_counter = 0;
};

void print_origin_suffix(const syntax::SyntaxObj *node) {
  if (node && !node->origin_macro.empty()) {
    std::fprintf(stderr, " (expanded from '%s' at %d:%d)", node->origin_macro.c_str(),
                 node->origin_line, node->origin_col);
  }
  std::fprintf(stderr, "\n");
}

const syntax::SyntaxObj *unwrap_ident_node(const syntax::SyntaxObj *node) {
  while (node && !node->is_terminal && node->children.size() == 1 &&
         (node->core.kind == parse::CoreFormKind::Ident ||
          node->core.kind == parse::CoreFormKind::PassThrough ||
          node->core.kind == parse::CoreFormKind::None)) {
    node = node->children[0];
  }
  return (node && node->is_terminal) ? node : nullptr;
}

const syntax::SyntaxObj *unwrap_lambda_node(const syntax::SyntaxObj *node) {
  while (node) {
    if (node->core.kind == parse::CoreFormKind::Lambda) return node;
    if (node->core.kind == parse::CoreFormKind::Paren && node->core.inner >= 0 &&
        static_cast<size_t>(node->core.inner) < node->children.size()) {
      node = node->children[static_cast<size_t>(node->core.inner)];
      continue;
    }
    if (node->core.kind == parse::CoreFormKind::PassThrough && node->children.size() == 1) {
      node = node->children[0];
      continue;
    }
    break;
  }
  return nullptr;
}

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

bool resolve_node(const syntax::SyntaxObj *node, const ResolveEnv &env, const PrimRegistry *prims,
                  ResolutionInfo &out) {
  if (!node) return false;
  using parse::CoreFormKind;

  switch (node->core.kind) {
    case CoreFormKind::PassThrough:
      if (node->children.size() == 1) {
        return resolve_node(node->children[0], env, prims, out);
      }
      break;

    case CoreFormKind::Paren:
      if (node->core.inner >= 0 && static_cast<size_t>(node->core.inner) < node->children.size()) {
        return resolve_node(node->children[static_cast<size_t>(node->core.inner)], env, prims, out);
      }
      break;

    case CoreFormKind::Datum:
    case CoreFormKind::String:
      return true;

    case CoreFormKind::Ident: {
      const syntax::SyntaxObj *id = unwrap_ident_node(node);
      if (!id) return false;
      if (auto bound = env.resolve(id->text, id->scopes)) {
        out.ref_to_binder[node->node_id] = *bound;
        out.ref_to_binder[id->node_id] = *bound;
        return true;
      }
      std::fprintf(stderr, "langwb: %d:%d: unbound identifier '%s'", id->line, id->col,
                   id->text.c_str());
      print_origin_suffix(id);
      return false;
    }

    case CoreFormKind::BinOp: {
      size_t li = static_cast<size_t>(node->core.lhs);
      size_t ri = static_cast<size_t>(node->core.rhs);
      if (node->core.lhs >= 0 && node->core.rhs >= 0 && li < node->children.size() &&
          ri < node->children.size()) {
        return resolve_node(node->children[li], env, prims, out) &&
               resolve_node(node->children[ri], env, prims, out);
      }
      break;
    }

    case CoreFormKind::If: {
      size_t ti = static_cast<size_t>(node->core.test);
      size_t theni = static_cast<size_t>(node->core.then_b);
      size_t elsei = static_cast<size_t>(node->core.else_b);
      if (node->core.test >= 0 && node->core.then_b >= 0 && node->core.else_b >= 0 &&
          ti < node->children.size() && theni < node->children.size() &&
          elsei < node->children.size()) {
        return resolve_node(node->children[ti], env, prims, out) &&
               resolve_node(node->children[theni], env, prims, out) &&
               resolve_node(node->children[elsei], env, prims, out);
      }
      break;
    }

    case CoreFormKind::Record: {
      if (node->core.fields >= 0 &&
          static_cast<size_t>(node->core.fields) < node->children.size()) {
        std::vector<std::pair<std::string, const syntax::SyntaxObj *>> fields;
        collect_fields(node->children[static_cast<size_t>(node->core.fields)], fields);
        for (const auto &[fname, fexpr] : fields) {
          if (!resolve_node(fexpr, env, prims, out)) return false;
        }
        return true;
      }
      break;
    }

    case CoreFormKind::Field: {
      if (node->core.obj >= 0 && static_cast<size_t>(node->core.obj) < node->children.size()) {
        return resolve_node(node->children[static_cast<size_t>(node->core.obj)], env, prims, out);
      }
      break;
    }

    case CoreFormKind::Let: {
      size_t bi = static_cast<size_t>(node->core.bind);
      size_t ri = static_cast<size_t>(node->core.rhs);
      size_t bodyi = static_cast<size_t>(node->core.body);
      if (node->core.bind >= 0 && node->core.rhs >= 0 && node->core.body >= 0 &&
          bi < node->children.size() && ri < node->children.size() &&
          bodyi < node->children.size()) {
        if (!resolve_node(node->children[ri], env, prims, out)) return false;
        const syntax::SyntaxObj *binder = unwrap_ident_node(node->children[bi]);
        if (!binder) return false;
        ResolveEnv inner = env;
        inner.bind(binder->text, binder->scopes, binder);
        return resolve_node(node->children[bodyi], inner, prims, out);
      }
      break;
    }

    case CoreFormKind::LetRec: {
      size_t bi = static_cast<size_t>(node->core.bind);
      size_t ri = static_cast<size_t>(node->core.rhs);
      size_t bodyi = static_cast<size_t>(node->core.body);
      if (node->core.bind >= 0 && node->core.rhs >= 0 && node->core.body >= 0 &&
          bi < node->children.size() && ri < node->children.size() &&
          bodyi < node->children.size()) {
        const syntax::SyntaxObj *binder = unwrap_ident_node(node->children[bi]);
        if (!binder) return false;
        ResolveEnv inner = env;
        inner.bind(binder->text, binder->scopes, binder);
        return resolve_node(node->children[ri], inner, prims, out) &&
               resolve_node(node->children[bodyi], inner, prims, out);
      }
      break;
    }

    case CoreFormKind::Lambda: {
      size_t pi = static_cast<size_t>(node->core.bind);
      size_t bodyi = static_cast<size_t>(node->core.body);
      if (node->core.bind >= 0 && node->core.body >= 0 && pi < node->children.size() &&
          bodyi < node->children.size()) {
        std::vector<const syntax::SyntaxObj *> params;
        collect_params(node->children[pi], params);
        ResolveEnv inner = env;
        for (const syntax::SyntaxObj *p : params) {
          inner.bind(p->text, p->scopes, p);
        }
        return resolve_node(node->children[bodyi], inner, prims, out);
      }
      break;
    }

    case CoreFormKind::App: {
      if (node->core.fn < 0 || static_cast<size_t>(node->core.fn) >= node->children.size()) break;
      const syntax::SyntaxObj *fn_node = node->children[static_cast<size_t>(node->core.fn)];
      if (const syntax::SyntaxObj *id = unwrap_ident_node(fn_node)) {
        if (auto bound = env.resolve(id->text, id->scopes)) {
          out.ref_to_binder[fn_node->node_id] = *bound;
          out.ref_to_binder[id->node_id] = *bound;
        } else if (prims && prims->has(id->text)) {
          out.prim_calls[node->node_id] = id->text;
        } else {
          std::fprintf(stderr, "langwb: %d:%d: unbound identifier '%s'", id->line, id->col,
                       id->text.c_str());
          print_origin_suffix(id);
          return false;
        }
      } else {
        if (!resolve_node(fn_node, env, prims, out)) return false;
      }
      if (node->core.args >= 0 && static_cast<size_t>(node->core.args) < node->children.size()) {
        std::vector<const syntax::SyntaxObj *> args;
        collect_call_args(node->children[static_cast<size_t>(node->core.args)], args);
        for (const syntax::SyntaxObj *a : args) {
          if (!resolve_node(a, env, prims, out)) return false;
        }
      }
      return true;
    }

    case CoreFormKind::Seq: {
      std::vector<const syntax::SyntaxObj *> stmts;
      if (node->core.inner >= 0 && static_cast<size_t>(node->core.inner) < node->children.size()) {
        std::function<void(const syntax::SyntaxObj *)> gather = [&](const syntax::SyntaxObj *n) {
          if (!n || n->is_terminal) return;
          if (n->symbol == "stmt" || n->core.kind == CoreFormKind::Define) {
            stmts.push_back(n);
            return;
          }
          for (const syntax::SyntaxObj *c : n->children) {
            if (c && c->is_terminal && c->text == ";") continue;
            gather(c);
          }
        };
        gather(node->children[static_cast<size_t>(node->core.inner)]);
      } else {
        for (const syntax::SyntaxObj *c : node->children) {
          if (c) stmts.push_back(c);
        }
      }

      // Pass 1: register all `Define` binders in block scope.
      ResolveEnv inner = env;
      for (const syntax::SyntaxObj *s : stmts) {
        const syntax::SyntaxObj *u = s;
        while (u && u->core.kind == CoreFormKind::PassThrough && u->children.size() == 1) {
          u = u->children[0];
        }
        if (u && u->core.kind == CoreFormKind::Define && u->core.bind >= 0 &&
            static_cast<size_t>(u->core.bind) < u->children.size()) {
          const syntax::SyntaxObj *binder =
              unwrap_ident_node(u->children[static_cast<size_t>(u->core.bind)]);
          if (!binder) return false;
          inner.bind(binder->text, binder->scopes, binder);
        }
      }
      // Pass 2: resolve all `Define` RHSs and expression statements.
      for (const syntax::SyntaxObj *s : stmts) {
        const syntax::SyntaxObj *u = s;
        while (u && u->core.kind == CoreFormKind::PassThrough && u->children.size() == 1) {
          u = u->children[0];
        }
        if (u && u->core.kind == CoreFormKind::Define && u->core.rhs >= 0 &&
            static_cast<size_t>(u->core.rhs) < u->children.size()) {
          if (!resolve_node(u->children[static_cast<size_t>(u->core.rhs)], inner, prims, out)) {
            return false;
          }
        } else {
          if (!resolve_node(u, inner, prims, out)) return false;
        }
      }
      return true;
    }

    case CoreFormKind::Define:
      std::fprintf(stderr, "langwb: %d:%d: 'def' is only allowed inside a sequence block",
                   node->line, node->col);
      print_origin_suffix(node);
      return false;

    case CoreFormKind::None:
      break;
  }

  // Fallback for terminal nodes directly spliced into templates (e.g. unhygienic_ident).
  if (node->is_terminal) {
    const auto &tok = node->text;
    if ((tok.size() >= 2 && tok.front() == '"' && tok.back() == '"') ||
        (!tok.empty() && tok[0] >= '0' && tok[0] <= '9')) {
      return true;
    }
    if (auto bound = env.resolve(tok, node->scopes)) {
      out.ref_to_binder[node->node_id] = *bound;
      return true;
    }
    std::fprintf(stderr, "langwb: %d:%d: unbound identifier '%s'", node->line, node->col,
                 tok.c_str());
    print_origin_suffix(node);
    return false;
  }

  // Unwrap single-child non-terminal wrapper if untagged (e.g. `program -> expr`).
  if (node->children.size() == 1 && !node->children[0]->is_terminal) {
    return resolve_node(node->children[0], env, prims, out);
  }

  std::fprintf(stderr, "langwb: %d:%d: unexpanded syntax form '%s' (no macro or core(...) rule)",
               node->line, node->col, node->symbol.c_str());
  print_origin_suffix(node);
  return false;
}

Sym *new_local(const LowerCtx &lctx, cchar *name = nullptr) {
  Sym *s = new_Sym(name);
  s->is_local = 1;
  s->nesting_depth = LOCALLY_NESTED;
  s->in = lctx.enclosing_fn;
  return s;
}

Sym *lower_expr(const syntax::SyntaxObj *node, Code **code, const Env &env, LowerCtx &lctx);

Sym *lower_lambda(const syntax::SyntaxObj *node, const Env &env, LowerCtx &lctx,
                  Sym *prealloc_fn = nullptr) {
  std::vector<const syntax::SyntaxObj *> param_nodes;
  collect_params(node->children[static_cast<size_t>(node->core.bind)], param_nodes);

  Sym *fn = prealloc_fn;
  if (!fn) {
    std::string fn_name = "__wb_fn_" + std::to_string(++lctx.anon_counter);
    fn = new_Sym(fn_name.c_str());
  }
  fn->in = lctx.enclosing_fn;
  fn->nesting_depth = LOCALLY_NESTED;
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
  Sym *body_sym =
      lower_expr(node->children[static_cast<size_t>(node->core.body)], &fn_code, inner, lctx);
  lctx.enclosing_fn = saved_enclosing;
  if (!body_sym) return nullptr;

  if1_move(if1, &fn_code, body_sym, fn->ret);
  if1_send(if1, &fn_code, 4, 0, sym_primitive, sym_reply, fn->cont, fn->ret);
  if1_closure(if1, fn, fn_code, static_cast<int>(closure_args.size()), closure_args.data());
  return fn;
}

Sym *lower_expr(const syntax::SyntaxObj *node, Code **code, const Env &env, LowerCtx &lctx) {
  if (!node) return nullptr;

  auto record = [&](Sym *s) -> Sym * {
    if (s && lctx.node_syms && node->node_id != 0) {
      (*lctx.node_syms)[node->node_id] = s;
    }
    return s;
  };

  using parse::CoreFormKind;

  switch (node->core.kind) {
    case CoreFormKind::PassThrough:
      if (node->children.size() == 1) {
        return record(lower_expr(node->children[0], code, env, lctx));
      }
      break;

    case CoreFormKind::Paren:
      return record(
          lower_expr(node->children[static_cast<size_t>(node->core.inner)], code, env, lctx));

    case CoreFormKind::Datum: {
      const syntax::SyntaxObj *t =
          (!node->is_terminal && node->children.size() == 1) ? node->children[0] : node;
      return record(if1_const(if1, sym_int64, t->text.c_str()));
    }

    case CoreFormKind::String: {
      const syntax::SyntaxObj *t =
          (!node->is_terminal && node->children.size() == 1) ? node->children[0] : node;
      std::string unquoted =
          (t->text.size() >= 2 && t->text.front() == '"' && t->text.back() == '"')
              ? t->text.substr(1, t->text.size() - 2)
              : t->text;
      return record(if1_const(if1, sym_string, unquoted.c_str(), nullptr, nullptr,
                              static_cast<int>(unquoted.size())));
    }

    case CoreFormKind::Ident: {
      const syntax::SyntaxObj *id = unwrap_ident_node(node);
      if (!id) return nullptr;
      if (auto bound = env.resolve(id->text, id->scopes)) {
        return record(*bound);
      }
      std::fprintf(stderr, "langwb: %d:%d: unbound identifier '%s'", id->line, id->col,
                   id->text.c_str());
      print_origin_suffix(id);
      return nullptr;
    }

    case CoreFormKind::Record: {
      std::vector<std::pair<std::string, const syntax::SyntaxObj *>> fields;
      collect_fields(node->children[static_cast<size_t>(node->core.fields)], fields);

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

    case CoreFormKind::Field: {
      Sym *obj = lower_expr(node->children[static_cast<size_t>(node->core.obj)], code, env, lctx);
      if (!obj) return nullptr;
      const syntax::SyntaxObj *fld =
          unwrap_ident_node(node->children[static_cast<size_t>(node->core.field)]);
      if (!fld) return nullptr;
      Sym *res = new_local(lctx);
      if1_send(if1, code, 4, 1, sym_operator, obj, sym_period,
               if1_make_symbol(if1, fld->text.c_str()), res);
      return record(res);
    }

    case CoreFormKind::BinOp: {
      Sym *lhs = lower_expr(node->children[static_cast<size_t>(node->core.lhs)], code, env, lctx);
      Sym *rhs = lower_expr(node->children[static_cast<size_t>(node->core.rhs)], code, env, lctx);
      if (!lhs || !rhs) return nullptr;
      Sym *op = if1_make_symbol(if1, node->children[static_cast<size_t>(node->core.op)]->text.c_str());
      Sym *res = new_local(lctx);
      if1_send(if1, code, 4, 1, sym_operator, lhs, op, rhs, res);
      return record(res);
    }

    case CoreFormKind::App: {
      std::vector<const syntax::SyntaxObj *> arg_nodes;
      if (node->core.args >= 0 && static_cast<size_t>(node->core.args) < node->children.size()) {
        collect_call_args(node->children[static_cast<size_t>(node->core.args)], arg_nodes);
      }

      auto pit = lctx.resolved ? lctx.resolved->prim_calls.find(node->node_id)
                               : std::unordered_map<uint64_t, std::string>::const_iterator{};
      bool is_prim = (lctx.resolved && pit != lctx.resolved->prim_calls.end());
      Sym *callee_sym = nullptr;
      if (!is_prim) {
        callee_sym =
            lower_expr(node->children[static_cast<size_t>(node->core.fn)], code, env, lctx);
        if (!callee_sym) return nullptr;
      }

      std::vector<Sym *> arg_syms;
      arg_syms.reserve(arg_nodes.size());
      for (const syntax::SyntaxObj *an : arg_nodes) {
        Sym *as = lower_expr(an, code, env, lctx);
        if (!as) return nullptr;
        arg_syms.push_back(as);
      }

      Sym *res = new_local(lctx);
      Code *send = if1_send1(if1, code);
      if (is_prim) {
        Sym *prim_sym = if1_make_symbol(if1, pit->second.c_str());
        if1_add_send_arg(if1, send, sym_primitive);
        if1_add_send_arg(if1, send, prim_sym);
      } else {
        if1_add_send_arg(if1, send, callee_sym);
      }
      for (Sym *as : arg_syms) {
        if1_add_send_arg(if1, send, as);
      }
      if1_add_send_result(if1, send, res);
      return record(res);
    }

    case CoreFormKind::Lambda:
      return record(lower_lambda(node, env, lctx));

    case CoreFormKind::If: {
      Code *cond_code = nullptr;
      Code *then_code = nullptr;
      Code *else_code = nullptr;
      Sym *cond_sym =
          lower_expr(node->children[static_cast<size_t>(node->core.test)], &cond_code, env, lctx);
      Sym *then_sym =
          lower_expr(node->children[static_cast<size_t>(node->core.then_b)], &then_code, env, lctx);
      Sym *else_sym =
          lower_expr(node->children[static_cast<size_t>(node->core.else_b)], &else_code, env, lctx);
      if (!cond_sym || !then_sym || !else_sym) return nullptr;

      Sym *res = new_local(lctx);
      if1_if(if1, code, cond_code, cond_sym, then_code, then_sym, else_code, else_sym, res);
      return record(res);
    }

    case CoreFormKind::Let: {
      const syntax::SyntaxObj *binder =
          unwrap_ident_node(node->children[static_cast<size_t>(node->core.bind)]);
      if (!binder) return nullptr;
      Sym *rhs = lower_expr(node->children[static_cast<size_t>(node->core.rhs)], code, env, lctx);
      if (!rhs) return nullptr;
      Sym *bound = rhs;
      if (!rhs->is_fun) {
        bound = new_local(lctx, binder->text.c_str());
        if1_move(if1, code, rhs, bound);
      }
      Env inner = env;
      inner.bind(binder->text, binder->scopes, bound);
      return record(
          lower_expr(node->children[static_cast<size_t>(node->core.body)], code, inner, lctx));
    }

    case CoreFormKind::LetRec: {
      const syntax::SyntaxObj *binder =
          unwrap_ident_node(node->children[static_cast<size_t>(node->core.bind)]);
      if (!binder) return nullptr;
      const syntax::SyntaxObj *rhs_node = node->children[static_cast<size_t>(node->core.rhs)];
      const syntax::SyntaxObj *lam_node = unwrap_lambda_node(rhs_node);
      Env inner = env;
      if (lam_node) {
        std::string fn_name =
            "__wb_recfn_" + binder->text + "_" + std::to_string(++lctx.anon_counter);
        Sym *fn = new_Sym(fn_name.c_str());
        inner.bind(binder->text, binder->scopes, fn);
        if (!lower_lambda(lam_node, inner, lctx, fn)) return nullptr;
      } else {
        Sym *bound = new_local(lctx, binder->text.c_str());
        inner.bind(binder->text, binder->scopes, bound);
        Sym *rhs = lower_expr(rhs_node, code, inner, lctx);
        if (!rhs) return nullptr;
        if1_move(if1, code, rhs, bound);
      }
      return record(
          lower_expr(node->children[static_cast<size_t>(node->core.body)], code, inner, lctx));
    }

    case CoreFormKind::Seq: {
      std::vector<const syntax::SyntaxObj *> stmts;
      if (node->core.inner >= 0 && static_cast<size_t>(node->core.inner) < node->children.size()) {
        std::function<void(const syntax::SyntaxObj *)> gather = [&](const syntax::SyntaxObj *n) {
          if (!n || n->is_terminal) return;
          if (n->symbol == "stmt" || n->core.kind == CoreFormKind::Define) {
            stmts.push_back(n);
            return;
          }
          for (const syntax::SyntaxObj *c : n->children) {
            if (c && c->is_terminal && c->text == ";") continue;
            gather(c);
          }
        };
        gather(node->children[static_cast<size_t>(node->core.inner)]);
      } else {
        for (const syntax::SyntaxObj *c : node->children) {
          if (c) stmts.push_back(c);
        }
      }

      // Pass 1: pre-allocate all `Define` symbols (functions and locals) so
      // mutual recursion and forward references resolve cleanly.
      Env inner = env;
      struct DefSlot {
        const syntax::SyntaxObj *def_node = nullptr;
        const syntax::SyntaxObj *lam_node = nullptr;
        Sym *sym = nullptr;
      };
      std::vector<DefSlot> defs;
      for (const syntax::SyntaxObj *s : stmts) {
        const syntax::SyntaxObj *u = s;
        while (u && u->core.kind == CoreFormKind::PassThrough && u->children.size() == 1) {
          u = u->children[0];
        }
        if (u && u->core.kind == CoreFormKind::Define && u->core.bind >= 0 && u->core.rhs >= 0) {
          const syntax::SyntaxObj *binder =
              unwrap_ident_node(u->children[static_cast<size_t>(u->core.bind)]);
          if (!binder) return nullptr;
          const syntax::SyntaxObj *rhs_node = u->children[static_cast<size_t>(u->core.rhs)];
          const syntax::SyntaxObj *lam_node = unwrap_lambda_node(rhs_node);
          Sym *sym = nullptr;
          if (lam_node) {
            std::string fn_name =
                "__wb_deffn_" + binder->text + "_" + std::to_string(++lctx.anon_counter);
            sym = new_Sym(fn_name.c_str());
          } else {
            sym = new_local(lctx, binder->text.c_str());
          }
          inner.bind(binder->text, binder->scopes, sym);
          defs.push_back(DefSlot{.def_node = u, .lam_node = lam_node, .sym = sym});
        }
      }

      // Pass 2: lower all definitions and expression statements in order.
      size_t def_idx = 0;
      Sym *last_val = nullptr;
      for (const syntax::SyntaxObj *s : stmts) {
        const syntax::SyntaxObj *u = s;
        while (u && u->core.kind == CoreFormKind::PassThrough && u->children.size() == 1) {
          u = u->children[0];
        }
        if (u && u->core.kind == CoreFormKind::Define) {
          const DefSlot &slot = defs[def_idx++];
          if (slot.lam_node) {
            if (!lower_lambda(slot.lam_node, inner, lctx, slot.sym)) return nullptr;
          } else {
            Sym *rhs =
                lower_expr(u->children[static_cast<size_t>(u->core.rhs)], code, inner, lctx);
            if (!rhs) return nullptr;
            if1_move(if1, code, rhs, slot.sym);
          }
          last_val = slot.sym;
        } else {
          last_val = lower_expr(u, code, inner, lctx);
          if (!last_val) return nullptr;
        }
      }
      return record(last_val ? last_val : if1_const(if1, sym_int64, "0"));
    }

    case CoreFormKind::Define:
      return nullptr;

    case CoreFormKind::None:
      break;
  }

  // Fallback for direct terminal nodes (e.g. unhygienic_ident spliced into templates).
  if (node->is_terminal) {
    const auto &tok = node->text;
    if (tok.size() >= 2 && tok.front() == '"' && tok.back() == '"') {
      std::string unquoted(tok.substr(1, tok.size() - 2));
      return record(if1_const(if1, sym_string, unquoted.c_str(), nullptr, nullptr,
                              static_cast<int>(unquoted.size())));
    }
    if (!tok.empty() && tok[0] >= '0' && tok[0] <= '9') {
      return record(if1_const(if1, sym_int64, tok.c_str()));
    }
    if (auto bound = env.resolve(tok, node->scopes)) {
      return record(*bound);
    }
    std::fprintf(stderr, "langwb: %d:%d: unbound identifier '%s'", node->line, node->col,
                 tok.c_str());
    print_origin_suffix(node);
    return nullptr;
  }

  // Unwrap untagged single-child non-terminal (e.g. `program -> expr`).
  if (node->children.size() == 1 && !node->children[0]->is_terminal) {
    return record(lower_expr(node->children[0], code, env, lctx));
  }

  std::fprintf(stderr, "langwb: %d:%d: unsupported SyntaxObj '%s' with %zu children", node->line,
               node->col, node->symbol.c_str(), node->children.size());
  print_origin_suffix(node);
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

  PrimRegistry local_prims;
  PrimRegistry *reg = prims ? prims : &local_prims;
  ensure_wb_result_registered(reg);

  ResolutionInfo resolved;
  ResolveEnv renv;
  if (!resolve_node(root, renv, reg, resolved)) return false;

  auto *cb = new WbCallbacks();
  ifa_init(cb);
  if1->partial_default = Partial_NEVER;
  init_default_builtin_types();
  new_builtin_global_variable(sym___main__, "__main__");
  reg->install_into_if1();

  Code *code = nullptr;
  Env env;
  NodeSymMap node_syms;
  RecordFieldMap record_fields;
  LowerCtx lctx{.enclosing_fn = sym___main__,
                .node_syms = &node_syms,
                .record_fields = &record_fields,
                .resolved = &resolved};
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

static bool compile_and_jit_impl(const syntax::SyntaxObj *root, int64_t *out_result,
                                 PrimRegistry *prims, CompileInfo *out_info, bool in_process) {
  if (!root || !out_result) return false;
  if (out_info) *out_info = CompileInfo{};

  PrimRegistry local_prims;
  PrimRegistry *reg = prims ? prims : &local_prims;
  ensure_wb_result_registered(reg);

  ResolutionInfo resolved;
  ResolveEnv renv;
  if (!resolve_node(root, renv, reg, resolved)) return false;

  auto *cb = new WbCallbacks();
  ifa_init(cb);
  if1->partial_default = Partial_NEVER;
  init_default_builtin_types();
  new_builtin_global_variable(sym___main__, "__main__");
  reg->install_into_if1();

  Code *code = nullptr;
  Env env;
  LowerCtx lctx{.enclosing_fn = sym___main__, .node_syms = nullptr, .resolved = &resolved};
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

  char arg0[] = "langwb";
  char *argv[] = {arg0, nullptr};
  int64_t result = 0;
  bool ok = false;

  if (in_process) {
    bool saved_mode = g_in_process_mode;
    bool saved_has = g_in_process_has_result;
    int64_t saved_res = g_in_process_result;
    g_in_process_mode = true;
    g_in_process_has_result = false;
    g_in_process_result = 0;
    int status = llvm_jit_execute_in_process(1, argv);
    ok = (status == 0 && g_in_process_has_result);
    result = g_in_process_result;
    g_in_process_mode = saved_mode;
    g_in_process_has_result = saved_has;
    g_in_process_result = saved_res;
  } else {
    int fds[2];
    if (::pipe(fds) != 0) {
      PrimRegistry::set_active(nullptr);
      ifa_reset();
      return false;
    }
    g_jit_result_fd = fds[1];
    int status = llvm_jit_execute(1, argv);
    g_jit_result_fd = -1;
    ::close(fds[1]);
    ssize_t nread = ::read(fds[0], &result, sizeof(result));
    ::close(fds[0]);
    ok = (status == 0 && nread == static_cast<ssize_t>(sizeof(result)));
  }

  PrimRegistry::set_active(nullptr);
  ifa_reset();

  if (!ok) return false;
  *out_result = result;
  return true;
}

bool compile_and_jit_i64(const syntax::SyntaxObj *root, int64_t *out_result, PrimRegistry *prims,
                         CompileInfo *out_info) {
  return compile_and_jit_impl(root, out_result, prims, out_info, false);
}

bool compile_and_run_in_process_i64(const syntax::SyntaxObj *root, int64_t *out_result,
                                    PrimRegistry *prims, CompileInfo *out_info) {
  return compile_and_jit_impl(root, out_result, prims, out_info, true);
}

std::shared_ptr<LLVMJitModule> compile_persistent_i64(const syntax::SyntaxObj *root,
                                                      PrimRegistry *prims) {
  if (!root) return nullptr;

  PrimRegistry local_prims;
  PrimRegistry *reg = prims ? prims : &local_prims;
  ensure_wb_result_registered(reg);

  ResolutionInfo resolved;
  ResolveEnv renv;
  if (!resolve_node(root, renv, reg, resolved)) return nullptr;

  auto *cb = new WbCallbacks();
  ifa_init(cb);
  if1->partial_default = Partial_NEVER;
  init_default_builtin_types();
  new_builtin_global_variable(sym___main__, "__main__");
  reg->install_into_if1();

  Code *code = nullptr;
  Env env;
  LowerCtx lctx{.enclosing_fn = sym___main__, .node_syms = nullptr, .resolved = &resolved};
  Sym *val = lower_expr(root, &code, env, lctx);
  if (!val) {
    PrimRegistry::set_active(nullptr);
    ifa_reset();
    return nullptr;
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

  if (ifa_analyze("<langwb-persistent>") < 0) {
    PrimRegistry::set_active(nullptr);
    ifa_reset();
    return nullptr;
  }
  ifa_optimize();

  FILE *null_fp = std::fopen("/dev/null", "w");
  llvm_codegen_print_ir(null_fp ? null_fp : stderr, pdb->fa, if1->top->fun, "<langwb-persistent>");
  if (null_fp) std::fclose(null_fp);

  LLVMJitModule *raw = llvm_jit_compile_persistent();
  PrimRegistry::set_active(nullptr);
  ifa_reset();

  return std::shared_ptr<LLVMJitModule>(raw, &llvm_jit_free_persistent);
}

bool run_persistent_i64(const std::shared_ptr<LLVMJitModule> &mod, int64_t *out_result) {
  if (!mod) return false;
  char arg0[] = "langwb";
  char *argv[] = {arg0, nullptr};
  bool saved_mode = g_in_process_mode;
  bool saved_has = g_in_process_has_result;
  int64_t saved_res = g_in_process_result;
  g_in_process_mode = true;
  g_in_process_has_result = false;
  g_in_process_result = 0;
  int status = llvm_jit_call_persistent(mod.get(), 1, argv);
  bool ok = (status == 0 && g_in_process_has_result);
  int64_t res = g_in_process_result;
  g_in_process_mode = saved_mode;
  g_in_process_has_result = saved_has;
  g_in_process_result = saved_res;
  if (ok && out_result) *out_result = res;
  return ok;
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
