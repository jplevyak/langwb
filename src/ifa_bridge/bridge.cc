#include "ifa_bridge/bridge.h"

#include <cstdio>
#include <filesystem>
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

extern "C" void _CG_wb_set_emit_sink(void (*fn)(const char *, size_t));

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

static void jit_emit_sink(const char *data, size_t len) {
  if (g_jit_result_fd >= 0 && data && len > 0) {
    (void)::write(g_jit_result_fd, data, len);
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
  std::unordered_map<const syntax::SyntaxObj *, int> binder_depth;
  std::unordered_set<const syntax::SyntaxObj *> assigned_binders;
  std::unordered_set<const syntax::SyntaxObj *> captured_binders;
  std::unordered_set<const syntax::SyntaxObj *> def_fn_binders;

  bool is_boxed(const syntax::SyntaxObj *binder) const {
    return binder && assigned_binders.count(binder) && captured_binders.count(binder);
  }
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

struct CapturedVar {
  std::string name;
  syntax::ScopeSet binder_scopes;
  Sym *outer_sym = nullptr;
};

struct LowerCtx {
  Sym *enclosing_fn = nullptr;
  NodeSymMap *node_syms = nullptr;
  RecordFieldMap *record_fields = nullptr;
  FnSymMap *fn_syms = nullptr;
  CallSymMap *call_syms = nullptr;
  const ResolutionInfo *resolved = nullptr;
  std::unordered_map<Sym *, std::vector<CapturedVar>> rec_self_caps;
  // Number of parameters the USER wrote for each function value (propagated
  // through `let`, `def` and import-alias moves), for the upfront "wrong number
  // of arguments" error. Deliberately not the IF1 closure's arity: captured
  // variables become hidden leading parameters (`lower_lambda`, `per_def_caps`)
  // that are supplied by the closure or appended at the call site after this
  // check, so counting them here would reject every correct call.
  std::unordered_map<Sym *, size_t> fn_arity;
  std::unordered_set<Sym *> boxed_syms;
  const PrimRegistry *prims = nullptr;
  std::unordered_map<std::string, Sym *> adt_variant_syms;
  // Scrutinees already given a `wb_check_adt`: each `match` arm lowers to its
  // own `__wb_is_variant`, but the type check should be reported once per match.
  std::unordered_set<Sym *> adt_checked_scruts;
  int anon_counter = 0;
};

void init_adt_types(LowerCtx &lctx, const PrimRegistry *prims) {
  lctx.prims = prims;
  if (!prims) return;
  for (const AdtDef &adt : prims->adts()) {
    for (const AdtVariant &v : adt.variants) {
      Sym *v_type = new_Sym(v.qualified_name().c_str());
      v_type->type_kind = Type_RECORD;
      v_type->implements.add(sym_object);
      v_type->specializes.add(sym_object);
      make_meta_type(v_type);
      if (v.fields.empty()) {
        Sym *fsym = new_Sym("_tag");
        fsym->in = v_type;
        v_type->has.add(fsym);
      } else {
        for (const std::string &fn : v.fields) {
          Sym *fsym = new_Sym(fn.c_str());
          fsym->in = v_type;
          v_type->has.add(fsym);
        }
      }
      lctx.adt_variant_syms[v.name] = v_type;
    }
  }
}

void print_origin_suffix(const syntax::SyntaxObj *node) {
  if (node && !node->origin_macro.empty()) {
    std::fprintf(stderr, " (expanded from '%s' at %d:%d)", node->origin_macro.c_str(),
                 node->origin_line, node->origin_col);
  }
  std::fprintf(stderr, "\n");
}

// If `name` is a constructor that exists but is private to the module that
// defines it, explains that (an importer sees only exported constructors).
// Returns the explanation, or "" when `name` is not such a constructor.
std::string private_variant_note(const PrimRegistry *prims, const std::string &name) {
  if (!prims) return {};
  for (const AdtDef &adt : prims->adts()) {
    for (const AdtVariant &v : adt.variants) {
      if (v.name != name || v.private_scope == 0) continue;
      std::string mod = adt.origin_module.empty()
                            ? std::string("its module")
                            : "module '" + std::filesystem::path(adt.origin_module).filename().string() + "'";
      return "constructor '" + name + "' of type '" + adt.name + "' is not exported by " + mod +
             " (add 'export " + name + ";' or 'export " + adt.name + ";' there)";
    }
  }
  return {};
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
  if (src->file) {
    // Imported module: report in that file (at the macro call site when expanded).
    loc.filename = src->file;
    if (src->line > 0 && src->origin_macro.empty()) {
      loc.line = src->line;
      loc.col = src->col;
    } else {
      loc.line = src->origin_line > 0 ? src->origin_line : src->line;
      loc.col = src->origin_line > 0 ? src->origin_col : src->col;
    }
  } else if (!src->origin_macro.empty()) {
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
                  ResolutionInfo &out, int lambda_depth = 0) {
  if (!node) return false;
  using parse::CoreFormKind;

  auto bad_core_role = [&](const char *detail) -> bool {
    std::fprintf(stderr, "langwb: %s%d:%d: invalid core(%s) annotation on '%s': %s", syntax::file_prefix(node).c_str(), node->line,
                 node->col, parse::core_form_name(node->core.kind), node->symbol.c_str(), detail);
    print_origin_suffix(node);
    return false;
  };

  auto valid_idx = [&](int8_t idx) -> bool {
    return idx >= 0 && static_cast<size_t>(idx) < node->children.size();
  };

  auto note_ref = [&](const syntax::SyntaxObj *binder) {
    auto it = out.binder_depth.find(binder);
    if (it != out.binder_depth.end() && it->second < lambda_depth) {
      out.captured_binders.insert(binder);
    }
  };

  switch (node->core.kind) {
    case CoreFormKind::PassThrough:
      if (node->children.size() == 1) {
        return resolve_node(node->children[0], env, prims, out, lambda_depth);
      }
      return bad_core_role("expected 1 child");

    case CoreFormKind::Paren:
      if (valid_idx(node->core.inner)) {
        return resolve_node(node->children[static_cast<size_t>(node->core.inner)], env, prims, out,
                            lambda_depth);
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
        note_ref(*bound);
        return true;
      }
      if (id->text == "true" || id->text == "false") {
        return true;
      }
      if (prims) {
        if (const AdtVariant *v = prims->find_variant(id->text);
            v && (v->private_scope == 0 || id->scopes.contains(v->private_scope))) {
          if (v->fields.empty()) return true;
          std::fprintf(stderr,
                       "%s:%d:%d: error: variant constructor '%s' requires %zu argument(s)\n",
                       syntax::file_or_input(id), id->line, id->col, id->text.c_str(), v->fields.size());
          return false;
        }
      }
      if (std::string note = private_variant_note(prims, id->text); !note.empty()) {
        std::fprintf(stderr, "langwb: %s%d:%d: %s\n", syntax::file_prefix(id).c_str(), id->line,
                     id->col, note.c_str());
        return false;
      }
      std::fprintf(stderr, "langwb: %s%d:%d: unbound identifier '%s'", syntax::file_prefix(id).c_str(), id->line, id->col,
                   id->text.c_str());
      print_origin_suffix(id);
      return false;
    }

    case CoreFormKind::BinOp: {
      if (!valid_idx(node->core.lhs) || !valid_idx(node->core.op) || !valid_idx(node->core.rhs)) {
        return bad_core_role("missing or out-of-range 'lhs', 'op', or 'rhs' role");
      }
      return resolve_node(node->children[static_cast<size_t>(node->core.lhs)], env, prims, out,
                          lambda_depth) &&
             resolve_node(node->children[static_cast<size_t>(node->core.rhs)], env, prims, out,
                          lambda_depth);
    }

    case CoreFormKind::If: {
      if (!valid_idx(node->core.test) || !valid_idx(node->core.then_b) ||
          !valid_idx(node->core.else_b)) {
        return bad_core_role("missing or out-of-range 'test', 'then', or 'else' role");
      }
      return resolve_node(node->children[static_cast<size_t>(node->core.test)], env, prims, out,
                          lambda_depth) &&
             resolve_node(node->children[static_cast<size_t>(node->core.then_b)], env, prims, out,
                          lambda_depth) &&
             resolve_node(node->children[static_cast<size_t>(node->core.else_b)], env, prims, out,
                          lambda_depth);
    }

    case CoreFormKind::Record: {
      if (!valid_idx(node->core.fields)) {
        return bad_core_role("missing or out-of-range 'fields' role");
      }
      std::vector<std::pair<std::string, const syntax::SyntaxObj *>> fields;
      collect_fields(node->children[static_cast<size_t>(node->core.fields)], fields);
      for (const auto &[fname, fexpr] : fields) {
        if (!resolve_node(fexpr, env, prims, out, lambda_depth)) return false;
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
      return resolve_node(node->children[static_cast<size_t>(node->core.obj)], env, prims, out,
                          lambda_depth);
    }

    case CoreFormKind::Let: {
      if (!valid_idx(node->core.bind) || !valid_idx(node->core.rhs) ||
          !valid_idx(node->core.body)) {
        return bad_core_role("missing or out-of-range 'bind', 'rhs', or 'body' role");
      }
      if (!resolve_node(node->children[static_cast<size_t>(node->core.rhs)], env, prims, out,
                        lambda_depth)) {
        return false;
      }
      const syntax::SyntaxObj *binder =
          unwrap_ident_node(node->children[static_cast<size_t>(node->core.bind)]);
      if (!binder) return bad_core_role("'bind' role must be an identifier");
      out.binder_depth[binder] = lambda_depth;
      ResolveEnv inner = env;
      inner.bind(binder->text, binder->scopes, binder);
      return resolve_node(node->children[static_cast<size_t>(node->core.body)], inner, prims, out,
                          lambda_depth);
    }

    case CoreFormKind::LetRec: {
      if (!valid_idx(node->core.bind) || !valid_idx(node->core.rhs) ||
          !valid_idx(node->core.body)) {
        return bad_core_role("missing or out-of-range 'bind', 'rhs', or 'body' role");
      }
      const syntax::SyntaxObj *binder =
          unwrap_ident_node(node->children[static_cast<size_t>(node->core.bind)]);
      if (!binder) return bad_core_role("'bind' role must be an identifier");
      out.binder_depth[binder] = lambda_depth;
      if (unwrap_lambda_node(node->children[static_cast<size_t>(node->core.rhs)])) {
        out.def_fn_binders.insert(binder);
      }
      ResolveEnv inner = env;
      inner.bind(binder->text, binder->scopes, binder);
      return resolve_node(node->children[static_cast<size_t>(node->core.rhs)], inner, prims, out,
                          lambda_depth) &&
             resolve_node(node->children[static_cast<size_t>(node->core.body)], inner, prims, out,
                          lambda_depth);
    }

    case CoreFormKind::Lambda: {
      if (!valid_idx(node->core.bind) || !valid_idx(node->core.body)) {
        return bad_core_role("missing or out-of-range 'bind' or 'body' role");
      }
      std::vector<const syntax::SyntaxObj *> params;
      collect_params(node->children[static_cast<size_t>(node->core.bind)], params);
      ResolveEnv inner = env;
      for (const syntax::SyntaxObj *p : params) {
        out.binder_depth[p] = lambda_depth + 1;
        inner.bind(p->text, p->scopes, p);
      }
      return resolve_node(node->children[static_cast<size_t>(node->core.body)], inner, prims, out,
                          lambda_depth + 1);
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
          note_ref(*bound);
        } else if (id->text == "__wb_is_variant" || id->text == "__wb_variant_field") {
          // Built-in ADT match helpers lowered directly in lower_expr.
          if (id->text == "__wb_is_variant" && prims && node->core.args >= 0 &&
              static_cast<size_t>(node->core.args) < node->children.size()) {
            std::vector<const syntax::SyntaxObj *> args;
            collect_call_args(node->children[static_cast<size_t>(node->core.args)], args);
            if (args.size() >= 2) {
              const syntax::SyntaxObj *vn = args[1];
              while (vn && !vn->is_terminal && vn->children.size() == 1) vn = vn->children[0];
              std::string vname;
              if (vn && vn->text.size() >= 2 && vn->text.front() == '"' && vn->text.back() == '"') {
                vname = vn->text.substr(1, vn->text.size() - 2);
              }
              const AdtVariant *v = prims->find_variant(vname);
              if (!v || (v->private_scope != 0 && !node->scopes.contains(v->private_scope))) {
                std::string note = private_variant_note(prims, vname);
                if (!note.empty()) {
                  std::fprintf(stderr, "%s:%d:%d: error: %s\n", syntax::file_or_input(node),
                               node->line, node->col, note.c_str());
                } else {
                  std::fprintf(stderr, "%s:%d:%d: error: unknown variant '%s' in match pattern\n",
                               syntax::file_or_input(node), node->line, node->col, vname.c_str());
                }
                return false;
              }
              if (args.size() >= 3) {
                const syntax::SyntaxObj *an = args[2];
                while (an && !an->is_terminal && an->children.size() == 1) an = an->children[0];
                if (an && !an->text.empty()) {
                  size_t pat_arity = static_cast<size_t>(std::strtoull(an->text.c_str(), nullptr, 10));
                  if (pat_arity != v->fields.size()) {
                    // Optional 4th argument: the pattern as written (`Some(a, b)`).
                    std::string shown = vname;
                    if (args.size() >= 4) {
                      const syntax::SyntaxObj *pn = args[3];
                      while (pn && !pn->is_terminal && pn->children.size() == 1) pn = pn->children[0];
                      if (pn && pn->text.size() >= 2 && pn->text.front() == '"') {
                        shown = pn->text.substr(1, pn->text.size() - 2);
                      }
                    }
                    std::string hint;
                    if (pat_arity == 0 && args.size() < 4) {
                      hint = "; write '" + vname + "(";
                      for (size_t fi = 0; fi < v->fields.size(); ++fi) hint += fi ? ", _" : "_";
                      hint += ")' to ignore the field(s)";
                    }
                    std::fprintf(stderr,
                                 "%s:%d:%d: error: pattern '%s' has %zu field(s); variant "
                                 "'%s' has %zu%s\n",
                                 syntax::file_or_input(node), node->line, node->col, shown.c_str(),
                                 pat_arity, vname.c_str(), v->fields.size(), hint.c_str());
                    return false;
                  }
                }
              }
            }
          }
        } else if (prims && prims->find_variant(id->text) &&
                   (prims->find_variant(id->text)->private_scope == 0 ||
                    id->scopes.contains(prims->find_variant(id->text)->private_scope))) {
          const AdtVariant *v = prims->find_variant(id->text);
          std::vector<const syntax::SyntaxObj *> args;
          if (node->core.args >= 0 && static_cast<size_t>(node->core.args) < node->children.size()) {
            collect_call_args(node->children[static_cast<size_t>(node->core.args)], args);
          }
          if (args.size() != v->fields.size()) {
            std::fprintf(stderr,
                         "%s:%d:%d: error: wrong number of arguments to variant '%s': "
                         "expected %zu, got %zu\n",
                         syntax::file_or_input(node), node->line, node->col, id->text.c_str(), v->fields.size(), args.size());
            return false;
          }
        } else if (prims && prims->has(id->text)) {
          out.prim_calls[node->node_id] = id->text;
        } else if (std::string note = private_variant_note(prims, id->text); !note.empty()) {
          std::fprintf(stderr, "langwb: %s%d:%d: %s\n", syntax::file_prefix(id).c_str(), id->line,
                       id->col, note.c_str());
          return false;
        } else {
          std::fprintf(stderr, "langwb: %s%d:%d: unbound identifier '%s'", syntax::file_prefix(id).c_str(), id->line, id->col,
                       id->text.c_str());
          print_origin_suffix(id);
          return false;
        }
      } else {
        if (!resolve_node(fn_node, env, prims, out, lambda_depth)) return false;
      }
      if (node->core.args >= 0 && static_cast<size_t>(node->core.args) < node->children.size()) {
        std::vector<const syntax::SyntaxObj *> args;
        collect_call_args(node->children[static_cast<size_t>(node->core.args)], args);
        for (const syntax::SyntaxObj *a : args) {
          if (!resolve_node(a, env, prims, out, lambda_depth)) return false;
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
          out.binder_depth[binder] = lambda_depth;
          if (u->core.rhs >= 0 && static_cast<size_t>(u->core.rhs) < u->children.size() &&
              unwrap_lambda_node(u->children[static_cast<size_t>(u->core.rhs)])) {
            out.def_fn_binders.insert(binder);
          }
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
          if (!resolve_node(u->children[static_cast<size_t>(u->core.rhs)], inner, prims, out,
                            lambda_depth)) {
            return false;
          }
        } else {
          if (!resolve_node(u, inner, prims, out, lambda_depth)) return false;
        }
      }
      return true;
    }

    case CoreFormKind::Define:
      std::fprintf(stderr, "langwb: %s%d:%d: 'def' is only allowed inside a sequence block",
                   syntax::file_prefix(node).c_str(), node->line, node->col);
      print_origin_suffix(node);
      return false;

    case CoreFormKind::Assign: {
      if (!valid_idx(node->core.lhs) || !valid_idx(node->core.rhs)) {
        return bad_core_role("missing or out-of-range 'lhs' or 'rhs' role");
      }
      const syntax::SyntaxObj *id =
          unwrap_ident_node(node->children[static_cast<size_t>(node->core.lhs)]);
      if (!id) return bad_core_role("expected identifier on assignment lhs");
      auto bound = env.resolve(id->text, id->scopes);
      if (!bound) {
        std::fprintf(stderr, "langwb: %s%d:%d: unbound identifier '%s'", syntax::file_prefix(id).c_str(), id->line, id->col,
                     id->text.c_str());
        print_origin_suffix(id);
        return false;
      }
      if (out.def_fn_binders.count(*bound)) {
        std::fprintf(stderr, "langwb: %s%d:%d: cannot assign to function definition '%s'", syntax::file_prefix(id).c_str(), id->line,
                     id->col, id->text.c_str());
        print_origin_suffix(id);
        return false;
      }
      out.ref_to_binder[node->children[static_cast<size_t>(node->core.lhs)]->node_id] = *bound;
      out.ref_to_binder[id->node_id] = *bound;
      out.assigned_binders.insert(*bound);
      note_ref(*bound);
      return resolve_node(node->children[static_cast<size_t>(node->core.rhs)], env, prims, out,
                          lambda_depth);
    }

    case CoreFormKind::SetField: {
      if (!valid_idx(node->core.obj) || !valid_idx(node->core.field) ||
          !valid_idx(node->core.rhs)) {
        return bad_core_role("missing or out-of-range 'obj', 'field', or 'rhs' role");
      }
      // `_tag` is the synthesized variant field of ADT values; overwriting it
      // would make `match` and `==` see the wrong variant.
      if (const syntax::SyntaxObj *fld =
              unwrap_ident_node(node->children[static_cast<size_t>(node->core.field)]);
          fld && fld->text == "_tag") {
        std::fprintf(stderr,
                     "langwb: %s%d:%d: cannot assign to '_tag': it is reserved for the variant "
                     "of ADT values",
                     syntax::file_prefix(fld).c_str(), fld->line, fld->col);
        print_origin_suffix(fld);
        return false;
      }
      return resolve_node(node->children[static_cast<size_t>(node->core.obj)], env, prims, out,
                          lambda_depth) &&
             resolve_node(node->children[static_cast<size_t>(node->core.rhs)], env, prims, out,
                          lambda_depth);
    }

    case CoreFormKind::While: {
      if (!valid_idx(node->core.test) || !valid_idx(node->core.body)) {
        return bad_core_role("missing or out-of-range 'test' or 'body' role");
      }
      return resolve_node(node->children[static_cast<size_t>(node->core.test)], env, prims, out,
                          lambda_depth) &&
             resolve_node(node->children[static_cast<size_t>(node->core.body)], env, prims, out,
                          lambda_depth);
    }

    case CoreFormKind::UsingSyntax:
      std::fprintf(stderr, "langwb: %s%d:%d: unexpanded 'using syntax' region\n", syntax::file_prefix(node).c_str(), node->line,
                   node->col);
      return false;

    case CoreFormKind::Amb:
      std::fprintf(stderr, "langwb: %s%d:%d: unresolved syntax ambiguity (%zu alternatives) for '%s'\n",
                   syntax::file_prefix(node).c_str(), node->line, node->col, node->children.size(), node->text.c_str());
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
      note_ref(*bound);
      return true;
    }
    std::fprintf(stderr, "langwb: %s%d:%d: unbound identifier '%s'", syntax::file_prefix(node).c_str(), node->line, node->col,
                 tok.c_str());
    print_origin_suffix(node);
    return false;
  }

  // Unwrap single-child non-terminal wrapper if untagged (e.g. `program -> expr`).
  if (node->children.size() == 1 && !node->children[0]->is_terminal) {
    return resolve_node(node->children[0], env, prims, out, lambda_depth);
  }

  std::fprintf(stderr, "langwb: %s%d:%d: unexpanded syntax form '%s' (no macro or core(...) rule)",
               syntax::file_prefix(node).c_str(), node->line, node->col, node->symbol.c_str());
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

// A literal `if`/`while` condition has no name, so IFA's non-bool check would
// say `expression illegal: int64`. Move it into a local named for the construct
// so the message reads `'while condition (expected bool)' illegal: int64`.
// Only literals are renamed: a computed condition may be an `isinstance` result
// whose branch narrowing depends on the IF testing it directly.
Sym *name_literal_condition(Sym *cond, Code **cond_code, const syntax::SyntaxObj *test_node,
                            const char *construct, LowerCtx &lctx) {
  if (!cond || !cond->is_constant || cond->type == sym_bool) return cond;
  std::string label = std::string(construct) + " condition (expected bool)";
  Sym *named = new_local(lctx, if1_cannonicalize_string(if1, label.c_str()));
  attach_loc(named, test_node);
  Code *mv = if1_move(if1, cond_code, cond, named);
  attach_loc(mv, test_node, named);
  return named;
}

Sym *make_box(Sym *init_val, const syntax::SyntaxObj *loc_node, Code **code, LowerCtx &lctx) {
  Sym *box_type = new_Sym("record { _val }");
  attach_loc(box_type, loc_node);
  box_type->type_kind = Type_RECORD;
  box_type->implements.add(sym_object);
  box_type->specializes.add(sym_object);
  Sym *fsym = new_Sym("_val");
  attach_loc(fsym, loc_node);
  fsym->in = box_type;
  box_type->has.add(fsym);

  Sym *box = new_local(lctx);
  attach_loc(box, loc_node);
  Code *mk = if1_send(if1, code, 3, 1, sym_primitive, sym_new, box_type, box);
  attach_loc(mk, loc_node, box);
  Sym *sres = new_local(lctx);
  Code *st = if1_send(if1, code, 5, 1, sym_operator, box, sym_setter,
                      if1_make_symbol(if1, "_val"), init_val, sres);
  attach_loc(st, loc_node, sres);
  lctx.boxed_syms.insert(box);
  return box;
}

Sym *unbox_read(Sym *box, const syntax::SyntaxObj *loc_node, Code **code, LowerCtx &lctx) {
  Sym *val = new_local(lctx);
  attach_loc(val, loc_node);
  Code *rd = if1_send(if1, code, 4, 1, sym_operator, box, sym_period,
                      if1_make_symbol(if1, "_val"), val);
  attach_loc(rd, loc_node, val);
  return val;
}

void box_write(Sym *box, Sym *rhs, const syntax::SyntaxObj *loc_node, Code **code, LowerCtx &lctx) {
  Sym *sres = new_local(lctx);
  attach_loc(sres, loc_node);
  Code *st = if1_send(if1, code, 5, 1, sym_operator, box, sym_setter,
                      if1_make_symbol(if1, "_val"), rhs, sres);
  attach_loc(st, loc_node, sres);
}

Sym *lower_expr(const syntax::SyntaxObj *node, Code **code, const Env &env, LowerCtx &lctx);

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

bool collect_free_captures(const syntax::SyntaxObj *node, const Env &outer_env,
                           const LowerCtx &lctx,
                           const std::unordered_set<uint64_t> &local_binders,
                           std::vector<CapturedVar> &out, std::unordered_set<Sym *> &seen_syms,
                           const std::unordered_map<Sym *, size_t> *sibling_defs = nullptr,
                           std::unordered_set<size_t> *out_sibling_calls = nullptr) {
  if (!node || !lctx.resolved) return true;
  if (auto it = lctx.resolved->ref_to_binder.find(node->node_id);
      it != lctx.resolved->ref_to_binder.end()) {
    const syntax::SyntaxObj *binder = it->second;
    if (binder && !local_binders.count(binder->node_id)) {
      if (auto bound = outer_env.resolve(binder->text, binder->scopes)) {
        Sym *outer_sym = *bound;
        if (outer_sym && !outer_sym->is_fun && seen_syms.insert(outer_sym).second) {
          out.push_back(CapturedVar{
              .name = binder->text, .binder_scopes = binder->scopes, .outer_sym = outer_sym});
        } else if (outer_sym && outer_sym->is_fun) {
          if (sibling_defs && out_sibling_calls) {
            if (auto sit = sibling_defs->find(outer_sym); sit != sibling_defs->end()) {
              out_sibling_calls->insert(sit->second);
            }
          }
          // If calling a recursive/mutually-recursive function that expects hidden
          // capture parameters, transitively capture those outer variables here too.
          if (auto rit = lctx.rec_self_caps.find(outer_sym); rit != lctx.rec_self_caps.end()) {
            for (const auto &rcap : rit->second) {
              if (auto rb = outer_env.resolve(rcap.name, rcap.binder_scopes)) {
                Sym *rsym = *rb;
                if (rsym && !rsym->is_fun && seen_syms.insert(rsym).second) {
                  out.push_back(CapturedVar{.name = rcap.name,
                                            .binder_scopes = rcap.binder_scopes,
                                            .outer_sym = rsym});
                }
              } else {
                std::fprintf(stderr,
                             "langwb: %s%d:%d: internal error: missing captured variable '%s' for "
                             "recursive callee '%s'\n",
                             syntax::file_prefix(node).c_str(), node->line, node->col, rcap.name.c_str(),
                             outer_sym->name ? outer_sym->name : "<fn>");
                return false;
              }
            }
          }
        }
      }
    }
    return true;
  }
  for (const syntax::SyntaxObj *c : node->children) {
    if (!collect_free_captures(c, outer_env, lctx, local_binders, out, seen_syms, sibling_defs,
                               out_sibling_calls)) {
      return false;
    }
  }
  return true;
}

Sym *lower_lambda(const syntax::SyntaxObj *node, Code **enclosing_code, const Env &env,
                  LowerCtx &lctx, Sym *prealloc_fn = nullptr,
                  const syntax::SyntaxObj *rec_binder = nullptr,
                  const std::vector<CapturedVar> *forced_captures = nullptr) {
  std::vector<const syntax::SyntaxObj *> param_nodes;
  collect_params(node->children[static_cast<size_t>(node->core.bind)], param_nodes);

  const syntax::SyntaxObj *body_node = node->children[static_cast<size_t>(node->core.body)];

  // Identify free variables captured from enclosing lexical scopes.
  std::vector<CapturedVar> captures;
  if (forced_captures) {
    captures = *forced_captures;
  } else {
    std::unordered_set<uint64_t> local_binders;
    for (const syntax::SyntaxObj *pn : param_nodes) local_binders.insert(pn->node_id);
    if (rec_binder) local_binders.insert(rec_binder->node_id);
    collect_local_binder_ids(body_node, local_binders);

    std::unordered_set<Sym *> seen_syms;
    if (!collect_free_captures(body_node, env, lctx, local_binders, captures, seen_syms)) {
      return nullptr;
    }
  }

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
  for (const auto &cap : captures) {
    Sym *cs = new_Sym(cap.name.c_str());
    cs->is_local = 1;
    cs->nesting_depth = LOCALLY_NESTED;
    cs->in = fn;
    closure_args.push_back(cs);
    if (lctx.boxed_syms.count(cap.outer_sym)) {
      lctx.boxed_syms.insert(cs);
    }
    inner.bind(cap.name, cap.binder_scopes, cs);
  }

  Code *fn_code = nullptr;
  Sym *saved_enclosing = lctx.enclosing_fn;
  lctx.enclosing_fn = fn;

  for (const syntax::SyntaxObj *pn : param_nodes) {
    Sym *ps = new_Sym(pn->text.c_str());
    attach_loc(ps, pn);
    ps->is_local = 1;
    ps->nesting_depth = LOCALLY_NESTED;
    ps->in = fn;
    closure_args.push_back(ps);
    if (lctx.resolved && lctx.resolved->is_boxed(pn)) {
      Sym *box = make_box(ps, pn, &fn_code, lctx);
      inner.bind(pn->text, pn->scopes, box);
    } else {
      inner.bind(pn->text, pn->scopes, ps);
    }
  }

  // If this is a recursive lambda that captures outer variables, record `captures`
  // so direct self-calls (including from nested lambdas) resolve and pass them explicitly.
  bool added_rec_caps = false;
  if (rec_binder && !captures.empty() && !forced_captures) {
    lctx.rec_self_caps[fn] = captures;
    added_rec_caps = true;
  }

  lctx.fn_arity[fn] = param_nodes.size();
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
  lctx.fn_arity[cls] = param_nodes.size();
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
      std::string unquoted;
      if (t->text.size() >= 2 && t->text.front() == '"' && t->text.back() == '"') {
        std::string_view inner(t->text.data() + 1, t->text.size() - 2);
        unquoted.reserve(inner.size());
        for (size_t i = 0; i < inner.size(); ++i) {
          if (inner[i] == '\\' && i + 1 < inner.size()) {
            char next = inner[++i];
            if (next == 'n') unquoted.push_back('\n');
            else if (next == 't') unquoted.push_back('\t');
            else unquoted.push_back(next);
          } else {
            unquoted.push_back(inner[i]);
          }
        }
      } else {
        unquoted = t->text;
      }
      return record(if1_const(if1, sym_string, unquoted.c_str(), nullptr, nullptr,
                              static_cast<int>(unquoted.size())));
    }

    case CoreFormKind::Ident: {
      const syntax::SyntaxObj *id = unwrap_ident_node(node);
      if (!id) return nullptr;
      if (auto bound = env.resolve(id->text, id->scopes)) {
        if (lctx.boxed_syms.count(*bound)) {
          return record(unbox_read(*bound, node, code, lctx));
        }
        return record(*bound);
      }
      if (id->text == "true") return record(sym_true);
      if (id->text == "false") return record(sym_false);
      if (lctx.prims) {
        if (const AdtVariant *v = lctx.prims->find_variant(id->text); v && v->fields.empty()) {
          auto vit = lctx.adt_variant_syms.find(v->name);
          if (vit != lctx.adt_variant_syms.end()) {
            Sym *obj = new_local(lctx);
            attach_loc(obj, node);
            Code *new_send = if1_send(if1, code, 3, 1, sym_primitive, sym_new, vit->second, obj);
            attach_loc(new_send, node, obj);
            Sym *sres = new_local(lctx);
            Code *set_send = if1_send(if1, code, 5, 1, sym_operator, obj, sym_setter,
                                      if1_make_symbol(if1, "_tag"),
                                      if1_const(if1, sym_int64, "0"), sres);
            attach_loc(set_send, node, sres);
            return record(obj);
          }
        }
      }
      std::fprintf(stderr, "langwb: %s%d:%d: unbound identifier '%s'", syntax::file_prefix(id).c_str(), id->line, id->col,
                   id->text.c_str());
      print_origin_suffix(id);
      return nullptr;
    }

    case CoreFormKind::Record: {
      std::vector<std::pair<std::string, const syntax::SyntaxObj *>> fields;
      collect_fields(node->children[static_cast<size_t>(node->core.fields)], fields);

      std::string rec_name = "record { ";
      for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) rec_name += ", ";
        rec_name += fields[i].first;
      }
      rec_name += " }";
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
      Sym *res = new_local(lctx);
      attach_loc(res, op_node);
      if ((op_node->text == "==" || op_node->text == "!=") && PrimRegistry::active() &&
          PrimRegistry::active()->has("wb_eq")) {
        const char *pname = (op_node->text == "==") ? "wb_eq" : "wb_ne";
        Code *eq_send = if1_send(if1, code, 4, 1, sym_primitive, if1_make_symbol(if1, pname),
                                 lhs, rhs, res);
        attach_loc(eq_send, op_node, res);
        return record(res);
      }
      Sym *op = if1_make_symbol(if1, op_node->text.c_str());
      Code *bin_send = if1_send(if1, code, 4, 1, sym_operator, lhs, op, rhs, res);
      attach_loc(bin_send, op_node, res);
      return record(res);
    }

    case CoreFormKind::App: {
      std::vector<const syntax::SyntaxObj *> arg_nodes;
      if (node->core.args >= 0 && static_cast<size_t>(node->core.args) < node->children.size()) {
        collect_call_args(node->children[static_cast<size_t>(node->core.args)], arg_nodes);
      }

      // Built-in ADT helpers (`__wb_is_variant`, `__wb_variant_field`) and ADT constructors.
      if (const syntax::SyntaxObj *fn_id =
              unwrap_ident_node(node->children[static_cast<size_t>(node->core.fn)]);
          fn_id && !env.resolve(fn_id->text, fn_id->scopes)) {
        if (fn_id->text == "__wb_is_variant" && (arg_nodes.size() == 3 || arg_nodes.size() == 4)) {
          Sym *scrut = lower_expr(arg_nodes[0], code, env, lctx);
          if (!scrut) return nullptr;
          std::string vname;
          const syntax::SyntaxObj *vn = arg_nodes[1];
          while (vn && !vn->is_terminal && vn->children.size() == 1) vn = vn->children[0];
          if (vn && vn->text.size() >= 2 && vn->text.front() == '"' && vn->text.back() == '"') {
            vname = vn->text.substr(1, vn->text.size() - 2);
          }
          auto vit = lctx.adt_variant_syms.find(vname);
          if (vit == lctx.adt_variant_syms.end()) {
            std::fprintf(stderr, "%s:%d:%d: error: unknown variant '%s' in match pattern\n",
                         syntax::file_or_input(node), node->line, node->col, vname.c_str());
            return nullptr;
          }
          if (lctx.prims && lctx.adt_checked_scruts.insert(scrut).second) {
            if (const AdtVariant *v = lctx.prims->find_variant(vname)) {
              // Report a non-ADT scrutinee once, at the `match` itself, under a
              // readable name instead of the template's `__m__`.
              const syntax::SyntaxObj *src = find_located_node(node);
              bool at_match = src && src->origin_macro == "match_expr" && src->origin_line > 0;
              auto match_ast = [&](Sym *sym) {
                WbAST *ast = make_wb_ast(node, sym);
                if (at_match) {
                  ast->filename = src->file ? src->file : "<input>";
                  ast->line_no = src->origin_line;
                  ast->col_no = src->origin_col;
                }
                return ast;
              };
              std::string label = "match scrutinee (expected " + v->type_name + ")";
              Sym *named = new_local(lctx, if1_cannonicalize_string(if1, label.c_str()));
              named->ast = match_ast(named);
              Code *mv = if1_move(if1, code, scrut, named);
              mv->ast = match_ast(named);
              Sym *tname_sym = if1_const(if1, sym_string, v->type_name.c_str(), nullptr, nullptr,
                                         static_cast<int>(v->type_name.size()));
              Sym *chk_res = new_local(lctx);
              Code *chk_send = if1_send(if1, code, 4, 1, sym_primitive,
                                        if1_make_symbol(if1, "wb_check_adt"), named, tname_sym,
                                        chk_res);
              chk_send->ast = match_ast(chk_res);
            }
          }
          Sym *res = new_local(lctx);
          attach_loc(res, node);
          Code *is_send = if1_send(if1, code, 4, 1, sym_primitive,
                                   if1_make_symbol(if1, "isinstance"), scrut, vit->second, res);
          attach_loc(is_send, node, res);
          return record(res);
        }
        if (fn_id->text == "__wb_variant_field" && arg_nodes.size() == 3) {
          Sym *scrut = lower_expr(arg_nodes[0], code, env, lctx);
          if (!scrut) return nullptr;
          std::string vname;
          const syntax::SyntaxObj *vn = arg_nodes[1];
          while (vn && !vn->is_terminal && vn->children.size() == 1) vn = vn->children[0];
          if (vn && vn->text.size() >= 2 && vn->text.front() == '"' && vn->text.back() == '"') {
            vname = vn->text.substr(1, vn->text.size() - 2);
          }
          const syntax::SyntaxObj *in = arg_nodes[2];
          while (in && !in->is_terminal && in->children.size() == 1) in = in->children[0];
          size_t fidx = in ? static_cast<size_t>(std::strtoull(in->text.c_str(), nullptr, 10)) : 0;
          const AdtVariant *v = lctx.prims ? lctx.prims->find_variant(vname) : nullptr;
          if (!v || fidx >= v->fields.size()) {
            std::fprintf(stderr, "%s:%d:%d: error: invalid field index %zu for variant '%s'\n",
                         syntax::file_or_input(node), node->line, node->col, fidx, vname.c_str());
            return nullptr;
          }
          Sym *res = new_local(lctx);
          attach_loc(res, node);
          Code *fld_send = if1_send(if1, code, 4, 1, sym_operator, scrut, sym_period,
                                    if1_make_symbol(if1, v->fields[fidx].c_str()), res);
          attach_loc(fld_send, node, res);
          return record(res);
        }
        if (!env.resolve(fn_id->text, fn_id->scopes) && lctx.prims) {
          if (const AdtVariant *v = lctx.prims->find_variant(fn_id->text)) {
            auto vit = lctx.adt_variant_syms.find(v->name);
            if (vit != lctx.adt_variant_syms.end()) {
              std::vector<Sym *> arg_syms;
              arg_syms.reserve(arg_nodes.size());
              for (const syntax::SyntaxObj *an : arg_nodes) {
                Sym *as = lower_expr(an, code, env, lctx);
                if (!as) return nullptr;
                arg_syms.push_back(as);
              }
              Sym *obj = new_local(lctx);
              attach_loc(obj, node);
              Code *new_send = if1_send(if1, code, 3, 1, sym_primitive, sym_new, vit->second, obj);
              attach_loc(new_send, node, obj);
              if (v->fields.empty()) {
                Sym *sres = new_local(lctx);
                Code *set_send = if1_send(if1, code, 5, 1, sym_operator, obj, sym_setter,
                                          if1_make_symbol(if1, "_tag"),
                                          if1_const(if1, sym_int64, "0"), sres);
                attach_loc(set_send, node, sres);
              } else {
                for (size_t i = 0; i < v->fields.size(); ++i) {
                  Sym *sres = new_local(lctx);
                  Code *set_send = if1_send(if1, code, 5, 1, sym_operator, obj, sym_setter,
                                            if1_make_symbol(if1, v->fields[i].c_str()), arg_syms[i],
                                            sres);
                  attach_loc(set_send, arg_nodes[i], sres);
                }
              }
              return record(obj);
            }
          }
        }
      }

      auto pit = lctx.resolved ? lctx.resolved->prim_calls.find(node->node_id)
                               : std::unordered_map<uint64_t, std::string>::const_iterator{};
      bool is_prim = (lctx.resolved && pit != lctx.resolved->prim_calls.end());
      Sym *callee_sym = nullptr;
      if (!is_prim) {
        callee_sym =
            lower_expr(node->children[static_cast<size_t>(node->core.fn)], code, env, lctx);
        if (!callee_sym) return nullptr;
        if (auto ait = lctx.fn_arity.find(callee_sym);
            ait != lctx.fn_arity.end() && arg_nodes.size() != ait->second) {
          const syntax::SyntaxObj *fn_node = node->children[static_cast<size_t>(node->core.fn)];
          const syntax::SyntaxObj *fn_id = unwrap_ident_node(fn_node);
          std::string label = fn_id ? fn_id->text : "<fn>";
          std::fprintf(stderr,
                       "%s:%d:%d: error: wrong number of arguments to '%s': expected %zu, "
                       "got %zu\n",
                       syntax::file_or_input(node), node->line, node->col, label.c_str(), ait->second, arg_nodes.size());
          return nullptr;
        }
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
          for (const auto &rcap : rit->second) {
            if (auto rb = env.resolve(rcap.name, rcap.binder_scopes)) {
              if1_add_send_arg(if1, send, *rb);
            } else {
              std::fprintf(stderr,
                           "langwb: %s%d:%d: internal error: missing captured variable '%s' for "
                           "recursive call to '%s'\n",
                           syntax::file_prefix(node).c_str(), node->line, node->col, rcap.name.c_str(),
                           callee_sym->name ? callee_sym->name : "<fn>");
              return nullptr;
            }
          }
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
      cond_sym = name_literal_condition(
          cond_sym, &cond_code, node->children[static_cast<size_t>(node->core.test)], "if", lctx);
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
      if (lctx.resolved && lctx.resolved->is_boxed(binder)) {
        bound = make_box(rhs, binder, code, lctx);
      } else if (!rhs->is_fun ||
                 (lctx.resolved && lctx.resolved->assigned_binders.count(binder))) {
        bound = new_local(lctx, binder->text.c_str());
        attach_loc(bound, binder);
        Code *mv = if1_move(if1, code, rhs, bound);
        attach_loc(mv, binder, bound);
        if (auto ait = lctx.fn_arity.find(rhs); ait != lctx.fn_arity.end()) {
          lctx.fn_arity[bound] = ait->second;
        }
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

      // Compute per-`def` capture sets via a call-graph fixpoint within this Seq
      // block so capture-free siblings remain plain functions while mutually
      // recursive siblings that transitively reach a capture agree on hidden args.
      std::unordered_map<Sym *, size_t> sibling_defs;
      for (size_t di = 0; di < defs.size(); ++di) {
        if (defs[di].lam_node) sibling_defs[defs[di].sym] = di;
      }
      std::vector<std::vector<CapturedVar>> per_def_caps(defs.size());
      std::vector<std::unordered_set<Sym *>> per_def_seen(defs.size());
      std::vector<std::unordered_set<size_t>> per_def_calls(defs.size());
      for (size_t di = 0; di < defs.size(); ++di) {
        const DefSlot &slot = defs[di];
        if (!slot.lam_node) continue;
        std::vector<const syntax::SyntaxObj *> pnodes;
        collect_params(slot.lam_node->children[static_cast<size_t>(slot.lam_node->core.bind)],
                       pnodes);
        const syntax::SyntaxObj *bnode =
            slot.lam_node->children[static_cast<size_t>(slot.lam_node->core.body)];
        std::unordered_set<uint64_t> local_b;
        for (const syntax::SyntaxObj *pn : pnodes) local_b.insert(pn->node_id);
        collect_local_binder_ids(bnode, local_b);
        if (!collect_free_captures(bnode, inner, lctx, local_b, per_def_caps[di], per_def_seen[di],
                                   &sibling_defs, &per_def_calls[di])) {
          return nullptr;
        }
      }
      bool changed = true;
      while (changed) {
        changed = false;
        for (size_t di = 0; di < defs.size(); ++di) {
          if (!defs[di].lam_node) continue;
          for (size_t dj : per_def_calls[di]) {
            for (const auto &cap : per_def_caps[dj]) {
              if (per_def_seen[di].insert(cap.outer_sym).second) {
                per_def_caps[di].push_back(cap);
                changed = true;
              }
            }
          }
        }
      }
      for (size_t di = 0; di < defs.size(); ++di) {
        if (defs[di].lam_node && !per_def_caps[di].empty()) {
          lctx.rec_self_caps[defs[di].sym] = per_def_caps[di];
        }
      }

      // Pass 2a: lower all non-lambda, non-alias `def` initializers first so
      // closures in Pass 2b capture already-initialized locals (e.g. `def secret = 1;`).
      for (size_t di = 0; di < defs.size(); ++di) {
        const DefSlot &slot = defs[di];
        if (slot.lam_node || slot.def_node->symbol == "import_alias") continue;
        const syntax::SyntaxObj *binder =
            unwrap_ident_node(slot.def_node->children[static_cast<size_t>(slot.def_node->core.bind)]);
        Sym *rhs_val = lower_expr(
            slot.def_node->children[static_cast<size_t>(slot.def_node->core.rhs)], code, inner,
            lctx);
        if (!rhs_val) return nullptr;
        if (lctx.resolved && lctx.resolved->is_boxed(binder)) {
          Sym *box = make_box(rhs_val, slot.def_node, code, lctx);
          Code *mv = if1_move(if1, code, box, slot.sym);
          attach_loc(mv, slot.def_node, slot.sym);
          lctx.boxed_syms.insert(slot.sym);
        } else {
          Code *mv = if1_move(if1, code, rhs_val, slot.sym);
          attach_loc(mv, slot.def_node, slot.sym);
        }
      }

      // Pass 2b: lower all `def` lambda bodies and `import_alias` bindings in order
      // (imported modules precede their importers, so exported closures are lowered
      // before an importer's `import_alias` copies them into the importer's scope).
      std::vector<Sym *> lowered_lam_vals(defs.size(), nullptr);
      std::unordered_map<Sym *, Sym *> raw_to_lowered_lam;
      for (size_t di = 0; di < defs.size(); ++di) {
        const DefSlot &slot = defs[di];
        if (slot.lam_node) {
          const syntax::SyntaxObj *binder = unwrap_ident_node(
              slot.def_node->children[static_cast<size_t>(slot.def_node->core.bind)]);
          Sym *lam_val = lower_lambda(slot.lam_node, code, inner, lctx, slot.sym, binder,
                                      per_def_caps[di].empty() ? nullptr : &per_def_caps[di]);
          if (!lam_val) {
            for (const DefSlot &ds : defs) {
              if (ds.lam_node) lctx.rec_self_caps.erase(ds.sym);
            }
            return nullptr;
          }
          lowered_lam_vals[di] = lam_val;
          raw_to_lowered_lam[slot.sym] = lam_val;
        } else if (slot.def_node->symbol == "import_alias") {
          Sym *rhs_val = lower_expr(
              slot.def_node->children[static_cast<size_t>(slot.def_node->core.rhs)], code, inner,
              lctx);
          if (!rhs_val) return nullptr;
          if (auto lit = raw_to_lowered_lam.find(rhs_val); lit != raw_to_lowered_lam.end()) {
            rhs_val = lit->second;
          }
          Code *mv = if1_move(if1, code, rhs_val, slot.sym);
          attach_loc(mv, slot.def_node, slot.sym);
          if (auto ait = lctx.fn_arity.find(rhs_val); ait != lctx.fn_arity.end()) {
            lctx.fn_arity[slot.sym] = ait->second;
          }
        }
      }
      for (const DefSlot &slot : defs) {
        if (slot.lam_node) lctx.rec_self_caps.erase(slot.sym);
      }
      for (size_t di = 0; di < defs.size(); ++di) {
        const DefSlot &slot = defs[di];
        if (!slot.lam_node || !lowered_lam_vals[di] || lowered_lam_vals[di] == slot.sym) continue;
        const syntax::SyntaxObj *binder =
            unwrap_ident_node(slot.def_node->children[static_cast<size_t>(slot.def_node->core.bind)]);
        if (binder) inner.bind(binder->text, binder->scopes, lowered_lam_vals[di]);
      }

      // Pass 2c: lower remaining expression statements in order.
      size_t def_idx = 0;
      Sym *last_val = nullptr;
      for (const syntax::SyntaxObj *s : stmts) {
        const syntax::SyntaxObj *u = s;
        while (u && u->core.kind == CoreFormKind::PassThrough && u->children.size() == 1) {
          u = u->children[0];
        }
        if (u && u->core.kind == CoreFormKind::Define) {
          size_t cur_idx = def_idx++;
          const DefSlot &slot = defs[cur_idx];
          last_val = slot.lam_node ? lowered_lam_vals[cur_idx] : slot.sym;
        } else {
          last_val = lower_expr(u, code, inner, lctx);
          if (!last_val) return nullptr;
        }
      }
      return record(last_val ? last_val : if1_const(if1, sym_int64, "0"));
    }

    case CoreFormKind::Assign: {
      const syntax::SyntaxObj *id =
          unwrap_ident_node(node->children[static_cast<size_t>(node->core.lhs)]);
      if (!id) return nullptr;
      auto bound = env.resolve(id->text, id->scopes);
      if (!bound) {
        std::fprintf(stderr, "langwb: %s%d:%d: unbound identifier '%s'", syntax::file_prefix(id).c_str(), id->line, id->col,
                     id->text.c_str());
        print_origin_suffix(id);
        return nullptr;
      }
      Sym *rhs = lower_expr(node->children[static_cast<size_t>(node->core.rhs)], code, env, lctx);
      if (!rhs) return nullptr;
      if (lctx.boxed_syms.count(*bound)) {
        box_write(*bound, rhs, node, code, lctx);
      } else {
        Code *mv = if1_move(if1, code, rhs, *bound);
        attach_loc(mv, node, *bound);
      }
      return record(rhs);
    }

    case CoreFormKind::SetField: {
      Sym *obj = lower_expr(node->children[static_cast<size_t>(node->core.obj)], code, env, lctx);
      if (!obj) return nullptr;
      const syntax::SyntaxObj *fld =
          unwrap_ident_node(node->children[static_cast<size_t>(node->core.field)]);
      if (!fld) return nullptr;
      Sym *val = lower_expr(node->children[static_cast<size_t>(node->core.rhs)], code, env, lctx);
      if (!val) return nullptr;
      Sym *res = new_local(lctx);
      attach_loc(res, fld);
      Code *set_send = if1_send(if1, code, 5, 1, sym_operator, obj, sym_setter,
                                if1_make_symbol(if1, fld->text.c_str()), val, res);
      attach_loc(set_send, fld, res);
      return record(val);
    }

    case CoreFormKind::While: {
      Label *cont = if1_alloc_label(if1);
      Label *brk = if1_alloc_label(if1);
      Code *cond_code = nullptr;
      Sym *cond_sym =
          lower_expr(node->children[static_cast<size_t>(node->core.test)], &cond_code, env, lctx);
      cond_sym = name_literal_condition(
          cond_sym, &cond_code, node->children[static_cast<size_t>(node->core.test)], "while",
          lctx);
      if (!cond_sym) return nullptr;
      Code *body_code = nullptr;
      Sym *body_sym =
          lower_expr(node->children[static_cast<size_t>(node->core.body)], &body_code, env, lctx);
      if (!body_sym) return nullptr;
      Code *lp = if1_loop(if1, code, cont, brk, cond_sym, nullptr, cond_code, nullptr, body_code);
      attach_loc(lp, node);
      return record(if1_const(if1, sym_int64, "0"));
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
      if (lctx.boxed_syms.count(*bound)) {
        return record(unbox_read(*bound, node, code, lctx));
      }
      return record(*bound);
    }
    std::fprintf(stderr, "langwb: %s%d:%d: unbound identifier '%s'", syntax::file_prefix(node).c_str(), node->line, node->col,
                 tok.c_str());
    print_origin_suffix(node);
    return nullptr;
  }

  // Unwrap untagged single-child non-terminal (e.g. `program -> expr`).
  if (node->children.size() == 1 && !node->children[0]->is_terminal) {
    return record(lower_expr(node->children[0], code, env, lctx));
  }

  std::fprintf(stderr, "langwb: %s%d:%d: unsupported SyntaxObj '%s' with %zu children", syntax::file_prefix(node).c_str(), node->line,
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

namespace {
// Kind of the root result, recorded by `wb_emit_val`'s emitter from the same
// IFA type `emit_llvm_wb_value` dispatches on (codegen runs in this process,
// before the JIT fork), so callers need not guess the kind from printed text.
EvalValue::Kind g_wb_root_kind = EvalValue::Kind::Other;

EvalValue::Kind classify_wb_type(Sym *ty) {
  using K = EvalValue::Kind;
  if (!ty) return K::Other;
  if (ty->type_kind == Type_SUM) {
    if (ty->has.n == 0) return K::Other;
    for (int i = 0; i < ty->has.n; ++i) {
      Sym *m = ty->has[i];
      if (!m || m->type_kind != Type_RECORD || !m->name || m->has.n == 0) return K::Other;
    }
    return K::Record;  // union of ADT variants / records, printed per variant
  }
  if (ty->is_fun || ty->type_kind == Type_FUN) return K::Function;
  if (ty == sym_string) return K::String;
  if (ty->name && std::strcmp(ty->name, "wb_list") == 0) return K::List;
  if (ty == sym_bool) return K::Bool;
  if (ty == sym_int64 || ty == sym_int || ty->num_kind == IF1_NUM_KIND_INT) return K::Int64;
  if (ty->type_kind == Type_RECORD) return K::Record;
  return K::Other;
}
}  // namespace

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
                  llvm::Type *void_ty = llvm::Type::getVoidTy(*TheContext);
                  llvm::FunctionType *mft = llvm::FunctionType::get(void_ty, {}, false);
                  llvm::FunctionCallee mfn =
                      TheModule->getOrInsertFunction("_CG_wb_emit_dirty_marker", mft);
                  Builder->CreateCall(mft, mfn.getCallee(), {});
                  Var *arg = pn->rvals[2];
                  llvm::Value *v = llvm_emit_get_var(arg);
                  g_wb_root_kind = classify_wb_type(arg->type);
                  emit_llvm_wb_value(arg->type, v, /*quote_strings=*/true, 0);
                },
            .is_functional = false,
            .is_visible = true,
            .reads_fields = true,
        });
  }
  if (!reg->has("wb_check_adt")) {
    reg->register_prim(
        "wb_check_adt",
        PrimitiveHandler{
            .transfer =
                [](PNode *pn, EntrySet *es) {
                  prim_emit_result(pn, es, make_abstract_type(sym_void));
                },
            .check =
                [](PNode *pn, EntrySet *es) {
                  if (pn->rvals.n < 4 || !pn->rvals[2] || !pn->rvals[3] || !pn->rvals[3]->sym) {
                    return;
                  }
                  const char *tname = pn->rvals[3]->sym->constant;
                  if (!tname) return;
                  AVar *av = make_AVar(pn->rvals[2], es);
                  if (!av || !av->out || !av->out->n) return;
                  std::string prefix = std::string(tname) + "::";
                  for (CreationSet *cs : *av->out) {
                    if (!cs || !cs->sym || !cs->sym->name) continue;
                    std::string_view sv(cs->sym->name);
                    if (sv.starts_with(prefix)) return;
                  }
                  if (pn->lvals.n > 0) {
                    type_violation(ATypeViolation_kind::PRIMITIVE_ARGUMENT, av, av->out,
                                   make_AVar(pn->lvals[0], es));
                  }
                },
            .emit_llvm = [](PNode * /*pn*/, Fun * /*f*/) {},
            .is_functional = true,
            .is_visible = false,
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

namespace {
bool is_fn_record_field(Sym *rec_ty, int idx) {
  Sym *fsym = (idx >= 0 && idx < rec_ty->has.n) ? rec_ty->has[idx] : nullptr;
  Sym *decl_fty = fsym ? fsym->type : nullptr;
  if ((fsym && (fsym->is_fun || fsym->fun)) ||
      (decl_fty && (decl_fty->is_fun || decl_fty->fun || decl_fty->type_kind == Type_FUN))) {
    return true;
  }
  for (CreationSet *cs : rec_ty->creators) {
    if (!cs || idx < 0 || idx >= cs->vars.n || !cs->vars[idx] || !cs->vars[idx]->out) continue;
    for (CreationSet *fcs : *cs->vars[idx]->out) {
      if (fcs && fcs->sym && (fcs->sym->is_fun || fcs->sym->fun || fcs->sym->type_kind == Type_FUN)) {
        return true;
      }
    }
  }
  return false;
}

void emit_raw(const std::string &str) {
  llvm::Type *void_ty = llvm::Type::getVoidTy(*TheContext);
  llvm::Type *ptr_ty = llvm::PointerType::getUnqual(*TheContext);
  llvm::Value *s = Builder->CreateGlobalString(str);
  llvm::FunctionType *ft = llvm::FunctionType::get(void_ty, {ptr_ty}, false);
  llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_emit_raw", ft);
  Builder->CreateCall(ft, fn.getCallee(), {s});
}

// Printer functions already emitted into the current module, by record type.
llvm::Module *g_printer_module = nullptr;
std::unordered_map<Sym *, llvm::Function *> g_record_printers;
}  // namespace

// `void _wb_print_<id>(ptr)` for record type `ty`, emitted once per module. It
// asks the runtime whether `obj` is already being printed (`<cycle>`) or the
// nesting is too deep (`...`), then prints `Variant(f1, ...)` for ADT variants
// or `{ a: ..., b: ... }` for plain records.
llvm::Function *get_record_printer(Sym *ty) {
  if (g_printer_module != TheModule.get()) {
    g_record_printers.clear();
    g_printer_module = TheModule.get();
  }
  if (auto it = g_record_printers.find(ty); it != g_record_printers.end()) return it->second;

  llvm::Type *void_ty = llvm::Type::getVoidTy(*TheContext);
  llvm::Type *ptr_ty = llvm::PointerType::getUnqual(*TheContext);
  llvm::Type *i32_ty = llvm::Type::getInt32Ty(*TheContext);
  llvm::FunctionType *pft = llvm::FunctionType::get(void_ty, {ptr_ty}, false);
  llvm::Function *pf = llvm::Function::Create(pft, llvm::Function::InternalLinkage,
                                              "_wb_print_" + std::to_string(ty->id), TheModule.get());
  g_record_printers[ty] = pf;  // before the body: recursive types call themselves

  llvm::BasicBlock *saved_bb = Builder->GetInsertBlock();
  llvm::BasicBlock::iterator saved_pt = Builder->GetInsertPoint();
  llvm::Value *obj = pf->getArg(0);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(*TheContext, "entry", pf);
  llvm::BasicBlock *body = llvm::BasicBlock::Create(*TheContext, "body", pf);
  llvm::BasicBlock *cyc = llvm::BasicBlock::Create(*TheContext, "cycle", pf);
  llvm::BasicBlock *deep = llvm::BasicBlock::Create(*TheContext, "deep", pf);
  Builder->SetInsertPoint(entry);
  llvm::FunctionType *eft = llvm::FunctionType::get(i32_ty, {ptr_ty}, false);
  llvm::FunctionCallee enter = TheModule->getOrInsertFunction("_CG_wb_print_enter", eft);
  llvm::Value *st = Builder->CreateCall(eft, enter.getCallee(), {obj}, "wb_pst");
  llvm::SwitchInst *sw = Builder->CreateSwitch(st, body, 2);
  sw->addCase(llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i32_ty), 1), cyc);
  sw->addCase(llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i32_ty), 2), deep);
  Builder->SetInsertPoint(cyc);
  emit_raw("<cycle>");
  Builder->CreateRetVoid();
  Builder->SetInsertPoint(deep);
  emit_raw("...");
  Builder->CreateRetVoid();

  Builder->SetInsertPoint(body);
  auto emit_field = [&](int idx) {
    if (is_fn_record_field(ty, idx)) {
      emit_raw("<fn>");
      return;
    }
    Sym *fty = nullptr;
    llvm::Value *fval = llvm_emit_record_field(ty, obj, idx, &fty);
    if (fval) {
      emit_llvm_wb_value(fty, fval, /*quote_strings=*/true, 0);
    } else {
      emit_raw("<uninit>");
    }
  };
  std::string_view tname = ty->name ? std::string_view(ty->name) : std::string_view{};
  // Variant classes are `Type::Variant`, or `module::Type::Variant` when two
  // modules define the same type name; print just the constructor name.
  if (size_t sep = tname.rfind("::"); sep != std::string_view::npos) {
    emit_raw(std::string(tname.substr(sep + 2)));
    std::vector<int> payload;
    for (int i = 0; i < ty->has.n; ++i) {
      if (ty->has[i] && ty->has[i]->name && std::strcmp(ty->has[i]->name, "_tag") == 0) continue;
      payload.push_back(i);
    }
    if (!payload.empty()) {
      emit_raw("(");
      for (size_t k = 0; k < payload.size(); ++k) {
        if (k > 0) emit_raw(", ");
        emit_field(payload[k]);
      }
      emit_raw(")");
    }
  } else {
    emit_raw("{ ");
    for (int i = 0; i < ty->has.n; ++i) {
      if (i > 0) emit_raw(", ");
      const char *fname = (ty->has[i] && ty->has[i]->name) ? ty->has[i]->name : "_";
      emit_raw(std::string(fname) + ": ");
      emit_field(i);
    }
    emit_raw(" }");
  }
  llvm::FunctionType *xft = llvm::FunctionType::get(void_ty, {}, false);
  llvm::FunctionCallee exitf = TheModule->getOrInsertFunction("_CG_wb_print_exit", xft);
  Builder->CreateCall(xft, exitf.getCallee(), {});
  Builder->CreateRetVoid();

  if (saved_bb) Builder->SetInsertPoint(saved_bb, saved_pt);
  return pf;
}

