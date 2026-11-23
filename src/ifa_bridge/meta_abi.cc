#include "ifa_bridge/meta_abi.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <unordered_map>

#include "ifa_bridge/attrs.h"

namespace {

langwb::ifa_bridge::MetaSession *g_meta_session = nullptr;
langwb::ifa_bridge::TransferSession *g_transfer_session = nullptr;

langwb::syntax::SyntaxObj *splice_in_place(langwb::syntax::SyntaxObj *node,
                                           const std::string &placeholder,
                                           langwb::syntax::SyntaxObj *repl) {
  if (!node) return nullptr;
  if (node->is_terminal) {
    if (node->text == placeholder) return repl->clone();
    return node;
  }
  if (node->children.size() == 1 && node->children[0]->is_terminal &&
      node->children[0]->text == placeholder) {
    return repl->clone();
  }
  for (size_t i = 0; i < node->children.size(); ++i) {
    node->children[i] = splice_in_place(node->children[i], placeholder, repl);
  }
  return node;
}

}  // namespace

// ============================================================================
// Exported Phase-1 Host ABI (`extern "C"` symbols resolved by LLVM LLJIT)
// ============================================================================

extern "C" int64_t _CG_stx_call() {
  if (!g_meta_session || !g_meta_session->call_site) return 0;
  return 1;  // handle 1 is always call_site
}

static langwb::syntax::SyntaxObj *get_app_args_node(langwb::syntax::SyntaxObj *call) {
  if (!call) return nullptr;
  if (call->core.kind == langwb::parse::CoreFormKind::App) {
    if (call->core.args < 0 || static_cast<size_t>(call->core.args) >= call->children.size()) {
      return nullptr;
    }
    return call->children[static_cast<size_t>(call->core.args)];
  }
  return call->children.size() == 4 ? call->children[2] : nullptr;
}

extern "C" int64_t _CG_stx_arg(int64_t call_h, int64_t idx) {
  if (!g_meta_session || idx < 0) return 0;
  auto *args_node = get_app_args_node(g_meta_session->lookup(call_h));
  if (!args_node) return 0;
  auto args = langwb::macro::MacroExpander::collect_call_args(args_node);
  if (static_cast<size_t>(idx) >= args.size()) return 0;
  return g_meta_session->intern(args[static_cast<size_t>(idx)]);
}

extern "C" int64_t _CG_stx_child(int64_t h, int64_t idx) {
  if (!g_meta_session || idx < 0) return 0;
  auto *stx = g_meta_session->lookup(h);
  if (!stx || static_cast<size_t>(idx) >= stx->children.size()) return 0;
  return g_meta_session->intern(stx->children[static_cast<size_t>(idx)]);
}

extern "C" int64_t _CG_stx_num_children(int64_t h) {
  if (!g_meta_session) return 0;
  auto *stx = g_meta_session->lookup(h);
  return stx ? static_cast<int64_t>(stx->children.size()) : 0;
}

extern "C" int64_t _CG_stx_parse(const char *start_sym, const char *tmpl) {
  if (!g_meta_session || !g_meta_session->macro_ctx || !start_sym || !tmpl) return 0;
  auto *stx = g_meta_session->macro_ctx->parse_template(start_sym, tmpl);
  return g_meta_session->intern(stx);
}

extern "C" int64_t _CG_stx_splice(int64_t tmpl_h, const char *placeholder, int64_t repl_h) {
  if (!g_meta_session || !placeholder) return 0;
  auto *tmpl = g_meta_session->lookup(tmpl_h);
  auto *repl = g_meta_session->lookup(repl_h);
  if (!tmpl || !repl) return 0;
  auto *cloned = tmpl->clone();
  auto *res = splice_in_place(cloned, placeholder, repl);
  return g_meta_session->intern(res);
}

extern "C" int64_t _CG_stx_unhygienic_id(const char *name) {
  if (!g_meta_session || !g_meta_session->macro_ctx || !name) return 0;
  auto *id = g_meta_session->macro_ctx->unhygienic_ident(name);
  return g_meta_session->intern(id);
}

