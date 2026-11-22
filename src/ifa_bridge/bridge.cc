#include "ifa_bridge/bridge.h"

#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <utility>
#include <vector>

#include "llvm.h"
#include "llvm_internal.h"

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

static void write_result_str(std::string_view sv) {
  if (g_jit_result_fd >= 0 && !sv.empty()) {
    (void)::write(g_jit_result_fd, sv.data(), sv.size());
  }
}

extern "C" void _CG_wb_emit_i64(int64_t value) {
  std::string s = std::to_string(value);
  write_result_str(s);
}

extern "C" void _CG_wb_emit_str(const char *s) {
  std::string out = "\"";
  if (s) {
    for (const char *p = s; *p; ++p) {
      if (*p == '"' || *p == '\\') out.push_back('\\');
      out.push_back(*p);
    }
  }
  out.push_back('"');
  write_result_str(out);
}

extern "C" void _CG_wb_emit_raw(const char *s) {
  if (s) write_result_str(s);
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

struct FnLowerInfo {
  Sym *fn_sym = nullptr;
  std::vector<uint64_t> param_node_ids;
};

struct CallLowerInfo {
  uint64_t call_node_id = 0;
  std::vector<uint64_t> arg_node_ids;
};

using FnSymMap = std::unordered_map<uint64_t, FnLowerInfo>;
using CallSymMap = std::unordered_map<Sym *, CallLowerInfo>;

struct LowerCtx {
  Sym *enclosing_fn = nullptr;
  NodeSymMap *node_syms = nullptr;
  RecordFieldMap *record_fields = nullptr;
  FnSymMap *fn_syms = nullptr;
  CallSymMap *call_syms = nullptr;
  const ResolutionInfo *resolved = nullptr;
  std::unordered_map<Sym *, std::vector<Sym *>> rec_self_caps;
  int anon_counter = 0;
};

void print_origin_suffix(const syntax::SyntaxObj *node) {
  if (node && !node->origin_macro.empty()) {
    std::fprintf(stderr, " (expanded from '%s' at %d:%d)", node->origin_macro.c_str(),
                 node->origin_line, node->origin_col);
  }
  std::fprintf(stderr, "\n");
}

struct SourceLoc {
  cchar *filename = "<input>";
  int line = 0;
  int col = 0;
};

const syntax::SyntaxObj *find_located_node(const syntax::SyntaxObj *node) {
  if (!node) return nullptr;
  if (node->line > 0 || !node->origin_macro.empty()) return node;
  for (const syntax::SyntaxObj *c : node->children) {
    if (const syntax::SyntaxObj *found = find_located_node(c)) return found;
  }
  return nullptr;
}

SourceLoc extract_source_loc(const syntax::SyntaxObj *node) {
  SourceLoc loc;
  const syntax::SyntaxObj *src = find_located_node(node);
  if (!src) return loc;
  if (!src->origin_macro.empty()) {
    std::string tag = "<macro:" + src->origin_macro + "@" + std::to_string(src->origin_line) +
                      ":" + std::to_string(src->origin_col) + ">";
    loc.filename = if1_cannonicalize_string(if1, tag.c_str());
    loc.line = (src->line > 0) ? src->line : (src->origin_line > 0 ? src->origin_line : 1);
    loc.col = (src->col > 0) ? src->col : src->origin_col;
  } else {
    loc.filename = "<input>";
    loc.line = src->line;
    loc.col = src->col;
  }
  return loc;
}

WbAST *make_wb_ast(const syntax::SyntaxObj *node, Sym *sym = nullptr) {
  SourceLoc loc = extract_source_loc(node);
  auto *ast = new WbAST();
  ast->filename = loc.filename;
  ast->line_no = loc.line;
  ast->col_no = loc.col;
  ast->sym_ptr = sym;
  return ast;
}

void attach_loc(Code *c, const syntax::SyntaxObj *node, Sym *sym = nullptr) {
  if (!c || !node) return;
  c->ast = make_wb_ast(node, sym);
}

void attach_loc(Sym *s, const syntax::SyntaxObj *node) {
  if (!s || !node) return;
  SourceLoc loc = extract_source_loc(node);
  if (auto *ws = dynamic_cast<WbSymbol *>(s->asymbol)) {
    ws->filename = loc.filename;
    ws->line_no = loc.line;
    ws->col_no = loc.col;
  }
  if (!s->ast) {
    auto *ast = new WbAST();
    ast->filename = loc.filename;
    ast->line_no = loc.line;
    ast->col_no = loc.col;
    ast->sym_ptr = s;
    s->ast = ast;
  }
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

  auto bad_core_role = [&](const char *detail) -> bool {
    std::fprintf(stderr, "langwb: %d:%d: invalid core(%s) annotation on '%s': %s", node->line,
                 node->col, parse::core_form_name(node->core.kind), node->symbol.c_str(), detail);
    print_origin_suffix(node);
    return false;
  };

  auto valid_idx = [&](int8_t idx) -> bool {
    return idx >= 0 && static_cast<size_t>(idx) < node->children.size();
  };

  switch (node->core.kind) {
    case CoreFormKind::PassThrough:
      if (node->children.size() == 1) {
        return resolve_node(node->children[0], env, prims, out);
      }
      return bad_core_role("expected 1 child");

    case CoreFormKind::Paren:
      if (valid_idx(node->core.inner)) {
        return resolve_node(node->children[static_cast<size_t>(node->core.inner)], env, prims, out);
      }
      return bad_core_role("missing or out-of-range 'inner' role");

    case CoreFormKind::Datum:
    case CoreFormKind::String:
      return true;

    case CoreFormKind::Ident: {
      const syntax::SyntaxObj *id = unwrap_ident_node(node);
      if (!id) return bad_core_role("expected identifier leaf");
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
      if (!valid_idx(node->core.lhs) || !valid_idx(node->core.op) || !valid_idx(node->core.rhs)) {
        return bad_core_role("missing or out-of-range 'lhs', 'op', or 'rhs' role");
      }
      return resolve_node(node->children[static_cast<size_t>(node->core.lhs)], env, prims, out) &&
             resolve_node(node->children[static_cast<size_t>(node->core.rhs)], env, prims, out);
    }

    case CoreFormKind::If: {
      if (!valid_idx(node->core.test) || !valid_idx(node->core.then_b) ||
          !valid_idx(node->core.else_b)) {
        return bad_core_role("missing or out-of-range 'test', 'then', or 'else' role");
      }
      return resolve_node(node->children[static_cast<size_t>(node->core.test)], env, prims, out) &&
             resolve_node(node->children[static_cast<size_t>(node->core.then_b)], env, prims,
                          out) &&
             resolve_node(node->children[static_cast<size_t>(node->core.else_b)], env, prims, out);
    }

    case CoreFormKind::Record: {
      if (!valid_idx(node->core.fields)) {
        return bad_core_role("missing or out-of-range 'fields' role");
      }
      std::vector<std::pair<std::string, const syntax::SyntaxObj *>> fields;
      collect_fields(node->children[static_cast<size_t>(node->core.fields)], fields);
      for (const auto &[fname, fexpr] : fields) {
        if (!resolve_node(fexpr, env, prims, out)) return false;
      }
      return true;
    }

    case CoreFormKind::Field: {
      if (!valid_idx(node->core.obj) || !valid_idx(node->core.field)) {
        return bad_core_role("missing or out-of-range 'obj' or 'field' role");
      }
      if (!unwrap_ident_node(node->children[static_cast<size_t>(node->core.field)])) {
        return bad_core_role("'field' role must be an identifier");
      }
      return resolve_node(node->children[static_cast<size_t>(node->core.obj)], env, prims, out);
    }

    case CoreFormKind::Let: {
      if (!valid_idx(node->core.bind) || !valid_idx(node->core.rhs) ||
          !valid_idx(node->core.body)) {
        return bad_core_role("missing or out-of-range 'bind', 'rhs', or 'body' role");
      }
      if (!resolve_node(node->children[static_cast<size_t>(node->core.rhs)], env, prims, out)) {
        return false;
      }
      const syntax::SyntaxObj *binder =
          unwrap_ident_node(node->children[static_cast<size_t>(node->core.bind)]);
      if (!binder) return bad_core_role("'bind' role must be an identifier");
      ResolveEnv inner = env;
      inner.bind(binder->text, binder->scopes, binder);
      return resolve_node(node->children[static_cast<size_t>(node->core.body)], inner, prims, out);
    }

    case CoreFormKind::LetRec: {
      if (!valid_idx(node->core.bind) || !valid_idx(node->core.rhs) ||
          !valid_idx(node->core.body)) {
        return bad_core_role("missing or out-of-range 'bind', 'rhs', or 'body' role");
      }
      const syntax::SyntaxObj *binder =
          unwrap_ident_node(node->children[static_cast<size_t>(node->core.bind)]);
      if (!binder) return bad_core_role("'bind' role must be an identifier");
      ResolveEnv inner = env;
      inner.bind(binder->text, binder->scopes, binder);
      return resolve_node(node->children[static_cast<size_t>(node->core.rhs)], inner, prims, out) &&
             resolve_node(node->children[static_cast<size_t>(node->core.body)], inner, prims, out);
    }

    case CoreFormKind::Lambda: {
      if (!valid_idx(node->core.bind) || !valid_idx(node->core.body)) {
        return bad_core_role("missing or out-of-range 'bind' or 'body' role");
      }
      std::vector<const syntax::SyntaxObj *> params;
      collect_params(node->children[static_cast<size_t>(node->core.bind)], params);
      ResolveEnv inner = env;
      for (const syntax::SyntaxObj *p : params) {
        inner.bind(p->text, p->scopes, p);
      }
      return resolve_node(node->children[static_cast<size_t>(node->core.body)], inner, prims, out);
    }

    case CoreFormKind::App: {
      if (!valid_idx(node->core.fn)) {
        return bad_core_role("missing or out-of-range 'fn' role");
      }
      if (node->core.args >= 0 && !valid_idx(node->core.args)) {
        return bad_core_role("out-of-range 'args' role");
      }
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
          if (n->core.kind != CoreFormKind::None) {
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

    case CoreFormKind::UsingSyntax:
      std::fprintf(stderr, "langwb: %d:%d: unexpanded 'using syntax' region\n", node->line,
                   node->col);
      return false;

    case CoreFormKind::Amb:
      std::fprintf(stderr, "langwb: %d:%d: unresolved syntax ambiguity (%zu alternatives) for '%s'\n",
                   node->line, node->col, node->children.size(), node->text.c_str());
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

struct CapturedVar {
  std::string name;
  syntax::ScopeSet binder_scopes;
  Sym *outer_sym = nullptr;
};

void collect_local_binder_ids(const syntax::SyntaxObj *node,
                              std::unordered_set<uint64_t> &local_binders) {
  if (!node || node->is_terminal) return;
  using parse::CoreFormKind;
  if ((node->core.kind == CoreFormKind::Let || node->core.kind == CoreFormKind::LetRec ||
       node->core.kind == CoreFormKind::Define) &&
      node->core.bind >= 0 && static_cast<size_t>(node->core.bind) < node->children.size()) {
    if (const syntax::SyntaxObj *b =
            unwrap_ident_node(node->children[static_cast<size_t>(node->core.bind)])) {
      local_binders.insert(b->node_id);
    }
  } else if (node->core.kind == CoreFormKind::Lambda && node->core.bind >= 0 &&
             static_cast<size_t>(node->core.bind) < node->children.size()) {
    std::vector<const syntax::SyntaxObj *> params;
    collect_params(node->children[static_cast<size_t>(node->core.bind)], params);
    for (const syntax::SyntaxObj *p : params) local_binders.insert(p->node_id);
  }
  for (const syntax::SyntaxObj *c : node->children) {
    collect_local_binder_ids(c, local_binders);
  }
}

void collect_free_captures(const syntax::SyntaxObj *node, const Env &outer_env,
                           const ResolutionInfo *resolved,
                           const std::unordered_set<uint64_t> &local_binders,
                           std::vector<CapturedVar> &out, std::unordered_set<Sym *> &seen_syms) {
  if (!node || !resolved) return;
  if (auto it = resolved->ref_to_binder.find(node->node_id); it != resolved->ref_to_binder.end()) {
    const syntax::SyntaxObj *binder = it->second;
    if (binder && !local_binders.count(binder->node_id)) {
      if (auto bound = outer_env.resolve(binder->text, binder->scopes)) {
        Sym *outer_sym = *bound;
        if (outer_sym && !outer_sym->is_fun && seen_syms.insert(outer_sym).second) {
          out.push_back(CapturedVar{
              .name = binder->text, .binder_scopes = binder->scopes, .outer_sym = outer_sym});
        }
      }
    }
    return;
  }
  for (const syntax::SyntaxObj *c : node->children) {
    collect_free_captures(c, outer_env, resolved, local_binders, out, seen_syms);
  }
}

Sym *lower_lambda(const syntax::SyntaxObj *node, Code **enclosing_code, const Env &env,
                  LowerCtx &lctx, Sym *prealloc_fn = nullptr,
                  const syntax::SyntaxObj *rec_binder = nullptr) {
  std::vector<const syntax::SyntaxObj *> param_nodes;
  collect_params(node->children[static_cast<size_t>(node->core.bind)], param_nodes);

  const syntax::SyntaxObj *body_node = node->children[static_cast<size_t>(node->core.body)];

  // Identify free variables captured from enclosing lexical scopes.
  std::unordered_set<uint64_t> local_binders;
  for (const syntax::SyntaxObj *pn : param_nodes) local_binders.insert(pn->node_id);
  if (rec_binder) local_binders.insert(rec_binder->node_id);
  collect_local_binder_ids(body_node, local_binders);

  std::vector<CapturedVar> captures;
  std::unordered_set<Sym *> seen_syms;
  collect_free_captures(body_node, env, lctx.resolved, local_binders, captures, seen_syms);

  Sym *fn = prealloc_fn;
  if (!fn) {
    std::string fn_name = "__wb_fn_" + std::to_string(++lctx.anon_counter);
    fn = new_Sym(fn_name.c_str());
  }
  attach_loc(fn, node);
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
  closure_args.reserve(1 + captures.size() + param_nodes.size());
  closure_args.push_back(fn);

  // Leading hidden parameters for captured free variables.
  std::vector<Sym *> cap_params;
  cap_params.reserve(captures.size());
  for (const auto &cap : captures) {
    Sym *cs = new_Sym(cap.name.c_str());
    cs->is_local = 1;
    cs->nesting_depth = LOCALLY_NESTED;
    cs->in = fn;
    closure_args.push_back(cs);
    cap_params.push_back(cs);
    inner.bind(cap.name, cap.binder_scopes, cs);
  }

  for (const syntax::SyntaxObj *pn : param_nodes) {
    Sym *ps = new_Sym(pn->text.c_str());
    attach_loc(ps, pn);
    ps->is_local = 1;
    ps->nesting_depth = LOCALLY_NESTED;
    ps->in = fn;
    closure_args.push_back(ps);
    inner.bind(pn->text, pn->scopes, ps);
  }

  Code *fn_code = nullptr;
  Sym *saved_enclosing = lctx.enclosing_fn;
  lctx.enclosing_fn = fn;

  // If this is a recursive lambda that captures outer variables, record `cap_params`
  // so direct self-calls inside `fn_code` pass them explicitly with zero closure allocation.
  bool added_rec_caps = false;
  if (rec_binder && !captures.empty()) {
    lctx.rec_self_caps[fn] = cap_params;
    added_rec_caps = true;
  }

  Sym *body_sym = lower_expr(body_node, &fn_code, inner, lctx);
  if (added_rec_caps) lctx.rec_self_caps.erase(fn);
  lctx.enclosing_fn = saved_enclosing;
  if (!body_sym) return nullptr;

  if1_move(if1, &fn_code, body_sym, fn->ret);
  if1_send(if1, &fn_code, 4, 0, sym_primitive, sym_reply, fn->cont, fn->ret);
  if1_closure(if1, fn, fn_code, static_cast<int>(closure_args.size()), closure_args.data());
  if (lctx.fn_syms && node->node_id != 0) {
    FnLowerInfo info;
    info.fn_sym = fn;
    for (const syntax::SyntaxObj *pn : param_nodes) {
      info.param_node_ids.push_back(pn->node_id);
    }
    (*lctx.fn_syms)[node->node_id] = std::move(info);
  }

  if (captures.empty()) {
    return fn;
  }

  // Emit an IF1 partial-application closure capturing the outer free variables.
  Sym *cls = new_local(lctx);
  attach_loc(cls, node);
  Code *psend = if1_send1(if1, enclosing_code);
  attach_loc(psend, node, cls);
  psend->partial = Partial_OK;
  if1_add_send_arg(if1, psend, fn);
  for (const auto &cap : captures) {
    if1_add_send_arg(if1, psend, cap.outer_sym);
  }
  if1_add_send_result(if1, psend, cls);
  return cls;
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
      attach_loc(rec_type, node);
      rec_type->type_kind = Type_RECORD;
      rec_type->implements.add(sym_object);
      rec_type->specializes.add(sym_object);

      std::vector<Sym *> field_vals;
      field_vals.reserve(fields.size());
      std::vector<std::string> field_names_copy;
      field_names_copy.reserve(fields.size());
      for (const auto &[fname, fexpr] : fields) {
        Sym *fsym = new_Sym(fname.c_str());
        attach_loc(fsym, fexpr);
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
      attach_loc(obj, node);
      Code *new_send = if1_send(if1, code, 3, 1, sym_primitive, sym_new, rec_type, obj);
      attach_loc(new_send, node, obj);
      for (size_t i = 0; i < fields.size(); ++i) {
        Sym *setter_res = new_local(lctx);
        Code *set_send = if1_send(if1, code, 5, 1, sym_operator, obj, sym_setter,
                                  if1_make_symbol(if1, fields[i].first.c_str()), field_vals[i],
                                  setter_res);
        attach_loc(set_send, fields[i].second, setter_res);
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
      attach_loc(res, fld);
      Code *fld_send = if1_send(if1, code, 4, 1, sym_operator, obj, sym_period,
                                if1_make_symbol(if1, fld->text.c_str()), res);
      attach_loc(fld_send, fld, res);
      return record(res);
    }

    case CoreFormKind::BinOp: {
      Sym *lhs = lower_expr(node->children[static_cast<size_t>(node->core.lhs)], code, env, lctx);
      Sym *rhs = lower_expr(node->children[static_cast<size_t>(node->core.rhs)], code, env, lctx);
      if (!lhs || !rhs) return nullptr;
      const syntax::SyntaxObj *op_node = node->children[static_cast<size_t>(node->core.op)];
      Sym *op = if1_make_symbol(if1, op_node->text.c_str());
      Sym *res = new_local(lctx);
      attach_loc(res, op_node);
      Code *bin_send = if1_send(if1, code, 4, 1, sym_operator, lhs, op, rhs, res);
      attach_loc(bin_send, op_node, res);
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
      attach_loc(res, node);
      Code *send = if1_send1(if1, code);
      attach_loc(send, node, res);
      if (is_prim) {
        Sym *prim_sym = if1_make_symbol(if1, pit->second.c_str());
        if1_add_send_arg(if1, send, sym_primitive);
        if1_add_send_arg(if1, send, prim_sym);
      } else {
        if1_add_send_arg(if1, send, callee_sym);
        if (auto rit = lctx.rec_self_caps.find(callee_sym); rit != lctx.rec_self_caps.end()) {
          for (Sym *cp : rit->second) if1_add_send_arg(if1, send, cp);
        }
      }
      for (Sym *as : arg_syms) {
        if1_add_send_arg(if1, send, as);
      }
      if1_add_send_result(if1, send, res);
      if (lctx.call_syms && !is_prim && node->node_id != 0) {
        CallLowerInfo cinfo;
        cinfo.call_node_id = node->node_id;
        for (const syntax::SyntaxObj *an : arg_nodes) {
          cinfo.arg_node_ids.push_back(an->node_id);
        }
        (*lctx.call_syms)[res] = std::move(cinfo);
      }
      return record(res);
    }

    case CoreFormKind::Lambda:
      return record(lower_lambda(node, code, env, lctx));

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
      attach_loc(res, node);
      Code *if_c =
          if1_if(if1, code, cond_code, cond_sym, then_code, then_sym, else_code, else_sym, res);
      attach_loc(if_c, node, res);
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
        attach_loc(bound, binder);
        Code *mv = if1_move(if1, code, rhs, bound);
        attach_loc(mv, binder, bound);
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
        fn->is_fun = 1;
        inner.bind(binder->text, binder->scopes, fn);
        Sym *lam_val = lower_lambda(lam_node, code, inner, lctx, fn, binder);
        if (!lam_val) return nullptr;
        if (lam_val != fn) {
          inner.bind(binder->text, binder->scopes, lam_val);
        }
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
          if (n->core.kind != CoreFormKind::None) {
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
            sym->is_fun = 1;
          } else {
            sym = new_local(lctx, binder->text.c_str());
          }
          attach_loc(sym, binder);
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
            const syntax::SyntaxObj *binder =
                unwrap_ident_node(u->children[static_cast<size_t>(u->core.bind)]);
            Sym *lam_val = lower_lambda(slot.lam_node, code, inner, lctx, slot.sym, binder);
            if (!lam_val) return nullptr;
            if (lam_val != slot.sym && binder) {
              inner.bind(binder->text, binder->scopes, lam_val);
            }
            last_val = lam_val;
          } else {
            Sym *rhs =
                lower_expr(u->children[static_cast<size_t>(u->core.rhs)], code, inner, lctx);
            if (!rhs) return nullptr;
            if1_move(if1, code, rhs, slot.sym);
            last_val = slot.sym;
          }
        } else {
          last_val = lower_expr(u, code, inner, lctx);
          if (!last_val) return nullptr;
        }
      }
      return record(last_val ? last_val : if1_const(if1, sym_int64, "0"));
    }

    case CoreFormKind::Define:
    case CoreFormKind::UsingSyntax:
    case CoreFormKind::Amb:
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
    ContourAttr ca;
    ca.is_monomorphic = true;
    ca.has_const_i64 = true;
    ca.const_i64 = s->imm.v_int64;
    ca.type_names.push_back("int64");
    attr.contours.push_back(std::move(ca));
    return attr;
  }

  if (!top) return attr;

  std::set<std::string> types;
  std::vector<std::string> fields;
  std::set<std::string> seen_fields;
  bool saw_any_value = false;
  bool all_same_const = true;
  int64_t common_const = 0;

  // Deduplicate repeated Var* / (Fun*, EntrySet*) observations across PNodes.
  std::set<Var *> seen_vars;
  std::unordered_map<EntrySet *, size_t> es_slot;
  struct ContourAccum {
    bool saw_value = false;
    bool has_const = false;
    bool all_same_const = true;
    int64_t const_val = 0;
    std::set<std::string> types;
    std::vector<std::string> fields;
    std::set<std::string> seen_fields;
  };
  std::vector<ContourAccum> contour_accums;

  auto collect_record_fields_into = [&](Sym *ty, std::vector<std::string> &dst,
                                        std::set<std::string> &seen) {
    if (!ty || ty->type_kind != Type_RECORD) return;
    size_t before = dst.size();
    for (int i = 0; i < ty->has.n; ++i) {
      if (ty->has[i] && ty->has[i]->name && seen.insert(ty->has[i]->name).second) {
        dst.emplace_back(ty->has[i]->name);
      }
    }
    // Cloned records may have unread fields pruned from `ty->has`; fall back
    // to the original lowered record field list recorded during lowering.
    if (dst.size() == before && ty->name && record_fields) {
      if (auto it = record_fields->find(ty->name); it != record_fields->end()) {
        for (const auto &fn : it->second) {
          if (seen.insert(fn).second) dst.push_back(fn);
        }
      }
    }
  };

  auto inspect_var = [&](Var *v, Fun *owner_fun) {
    if (!v || !seen_vars.insert(v).second) return;
    if (v->type) {
      if (v->type->name) types.insert(v->type->name);
      collect_record_fields_into(v->type, fields, seen_fields);
    }
    bool saw_av = false;
    for (EntrySet *es : owner_fun->ess) {
      size_t slot = 0;
      if (es) {
        auto [it, inserted] = es_slot.emplace(es, contour_accums.size());
        if (inserted) contour_accums.emplace_back();
        slot = it->second;
      } else {
        slot = contour_accums.size();
        contour_accums.emplace_back();
        continue;
      }
      AVar *av = make_AVar(v, es);
      if (!av || !av->out || av->out->n == 0) continue;
      saw_av = true;
      saw_any_value = true;
      auto &ca = contour_accums[slot];
      if (!ca.saw_value) {
        ca.saw_value = true;
        attr.contour_count++;
      }
      int64_t c = 0;
      if (try_get_const_i64(av, &c)) {
        if (!ca.has_const) {
          ca.const_val = c;
          ca.has_const = true;
        } else if (ca.const_val != c) {
          ca.all_same_const = false;
        }
        if (!attr.has_const_i64) {
          common_const = c;
          attr.has_const_i64 = true;
        } else if (common_const != c) {
          all_same_const = false;
        }
      } else {
        ca.all_same_const = false;
        all_same_const = false;
      }
      for (int i = 0; i < av->out->n; ++i) {
        CreationSet *cs = av->out->v[i];
        if (cs && cs->sym) {
          collect_record_fields_into(cs->sym, fields, seen_fields);
          collect_record_fields_into(cs->sym->type, fields, seen_fields);
          collect_record_fields_into(cs->sym, ca.fields, ca.seen_fields);
          collect_record_fields_into(cs->sym->type, ca.fields, ca.seen_fields);
          if (cs->sym->type && cs->sym->type->name) {
            types.insert(cs->sym->type->name);
            ca.types.insert(cs->sym->type->name);
          } else if (cs->sym->name) {
            types.insert(cs->sym->name);
            ca.types.insert(cs->sym->name);
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

  attr.contours.reserve(contour_accums.size());
  for (auto &ca : contour_accums) {
    ContourAttr out_ca;
    out_ca.type_names.assign(ca.types.begin(), ca.types.end());
    out_ca.field_names = std::move(ca.fields);
    out_ca.is_monomorphic = (out_ca.type_names.size() == 1);
    if (ca.saw_value && ca.has_const && ca.all_same_const) {
      out_ca.has_const_i64 = true;
      out_ca.const_i64 = ca.const_val;
    }
    attr.contours.push_back(std::move(out_ca));
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
  if (!reg->has("wb_emit_val")) {
    reg->register_prim(
        "wb_emit_val",
        PrimitiveHandler{
            .transfer =
                [](PNode *pn, EntrySet *es) {
                  prim_emit_result(pn, es, make_abstract_type(sym_void));
                },
            .emit_llvm =
                [](PNode *pn, Fun * /*f*/) {
                  if (pn->rvals.n < 3 || !pn->rvals[2]) return;
                  Var *arg = pn->rvals[2];
                  llvm::Value *v = llvm_emit_get_var(arg);
                  llvm::Type *void_ty = llvm::Type::getVoidTy(*TheContext);
                  llvm::Type *ptr_ty = llvm::PointerType::getUnqual(*TheContext);
                  llvm::Type *i64_ty = llvm::Type::getInt64Ty(*TheContext);
                  Sym *ty = arg->type;
                  if (ty && (ty->is_fun || ty->type_kind == Type_FUN)) {
                    llvm::Value *s = Builder->CreateGlobalString("<fn>");
                    llvm::FunctionType *ft = llvm::FunctionType::get(void_ty, {ptr_ty}, false);
                    llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_emit_raw", ft);
                    Builder->CreateCall(ft, fn.getCallee(), {s});
                  } else if (ty == sym_string && v && v->getType()->isPointerTy()) {
                    llvm::FunctionType *ft = llvm::FunctionType::get(void_ty, {ptr_ty}, false);
                    llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_emit_str", ft);
                    Builder->CreateCall(ft, fn.getCallee(), {v});
                  } else if (v && v->getType()->isIntegerTy(64)) {
                    llvm::FunctionType *ft = llvm::FunctionType::get(void_ty, {i64_ty}, false);
                    llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_emit_i64", ft);
                    Builder->CreateCall(ft, fn.getCallee(), {v});
                  } else {
                    llvm::Value *s = Builder->CreateGlobalString("<val>");
                    llvm::FunctionType *ft = llvm::FunctionType::get(void_ty, {ptr_ty}, false);
                    llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_emit_raw", ft);
                    Builder->CreateCall(ft, fn.getCallee(), {s});
                  }
                },
            .is_functional = false,
            .is_visible = true,
        });
  }
  if (!reg->has("wb_emit_raw")) {
    reg->register_prim(
        "wb_emit_raw",
        PrimitiveHandler{
            .transfer =
                [](PNode *pn, EntrySet *es) {
                  prim_emit_result(pn, es, make_abstract_type(sym_void));
                },
            .emit_llvm =
                [](PNode *pn, Fun * /*f*/) {
                  if (pn->rvals.n < 3 || !pn->rvals[2]) return;
                  llvm::Value *v = llvm_emit_get_var(pn->rvals[2]);
                  if (!v) return;
                  llvm::Type *void_ty = llvm::Type::getVoidTy(*TheContext);
                  llvm::Type *ptr_ty = llvm::PointerType::getUnqual(*TheContext);
                  llvm::FunctionType *ft = llvm::FunctionType::get(void_ty, {ptr_ty}, false);
                  llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_emit_raw", ft);
                  Builder->CreateCall(ft, fn.getCallee(), {v});
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
  if1_register_sym(if1, s->sym, sym ? sym->name : nullptr);
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
  FnSymMap fn_syms;
  CallSymMap call_syms;
  LowerCtx lctx{.enclosing_fn = sym___main__,
                .node_syms = &node_syms,
                .record_fields = &record_fields,
                .fn_syms = &fn_syms,
                .call_syms = &call_syms,
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

  for (const auto &[ref_id, binder_node] : resolved.ref_to_binder) {
    if (binder_node) out_attrs->set_ref_binder(ref_id, binder_node->node_id);
  }

  Fun *top = if1->top ? if1->top->fun : nullptr;
  for (const auto &[node_id, sym] : node_syms) {
    out_attrs->set(node_id, extract_sym_attr(sym, top, &record_fields));
  }

  // Count distinct target Funs reachable from each call site across the whole program
  // so contour cloning only redirects calls proven to target a single function exclusively.
  std::unordered_map<uint64_t, std::set<Fun *>> call_targets;
  for (Fun *f : pdb->fa->funs) {
    if (!f) continue;
    for (EntrySet *es : f->ess) {
      if (!es) continue;
      for (AEdge *e : es->edges) {
        if (!e || !e->pnode || e->pnode->lvals.n == 0 || !e->pnode->lvals[0]) continue;
        Sym *res_sym = e->pnode->lvals[0]->sym;
        if (auto cit = call_syms.find(res_sym); cit != call_syms.end()) {
          call_targets[cit->second.call_node_id].insert(f);
        }
      }
    }
  }
  for (const auto &[cid, targets] : call_targets) {
    out_attrs->set_call_target_count(cid, static_cast<int>(targets.size()));
  }

  // Capture per-function contours and call-site -> contour edges from IFA's EntrySets.
  for (const auto &[fn_node_id, fn_rec] : fn_syms) {
    if (!fn_rec.fn_sym || !fn_rec.fn_sym->fun) continue;
    Fun *f = fn_rec.fn_sym->fun;
    FnContourInfo info;
    info.fn_node_id = fn_node_id;
    info.contour_count = f->ess.n;
    info.param_node_ids = fn_rec.param_node_ids;
    std::set<std::pair<uint64_t, int>> seen_calls;
    for (int ci = 0; ci < f->ess.n; ++ci) {
      EntrySet *es = f->ess[ci];
      if (!es) continue;
      for (AEdge *e : es->edges) {
        if (!e || !e->pnode || e->pnode->lvals.n == 0 || !e->pnode->lvals[0]) continue;
        Sym *res_sym = e->pnode->lvals[0]->sym;
        auto cit = call_syms.find(res_sym);
        if (cit == call_syms.end()) continue;
        if (seen_calls.insert({cit->second.call_node_id, ci}).second) {
          info.call_sites.push_back(FnContourInfo::CallSite{
              .call_node_id = cit->second.call_node_id,
              .contour_idx = ci,
              .arg_node_ids = cit->second.arg_node_ids,
          });
        }
      }
    }
    out_attrs->set_fn_info(fn_node_id, std::move(info));
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

bool compile_and_jit_value(const syntax::SyntaxObj *root, EvalValue *out_value,
                           PrimRegistry *prims, CompileInfo *out_info,
                           const std::vector<std::string> *record_fields) {
  if (!root || !out_value) return false;
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

  Sym *emit_val_sym = if1_make_symbol(if1, "wb_emit_val");
  Sym *emit_raw_sym = if1_make_symbol(if1, "wb_emit_raw");
  auto emit_raw = [&](const std::string &s) {
    Sym *sconst = if1_const(if1, sym_string, s.c_str(), nullptr, nullptr,
                            static_cast<int>(s.size()));
    Sym *d = new_Sym();
    if1_send(if1, &code, 3, 1, sym_primitive, emit_raw_sym, sconst, d);
  };

  if (record_fields && !record_fields->empty()) {
    emit_raw("{ ");
    for (size_t i = 0; i < record_fields->size(); ++i) {
      if (i > 0) emit_raw(", ");
      emit_raw((*record_fields)[i] + ": ");
      Sym *fval = new_local(lctx);
      if1_send(if1, &code, 4, 1, sym_operator, val, sym_period,
               if1_make_symbol(if1, (*record_fields)[i].c_str()), fval);
      Sym *d = new_Sym();
      if1_send(if1, &code, 3, 1, sym_primitive, emit_val_sym, fval, d);
    }
    emit_raw(" }");
  } else {
    Sym *d = new_Sym();
    if1_send(if1, &code, 3, 1, sym_primitive, emit_val_sym, val, d);
  }

  Sym *fn = sym___main__;
  fn->cont = new_Sym();
  fn->cont->var = new Var(fn->cont);
  fn->ret = sym_nil;
  if1_send(if1, &code, 4, 0, sym_primitive, sym_reply, fn->cont, fn->ret);
  if1_closure(if1, fn, code, 1, &fn);
  if1->top = fn;

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

  char arg0[] = "langwb";
  char *argv[] = {arg0, nullptr};
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

  std::string out_str;
  char buf[256];
  ssize_t n = 0;
  while ((n = ::read(fds[0], buf, sizeof(buf))) > 0) {
    out_str.append(buf, static_cast<size_t>(n));
  }
  ::close(fds[0]);

  PrimRegistry::set_active(nullptr);
  ifa_reset();

  if (status != 0 || out_str.empty()) return false;
  out_value->str = out_str;
  if (record_fields && !record_fields->empty()) {
    out_value->kind = EvalValue::Kind::Record;
  } else if (out_str.front() == '"') {
    out_value->kind = EvalValue::Kind::String;
  } else if (out_str == "<fn>") {
    out_value->kind = EvalValue::Kind::Function;
  } else {
    out_value->kind = EvalValue::Kind::Int64;
    out_value->i64 = std::strtoll(out_str.c_str(), nullptr, 10);
  }
  return true;
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