void emit_llvm_wb_value(Sym *ty, llvm::Value *v, bool quote_strings, int depth) {
  llvm::Type *void_ty = llvm::Type::getVoidTy(*TheContext);
  llvm::Type *ptr_ty = llvm::PointerType::getUnqual(*TheContext);
  llvm::Type *i64_ty = llvm::Type::getInt64Ty(*TheContext);
  llvm::Type *i8_ty = llvm::Type::getInt8Ty(*TheContext);

  auto emit_raw_str = [&](const std::string &str) {
    llvm::Value *s = Builder->CreateGlobalString(str);
    llvm::FunctionType *ft = llvm::FunctionType::get(void_ty, {ptr_ty}, false);
    llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_emit_raw", ft);
    Builder->CreateCall(ft, fn.getCallee(), {s});
  };

  if (!v) {
    emit_raw_str("<val>");
    return;
  }

  if (ty && ty->type_kind == Type_SUM) {
    // Dispatch tagged ADT/record unions at runtime by classtag so values like
    // `if c then Some(1) else None` print as `Some(1)` or `None` instead of `<union>`.
    if (v->getType()->isPointerTy() && ty->has.n > 0) {
      std::vector<Sym *> rec_members;
      bool all_tagged_recs = true;
      for (int i = 0; i < ty->has.n; ++i) {
        Sym *m = ty->has[i];
        if (!m || m->type_kind != Type_RECORD || !m->name || m->has.n == 0) {
          all_tagged_recs = false;
          break;
        }
        rec_members.push_back(m);
      }
      if (all_tagged_recs && !rec_members.empty()) {
        llvm::Function *cur_fn = Builder->GetInsertBlock()->getParent();
        llvm::BasicBlock *done_bb = llvm::BasicBlock::Create(*TheContext, "wb_sum_done", cur_fn);
        for (size_t i = 0; i < rec_members.size(); ++i) {
          Sym *m = rec_members[i];
          llvm::Value *eq = llvm_emit_classtag_eq(m, v);
          if (!eq) continue;
          llvm::BasicBlock *arm_bb = llvm::BasicBlock::Create(*TheContext, "wb_sum_arm", cur_fn);
          llvm::BasicBlock *next_bb = llvm::BasicBlock::Create(*TheContext, "wb_sum_next", cur_fn);
          Builder->CreateCondBr(eq, arm_bb, next_bb);
          Builder->SetInsertPoint(arm_bb);
          emit_llvm_wb_value(m, v, quote_strings, depth);
          Builder->CreateBr(done_bb);
          Builder->SetInsertPoint(next_bb);
        }
        emit_raw_str("<union>");
        Builder->CreateBr(done_bb);
        Builder->SetInsertPoint(done_bb);
        return;
      }
    }
    emit_raw_str("<union>");
    return;
  }
  if (ty && (ty->is_fun || ty->type_kind == Type_FUN)) {
    emit_raw_str("<fn>");
    return;
  }
  if (ty == sym_string && v->getType()->isPointerTy()) {
    llvm::FunctionType *ft = llvm::FunctionType::get(void_ty, {ptr_ty}, false);
    const char *cname = quote_strings ? "_CG_wb_emit_str" : "_CG_wb_emit_str_unquoted";
    llvm::FunctionCallee fn = TheModule->getOrInsertFunction(cname, ft);
    Builder->CreateCall(ft, fn.getCallee(), {v});
    return;
  }
  if (ty && ty->name && std::strcmp(ty->name, "wb_list") == 0 && v->getType()->isIntegerTy(64)) {
    llvm::FunctionType *ft = llvm::FunctionType::get(void_ty, {i64_ty}, false);
    llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_emit_list", ft);
    Builder->CreateCall(ft, fn.getCallee(), {v});
    return;
  }
  if (ty == sym_bool && v->getType()->isIntegerTy()) {
    llvm::Value *b8 = v->getType()->isIntegerTy(8)
                          ? v
                          : (v->getType()->isIntegerTy(1)
                                 ? Builder->CreateZExt(v, i8_ty)
                                 : Builder->CreateZExt(
                                       Builder->CreateICmpNE(v, llvm::ConstantInt::get(v->getType(), 0)),
                                       i8_ty));
    llvm::FunctionType *ft = llvm::FunctionType::get(void_ty, {i8_ty}, false);
    llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_emit_bool", ft);
    Builder->CreateCall(ft, fn.getCallee(), {b8});
    return;
  }
  if (ty && (ty == sym_int64 || ty == sym_int || ty->num_kind == IF1_NUM_KIND_INT) &&
      v->getType()->isIntegerTy()) {
    llvm::Value *v64 = v->getType()->isIntegerTy(64) ? v : Builder->CreateSExt(v, i64_ty);
    llvm::FunctionType *ft = llvm::FunctionType::get(void_ty, {i64_ty}, false);
    llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_emit_i64", ft);
    Builder->CreateCall(ft, fn.getCallee(), {v64});
    return;
  }
  if (ty && ty->type_kind == Type_RECORD && v->getType()->isPointerTy()) {
    // One printer function per record type, calling itself (or other types'
    // printers) for nested fields: recursive values print to any depth, and the
    // runtime stack in `_CG_wb_print_enter` turns a cycle into `<cycle>`.
    if (llvm::Function *pf = get_record_printer(ty)) {
      Builder->CreateCall(pf, {v});
    } else {
      emit_raw_str("<val>");
    }
    return;
  }
  if (!ty && v->getType()->isIntegerTy(1)) {
    llvm::Value *b8 = Builder->CreateZExt(v, i8_ty);
    llvm::FunctionType *ft = llvm::FunctionType::get(void_ty, {i8_ty}, false);
    llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_emit_bool", ft);
    Builder->CreateCall(ft, fn.getCallee(), {b8});
    return;
  }
  if (!ty && v->getType()->isIntegerTy(64)) {
    llvm::FunctionType *ft = llvm::FunctionType::get(void_ty, {i64_ty}, false);
    llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_emit_i64", ft);
    Builder->CreateCall(ft, fn.getCallee(), {v});
    return;
  }
  emit_raw_str("<val>");
}