extern "C" int64_t _CG_stx_int(int64_t val) {
  if (!g_meta_session || !g_meta_session->macro_ctx) return 0;
  std::string src = (val < 0) ? ("(0 - " + std::to_string(-val) + ")") : std::to_string(val);
  auto *stx = g_meta_session->macro_ctx->parse_template("expr", src);
  return g_meta_session->intern(stx);
}

extern "C" int64_t _CG_stx_set_prop(int64_t h, const char *key, int64_t val) {
  if (!g_meta_session || !key) return 0;
  auto *stx = g_meta_session->lookup(h);
  if (!stx) return 0;
  auto *cloned = stx->clone();
  cloned->props[key] = val;
  return g_meta_session->intern(cloned);
}

extern "C" int64_t _CG_stx_get_prop(int64_t h, const char *key) {
  if (!g_meta_session || !key) return 0;
  auto *stx = g_meta_session->lookup(h);
  if (!stx) return 0;
  auto it = stx->props.find(key);
  return it != stx->props.end() ? it->second : 0;
}

extern "C" int64_t _CG_stx_has_prop(int64_t h, const char *key) {
  if (!g_meta_session || !key) return 0;
  auto *stx = g_meta_session->lookup(h);
  if (!stx) return 0;
  return stx->props.find(key) != stx->props.end() ? 1 : 0;
}

extern "C" int64_t _CG_stx_local_expand(int64_t h) {
  if (!g_meta_session || !g_meta_session->macro_ctx) return 0;
  auto *stx = g_meta_session->lookup(h);
  if (!stx) return 0;
  auto *expanded = g_meta_session->macro_ctx->local_expand(stx);
  return g_meta_session->intern(expanded);
}

extern "C" int64_t _CG_stx_is_core(int64_t h, const char *kind_name) {
  if (!g_meta_session || !kind_name) return 0;
  auto *stx = g_meta_session->lookup(h);
  if (!stx) return 0;
  return std::string_view(langwb::parse::core_form_name(stx->core.kind)) == kind_name ? 1 : 0;
}

extern "C" int64_t _CG_stx_text_eq(int64_t h, const char *expected) {
  if (!g_meta_session || !expected) return 0;
  auto *stx = g_meta_session->lookup(h);
  if (!stx) return 0;
  return stx->text == expected ? 1 : 0;
}

extern "C" int64_t _CG_stx_symbol_eq(int64_t h, const char *expected) {
  if (!g_meta_session || !expected) return 0;
  auto *stx = g_meta_session->lookup(h);
  if (!stx) return 0;
  return stx->symbol == expected ? 1 : 0;
}

extern "C" int64_t _CG_stx_num_args(int64_t h) {
  if (!g_meta_session) return 0;
  auto *args_node = get_app_args_node(g_meta_session->lookup(h));
  if (!args_node) return 0;
  return static_cast<int64_t>(langwb::macro::MacroExpander::collect_call_args(args_node).size());
}

static const langwb::syntax::SyntaxObj *unwrap_id_for_abi(const langwb::syntax::SyntaxObj *n) {
  while (n && !n->is_terminal && n->children.size() == 1) n = n->children[0];
  return (n && n->is_terminal) ? n : nullptr;
}

extern "C" int64_t _CG_stx_is_ident(int64_t h) {
  if (!g_meta_session) return 0;
  auto *stx = unwrap_id_for_abi(g_meta_session->lookup(h));
  if (!stx || stx->text.empty()) return 0;
  unsigned char c = static_cast<unsigned char>(stx->text[0]);
  return (std::isalpha(c) || c == '_') ? 1 : 0;
}

extern "C" int64_t _CG_stx_has_same_scopes(int64_t a, int64_t b) {
  if (!g_meta_session) return 0;
  auto *sa = g_meta_session->lookup(a);
  auto *sb = g_meta_session->lookup(b);
  if (!sa || !sb) return 0;
  return (sa->scopes == sb->scopes) ? 1 : 0;
}

