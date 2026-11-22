#include "ifa_bridge/meta_abi.h"

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

extern "C" int64_t _CG_stx_arg(int64_t call_h, int64_t idx) {
  if (!g_meta_session || idx < 0) return 0;
  auto *call = g_meta_session->lookup(call_h);
  if (!call || call->children.size() != 4) return 0;
  auto args = langwb::macro::MacroExpander::collect_call_args(call->children[2]);
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
  auto *stx = g_meta_session->lookup(h);
  if (!stx || stx->children.size() < 4) return 0;
  return static_cast<int64_t>(langwb::macro::MacroExpander::collect_call_args(stx->children[2]).size());
}

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

extern "C" int64_t _CG_attr_num_fields(int64_t h) {
  if (!g_meta_session || !g_meta_session->macro_ctx || !g_meta_session->macro_ctx->attrs()) {
    return 0;
  }
  auto *stx = g_meta_session->lookup(h);
  const auto *a = g_meta_session->macro_ctx->attrs()->get(stx);
  return a ? static_cast<int64_t>(a->field_names.size()) : 0;
}

extern "C" int64_t _CG_grammar_add_rule(const char *prod, const char *rhs, const char *mod) {
  if (!g_meta_session || !g_meta_session->grammar_def || !prod || !rhs) return 0;
  g_meta_session->grammar_def->add_rule(prod, rhs, mod ? mod : "");
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
  reg_i64("attr_is_const_i64", true);
  reg_i64("attr_const_i64", true);
  reg_i64("attr_num_fields", true);
  reg_i64("grammar_add_rule", false);

  // Transfer-function lattice ABI primitives.
  reg_i64("fa_num_args", true);
  reg_i64("fa_arg_is_const_i64", true);
  reg_i64("fa_arg_const_i64", true);
  reg_i64("fa_emit_const_i64", false);
  reg_i64("fa_emit_i64", false);
}

}  // namespace langwb::ifa_bridge