// IFA clones a Sym (a type it splits, a function it specializes) through
// this hook, so the copy must carry the Sym's contents, not just its name.
// A bare `new Sym()` lost `has`: a record class IFA split into two creation
// sets got a clone with unnamed members, and setting a field through it
// (`c.t.h = 5` where `c.t` is a split `Cons`) found no field and emitted
// nothing. Same shape as pyc's PycSymbol::copy.
IFASymbol *WbSymbol::copy() {
  auto *s = new WbSymbol();
  s->filename = filename;
  s->line_no = line_no;
  s->col_no = col_no;
  if (sym) {
    s->sym = sym->copy();
  } else {
    s->sym = new Sym();
    if1_register_sym(if1, s->sym, nullptr);
  }
  s->sym->asymbol = s;
  if (s->sym->type_kind != Type_NONE) s->sym->type = s->sym;
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
  (void)wb_list_type_sym();
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
  init_adt_types(lctx, prims);
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

  // Count distinct target functions (by canonical source Sym name, so IFA-cloned
  // copies of the same source lambda count as 1 target) reachable from each call site.
  std::unordered_map<uint64_t, std::set<const void *>> call_targets;
  for (Fun *f : pdb->fa->funs) {
    if (!f) continue;
    const void *target_key =
        (f->sym && f->sym->name) ? static_cast<const void *>(f->sym->name)
                                 : static_cast<const void *>(f);
    for (EntrySet *es : f->ess) {
      if (!es) continue;
      for (AEdge *e : es->edges) {
        if (!e || !e->pnode || e->pnode->lvals.n == 0 || !e->pnode->lvals[0]) continue;
        Sym *res_sym = e->pnode->lvals[0]->sym;
        if (auto cit = call_syms.find(res_sym); cit != call_syms.end()) {
          call_targets[cit->second.call_node_id].insert(target_key);
        }
      }
    }
  }
  for (const auto &[cid, targets] : call_targets) {
    out_attrs->set_call_target_count(cid, static_cast<int>(targets.size()));
  }

  // Capture per-function contours and call-site -> contour edges across ALL
  // IFA-cloned Fun instances sharing this lambda's canonical Sym name (in the
  // exact same `pdb->fa->funs` x `f->ess` order as `extract_sym_attr`).
  for (const auto &[fn_node_id, fn_rec] : fn_syms) {
    if (!fn_rec.fn_sym) continue;
    FnContourInfo info;
    info.fn_node_id = fn_node_id;
    info.param_node_ids = fn_rec.param_node_ids;
    std::set<std::pair<uint64_t, int>> seen_calls;
    int contour_idx = 0;
    for (Fun *f : pdb->fa->funs) {
      if (!f || !f->sym) continue;
      bool same_fn = (f->sym == fn_rec.fn_sym) ||
                     (fn_rec.fn_sym->name && f->sym->name == fn_rec.fn_sym->name);
      if (!same_fn) continue;
      for (int ci = 0; ci < f->ess.n; ++ci) {
        EntrySet *es = f->ess[ci];
        if (!es) continue;
        for (AEdge *e : es->edges) {
          if (!e || !e->pnode || e->pnode->lvals.n == 0 || !e->pnode->lvals[0]) continue;
          Sym *res_sym = e->pnode->lvals[0]->sym;
          auto cit = call_syms.find(res_sym);
          if (cit == call_syms.end()) continue;
          if (seen_calls.insert({cit->second.call_node_id, contour_idx}).second) {
            info.call_sites.push_back(FnContourInfo::CallSite{
                .call_node_id = cit->second.call_node_id,
                .contour_idx = contour_idx,
                .arg_node_ids = cit->second.arg_node_ids,
            });
          }
        }
        ++contour_idx;
      }
    }
    info.contour_count = contour_idx;
    if (info.contour_count > 0) {
      out_attrs->set_fn_info(fn_node_id, std::move(info));
    }
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
  (void)wb_list_type_sym();
  new_builtin_global_variable(sym___main__, "__main__");
  reg->install_into_if1();

  Code *code = nullptr;
  Env env;
  LowerCtx lctx{.enclosing_fn = sym___main__, .node_syms = nullptr, .resolved = &resolved};
  init_adt_types(lctx, prims);
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

Sym *wb_list_type_sym() {
  static IF1 *cached_if1 = nullptr;
  static Sym *cached_sym = nullptr;
  if (if1 != cached_if1 || !cached_sym) {
    cached_if1 = if1;
    cached_sym = new_Sym("wb_list");
    cached_sym->num_kind = IF1_NUM_KIND_INT;
    cached_sym->num_index = IF1_INT_TYPE_64;
    cached_sym->type_kind = Type_PRIMITIVE;
    cached_sym->implements.add(sym_object);
    cached_sym->specializes.add(sym_object);
    cached_sym->type = cached_sym;
  }
  return cached_sym;
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
  (void)wb_list_type_sym();
  new_builtin_global_variable(sym___main__, "__main__");
  reg->install_into_if1();

  Code *code = nullptr;
  Env env;
  LowerCtx lctx{.enclosing_fn = sym___main__, .node_syms = nullptr, .resolved = &resolved};
  init_adt_types(lctx, prims);
  Sym *val = lower_expr(root, &code, env, lctx);
  if (!val) {
    PrimRegistry::set_active(nullptr);
    ifa_reset();
    return false;
  }

  Sym *emit_val_sym = if1_make_symbol(if1, "wb_emit_val");
  (void)record_fields;
  {
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

  g_wb_root_kind = EvalValue::Kind::Other;
  FILE *null_fp = std::fopen("/dev/null", "w");
  llvm_codegen_print_ir(null_fp ? null_fp : stderr, pdb->fa, if1->top->fun, "<langwb>");
  if (null_fp) std::fclose(null_fp);

  EvalValue::Kind root_kind = g_wb_root_kind;

  char arg0[] = "langwb";
  char *argv[] = {arg0, nullptr};
  int fds[2];
  if (::pipe(fds) != 0) {
    PrimRegistry::set_active(nullptr);
    ifa_reset();
    return false;
  }
  g_jit_result_fd = fds[1];
  _CG_wb_set_emit_sink(jit_emit_sink);
  int status = llvm_jit_execute(1, argv);
  _CG_wb_set_emit_sink(nullptr);
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
  out_value->stdout_dirty = false;
  if (out_str.front() == '\x01') {
    out_value->stdout_dirty = true;
    out_str.erase(out_str.begin());
  }
  if (out_str.empty()) return false;
  out_value->str = out_str;
  out_value->kind = root_kind;
  if (root_kind == EvalValue::Kind::Bool) {
    out_value->b = (out_str == "true");
  } else if (root_kind == EvalValue::Kind::Int64) {
    char *endp = nullptr;
    long long parsed = std::strtoll(out_str.c_str(), &endp, 10);
    if (endp && *endp == '\0') {
      out_value->i64 = static_cast<int64_t>(parsed);
    } else {
      out_value->kind = EvalValue::Kind::Other;
    }
  }
  return true;
}

bool compile_to_llvm_ir(const syntax::SyntaxObj *root, const std::string &out_ll_path,
                        PrimRegistry *prims, const std::vector<std::string> *record_fields) {
  if (!root || out_ll_path.empty()) return false;

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
  (void)wb_list_type_sym();
  new_builtin_global_variable(sym___main__, "__main__");
  reg->install_into_if1();

  Code *code = nullptr;
  Env env;
  LowerCtx lctx{.enclosing_fn = sym___main__, .node_syms = nullptr, .resolved = &resolved};
  init_adt_types(lctx, prims);
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

  (void)record_fields;
  {
    Sym *d = new_Sym();
    if1_send(if1, &code, 3, 1, sym_primitive, emit_val_sym, val, d);
    emit_raw("\n");
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
  if (ifa_analyze("<langwb-aot>") < 0) {
    PrimRegistry::set_active(nullptr);
    ifa_reset();
    return false;
  }
  ifa_optimize();

  FILE *ll_fp = std::fopen(out_ll_path.c_str(), "w");
  if (!ll_fp) {
    std::fprintf(stderr, "langwb: cannot open '%s' for writing\n", out_ll_path.c_str());
    PrimRegistry::set_active(nullptr);
    ifa_reset();
    return false;
  }
  llvm_codegen_print_ir(ll_fp, pdb->fa, if1->top->fun, "<langwb-aot>");
  std::fclose(ll_fp);

  PrimRegistry::set_active(nullptr);
  ifa_reset();
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
  (void)wb_list_type_sym();
  new_builtin_global_variable(sym___main__, "__main__");
  reg->install_into_if1();

  Code *code = nullptr;
  Env env;
  LowerCtx lctx{.enclosing_fn = sym___main__, .node_syms = nullptr, .resolved = &resolved};
  init_adt_types(lctx, prims);
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