extern "C" int64_t _CG_stx_bound_id_eq(int64_t a, int64_t b) {
  if (!g_meta_session) return 0;
  auto *sa = unwrap_id_for_abi(g_meta_session->lookup(a));
  auto *sb = unwrap_id_for_abi(g_meta_session->lookup(b));
  if (!sa || !sb) return 0;
  return (sa->text == sb->text && sa->scopes == sb->scopes) ? 1 : 0;
}

extern "C" int64_t _CG_stx_free_id_eq(int64_t a, int64_t b) {
  if (!g_meta_session) return 0;
  auto *sa = unwrap_id_for_abi(g_meta_session->lookup(a));
  auto *sb = unwrap_id_for_abi(g_meta_session->lookup(b));
  if (!sa || !sb) return 0;
  const auto *env =
      g_meta_session->macro_ctx ? g_meta_session->macro_ctx->lexical_env() : nullptr;
  if (env) {
    auto ra = env->resolve(sa->text, sa->scopes);
    auto rb = env->resolve(sb->text, sb->scopes);
    if (ra.has_value() != rb.has_value()) return 0;
    if (ra.has_value()) return (*ra == *rb) ? 1 : 0;
  }
  return (sa->text == sb->text) ? 1 : 0;
}

extern "C" int64_t _CG_stx_binding_eq(int64_t a, int64_t b) { return _CG_stx_free_id_eq(a, b); }

extern "C" int64_t _CG_attr_is_const_i64(int64_t h) {
  if (!g_meta_session || !g_meta_session->macro_ctx || !g_meta_session->macro_ctx->attrs()) {
    return 0;
  }
  auto *stx = g_meta_session->lookup(h);
  return g_meta_session->macro_ctx->attrs()->is_const_i64(stx) ? 1 : 0;
}

extern "C" int64_t _CG_attr_const_i64(int64_t h) {
  if (!g_meta_session || !g_meta_session->macro_ctx || !g_meta_session->macro_ctx->attrs()) {
    return 0;
  }
  auto *stx = g_meta_session->lookup(h);
  int64_t val = 0;
  g_meta_session->macro_ctx->attrs()->is_const_i64(stx, &val);
  return val;
}

extern "C" int64_t _CG_stx_error(int64_t h, const char *msg) {
  if (!g_meta_session) return 0;
  auto *stx = g_meta_session->lookup(h);
  if (!stx) stx = g_meta_session->call_site;
  if (stx) {
    std::fprintf(stderr, "%s:%d:%d: error: %s", langwb::syntax::file_or_input(stx), stx->line, stx->col,
                 msg ? msg : "static check failed");
    if (!stx->origin_macro.empty()) {
      std::fprintf(stderr, " (expanded from '%s' at %d:%d)", stx->origin_macro.c_str(),
                   stx->origin_line, stx->origin_col);
    }
    std::fprintf(stderr, "\n");
  } else {
    std::fprintf(stderr, "langwb: error: %s\n", msg ? msg : "static check failed");
  }
  g_meta_session->had_error = true;
  return 0;
}

extern "C" int64_t _CG_attr_is_type(int64_t h, const char *tname) {
  if (!g_meta_session || !g_meta_session->macro_ctx || !g_meta_session->macro_ctx->attrs() ||
      !tname) {
    return 0;
  }
  auto *stx = g_meta_session->lookup(h);
  const auto *a = g_meta_session->macro_ctx->attrs()->get(stx);
  return (a && a->is_monomorphic && a->type_names.size() == 1 && a->type_names[0] == tname) ? 1 : 0;
}

extern "C" int64_t _CG_attr_has_field(int64_t h, const char *fname) {
  if (!g_meta_session || !g_meta_session->macro_ctx || !g_meta_session->macro_ctx->attrs() ||
      !fname) {
    return 0;
  }
  auto *stx = g_meta_session->lookup(h);
  const auto *a = g_meta_session->macro_ctx->attrs()->get(stx);
  if (!a) return 0;
  for (const auto &f : a->field_names) {
    if (f == fname) return 1;
  }
  return 0;
}

extern "C" int64_t _CG_attr_is_monomorphic(int64_t h) {
  if (!g_meta_session || !g_meta_session->macro_ctx || !g_meta_session->macro_ctx->attrs()) {
    return 0;
  }
  auto *stx = g_meta_session->lookup(h);
  const auto *a = g_meta_session->macro_ctx->attrs()->get(stx);
  return (a && a->is_monomorphic) ? 1 : 0;
}

extern "C" int64_t _CG_attr_num_fields(int64_t h) {
  if (!g_meta_session || !g_meta_session->macro_ctx || !g_meta_session->macro_ctx->attrs()) {
    return 0;
  }
  auto *stx = g_meta_session->lookup(h);
  const auto *a = g_meta_session->macro_ctx->attrs()->get(stx);
  return a ? static_cast<int64_t>(a->field_names.size()) : 0;
}

extern "C" int64_t _CG_attr_field_name(int64_t h, int64_t idx) {
  if (!g_meta_session || !g_meta_session->macro_ctx || !g_meta_session->macro_ctx->attrs() ||
      idx < 0) {
    return 0;
  }
  auto *stx = g_meta_session->lookup(h);
  const auto *a = g_meta_session->macro_ctx->attrs()->get(stx);
  if (!a || static_cast<size_t>(idx) >= a->field_names.size()) return 0;
  auto *id = new langwb::syntax::SyntaxObj();
  id->node_id = langwb::syntax::SyntaxObj::next_node_id();
  id->symbol = "[a-zA-Z_][a-zA-Z0-9_]*";
  id->text = a->field_names[static_cast<size_t>(idx)];
  id->is_terminal = true;
  return g_meta_session->intern(id);
}

extern "C" int64_t _CG_attr_contour_count(int64_t h) {
  if (!g_meta_session || !g_meta_session->macro_ctx || !g_meta_session->macro_ctx->attrs()) {
    return 0;
  }
  auto *stx = g_meta_session->lookup(h);
  const auto *a = g_meta_session->macro_ctx->attrs()->get_raw(stx ? stx->node_id : 0);
  return a ? static_cast<int64_t>(a->contours.size()) : 0;
}

extern "C" int64_t _CG_attr_contour_is_const_i64(int64_t h, int64_t c) {
  if (!g_meta_session || !g_meta_session->macro_ctx || !g_meta_session->macro_ctx->attrs() || c < 0) {
    return 0;
  }
  auto *stx = g_meta_session->lookup(h);
  const auto *a = g_meta_session->macro_ctx->attrs()->get_raw(stx ? stx->node_id : 0);
  if (!a || static_cast<size_t>(c) >= a->contours.size()) return 0;
  return a->contours[static_cast<size_t>(c)].has_const_i64 ? 1 : 0;
}

extern "C" int64_t _CG_attr_contour_const_i64(int64_t h, int64_t c) {
  if (!g_meta_session || !g_meta_session->macro_ctx || !g_meta_session->macro_ctx->attrs() || c < 0) {
    return 0;
  }
  auto *stx = g_meta_session->lookup(h);
  const auto *a = g_meta_session->macro_ctx->attrs()->get_raw(stx ? stx->node_id : 0);
  if (!a || static_cast<size_t>(c) >= a->contours.size()) return 0;
  return a->contours[static_cast<size_t>(c)].const_i64;
}

extern "C" int64_t _CG_attr_contour_num_fields(int64_t h, int64_t c) {
  if (!g_meta_session || !g_meta_session->macro_ctx || !g_meta_session->macro_ctx->attrs() || c < 0) {
    return 0;
  }
  auto *stx = g_meta_session->lookup(h);
  const auto *a = g_meta_session->macro_ctx->attrs()->get_raw(stx ? stx->node_id : 0);
  if (!a || static_cast<size_t>(c) >= a->contours.size()) return 0;
  return static_cast<int64_t>(a->contours[static_cast<size_t>(c)].field_names.size());
}

namespace {

// Rewrites bare non-terminal identifiers in a DParser RHS string (outside '...'
// and "..." literals) according to `rename_map`.
std::string rewrite_rhs_nterms(std::string_view rhs,
                               const std::unordered_map<std::string, std::string> &rename_map) {
  if (rename_map.empty()) return std::string(rhs);
  std::string out;
  out.reserve(rhs.size());
  size_t i = 0;
  while (i < rhs.size()) {
    char c = rhs[i];
    if (c == '\'' || c == '"') {
      char quote = c;
      out.push_back(c);
      ++i;
      while (i < rhs.size()) {
        out.push_back(rhs[i]);
        if (rhs[i] == '\\' && i + 1 < rhs.size()) {
          out.push_back(rhs[++i]);
        } else if (rhs[i] == quote) {
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (c == '[') {
      out.push_back(c);
      ++i;
      while (i < rhs.size()) {
        out.push_back(rhs[i]);
        if (rhs[i] == '\\' && i + 1 < rhs.size()) {
          out.push_back(rhs[++i]);
        } else if (rhs[i] == ']') {
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t start = i;
      while (i < rhs.size() && (std::isalnum(static_cast<unsigned char>(rhs[i])) || rhs[i] == '_')) {
        ++i;
      }
      std::string tok(rhs.substr(start, i - start));
      auto it = rename_map.find(tok);
      out += (it != rename_map.end()) ? it->second : tok;
      continue;
    }
    out.push_back(c);
    ++i;
  }
  return out;
}

}  // namespace

extern "C" int64_t _CG_grammar_add_rule(const char *prod, const char *rhs, const char *mod) {
  if (!g_meta_session || !prod || !rhs) return 0;
  langwb::parse::GrammarDef *gdef =
      (g_meta_session->macro_ctx && g_meta_session->macro_ctx->active_grammar_def())
          ? g_meta_session->macro_ctx->active_grammar_def()
          : g_meta_session->grammar_def;
  if (!gdef) return 0;

  std::string prod_name(prod);
  auto &hmap = g_meta_session->hygienic_nterms;
  if (auto it = hmap.find(prod_name); it != hmap.end()) {
    prod_name = it->second;
  } else if (!gdef->has_production(prod_name)) {
    const auto *exp = g_meta_session->macro_ctx ? g_meta_session->macro_ctx->expander() : nullptr;
    if (!exp || !exp->has_form_macro(prod_name)) {
      if (g_meta_session->macro_ctx) {
        langwb::syntax::ScopeId sid = g_meta_session->macro_ctx->scopes().fresh();
        std::string mangled = prod_name + "__s" + std::to_string(sid);
        hmap[prod_name] = mangled;
        prod_name = std::move(mangled);
      }
    }
  }

  std::string rewritten_rhs = rewrite_rhs_nterms(rhs, hmap);
  gdef->add_rule(prod_name, rewritten_rhs, mod ? mod : "");
  return 1;
}

// ============================================================================
// Exported IFA Transfer-Function Lattice ABI (`_CG_fa_*`)
// ============================================================================

extern "C" int64_t _CG_fa_num_args() {
  if (!g_transfer_session || !g_transfer_session->pn) return 0;
  int n = g_transfer_session->pn->rvals.n - 2;
  return n > 0 ? n : 0;
}

extern "C" int64_t _CG_fa_arg_is_const_i64(int64_t idx) {
  if (!g_transfer_session || !g_transfer_session->pn || !g_transfer_session->es) return 0;
  int64_t val = 0;
  AVar *av = langwb::ifa_bridge::prim_arg_avar(g_transfer_session->pn, g_transfer_session->es,
                                               static_cast<int>(idx));
  return langwb::ifa_bridge::try_get_const_i64(av, &val) ? 1 : 0;
}

extern "C" int64_t _CG_fa_arg_const_i64(int64_t idx) {
  if (!g_transfer_session || !g_transfer_session->pn || !g_transfer_session->es) return 0;
  int64_t val = 0;
  AVar *av = langwb::ifa_bridge::prim_arg_avar(g_transfer_session->pn, g_transfer_session->es,
                                               static_cast<int>(idx));
  langwb::ifa_bridge::try_get_const_i64(av, &val);
  return val;
}

extern "C" int64_t _CG_fa_emit_const_i64(int64_t val) {
  if (!g_transfer_session || !g_transfer_session->pn || !g_transfer_session->es) return 0;
  langwb::ifa_bridge::prim_emit_result(g_transfer_session->pn, g_transfer_session->es,
                                       langwb::ifa_bridge::make_const_i64_type(val));
  return 1;
}

extern "C" int64_t _CG_fa_emit_i64() {
  if (!g_transfer_session || !g_transfer_session->pn || !g_transfer_session->es) return 0;
  langwb::ifa_bridge::prim_emit_result(g_transfer_session->pn, g_transfer_session->es,
                                       make_abstract_type(sym_int64));
  return 1;
}

namespace langwb::ifa_bridge {

MetaSession::MetaSession(syntax::SyntaxObj *call, macro::MacroContext *ctx,
                         parse::GrammarDef *gdef)
    : call_site(call), macro_ctx(ctx), grammar_def(gdef) {
  handles.push_back(nullptr);  // handle 0 = null
  handles.push_back(call);     // handle 1 = call_site
}

int64_t MetaSession::intern(syntax::SyntaxObj *stx) {
  if (!stx) return 0;
  int64_t h = static_cast<int64_t>(handles.size());
  handles.push_back(stx);
  return h;
}

syntax::SyntaxObj *MetaSession::lookup(int64_t handle) const {
  if (handle <= 0 || static_cast<size_t>(handle) >= handles.size()) return nullptr;
  return handles[static_cast<size_t>(handle)];
}

ScopedMetaSession::ScopedMetaSession(MetaSession *session) : prev_(g_meta_session) {
  g_meta_session = session;
}

ScopedMetaSession::~ScopedMetaSession() { g_meta_session = prev_; }

ScopedTransferSession::ScopedTransferSession(TransferSession *session)
    : prev_(g_transfer_session) {
  g_transfer_session = session;
}

ScopedTransferSession::~ScopedTransferSession() { g_transfer_session = prev_; }

void register_meta_abi_prims(PrimRegistry &reg) {
  auto reg_i64 = [&](const char *name, bool functional) {
    reg.register_prim(name, PrimitiveHandler{
                                .transfer =
                                    [](PNode *pn, EntrySet *es) {
                                      prim_emit_result(pn, es, make_abstract_type(sym_int64));
                                    },
                                .is_functional = functional,
                                .is_visible = true,
                            });
  };

  reg_i64("stx_call", true);
  reg_i64("stx_arg", true);
  reg_i64("stx_child", true);
  reg_i64("stx_num_children", true);
  reg_i64("stx_parse", false);
  reg_i64("stx_splice", false);
  reg_i64("stx_unhygienic_id", false);
  reg_i64("stx_int", false);
  reg_i64("stx_set_prop", false);
  reg_i64("stx_get_prop", true);
  reg_i64("stx_has_prop", true);
  reg_i64("stx_local_expand", false);
  reg_i64("stx_is_core", true);
  reg_i64("stx_text_eq", true);
  reg_i64("stx_symbol_eq", true);
  reg_i64("stx_num_args", true);
  reg_i64("stx_is_ident", true);
  reg_i64("stx_has_same_scopes", true);
  reg_i64("stx_bound_id_eq", true);
  reg_i64("stx_free_id_eq", true);
  reg_i64("stx_binding_eq", true);
  reg_i64("stx_error", false);
  reg_i64("attr_is_const_i64", true);
  reg_i64("attr_const_i64", true);
  reg_i64("attr_is_type", true);
  reg_i64("attr_has_field", true);
  reg_i64("attr_is_monomorphic", true);
  reg_i64("attr_num_fields", true);
  reg_i64("attr_field_name", true);
  reg_i64("attr_contour_count", true);
  reg_i64("attr_contour_is_const_i64", true);
  reg_i64("attr_contour_const_i64", true);
  reg_i64("attr_contour_num_fields", true);
  reg_i64("grammar_add_rule", false);

  // Transfer-function lattice ABI primitives.
  reg_i64("fa_num_args", true);
  reg_i64("fa_arg_is_const_i64", true);
  reg_i64("fa_arg_const_i64", true);
  reg_i64("fa_emit_const_i64", false);
  reg_i64("fa_emit_i64", false);
}

}  // namespace langwb::ifa_bridge
