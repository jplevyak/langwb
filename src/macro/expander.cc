#include "macro/expander.h"

#include <cstdio>

#include "parse/grammar_def.h"

namespace langwb::macro {

namespace {

syntax::SyntaxObj *apply_splices(
    syntax::SyntaxObj *node,
    const std::unordered_map<std::string, syntax::SyntaxObj *> &splices) {
  if (!node) return nullptr;

  // Direct terminal match (e.g., an identifier placeholder in a binder slot).
  if (node->is_terminal) {
    auto it = splices.find(node->text);
    if (it != splices.end() && it->second) {
      return it->second->clone();
    }
    return node;
  }

  // Expression wrapper holding a single terminal placeholder.
  if (node->children.size() == 1 && node->children[0]->is_terminal) {
    auto it = splices.find(node->children[0]->text);
    if (it != splices.end() && it->second) {
      return it->second->clone();
    }
  }

  for (size_t i = 0; i < node->children.size(); ++i) {
    node->children[i] = apply_splices(node->children[i], splices);
  }
  return node;
}

void collect_args_rec(syntax::SyntaxObj *node, std::vector<syntax::SyntaxObj *> &out) {
  if (!node) return;
  if (node->symbol == "expr") {
    out.push_back(node);
    return;
  }
  for (syntax::SyntaxObj *c : node->children) {
    if (c->is_terminal && c->text == ",") continue;
    collect_args_rec(c, out);
  }
}

// Collect statement nodes from a `stmts` list subtree (skipping `;` separators).
// Stops at any complete statement/expression/define node identified via core
// metadata or registered form macros (works for any statement nonterminal name).
void collect_seq_stmts(syntax::SyntaxObj *node,
                       const std::unordered_map<std::string, MacroFn> &form_macros,
                       std::vector<syntax::SyntaxObj *> &out) {
  if (!node || node->is_terminal) return;
  if (node->core.kind != parse::CoreFormKind::None ||
      form_macros.find(node->symbol) != form_macros.end()) {
    out.push_back(node);
    return;
  }
  for (syntax::SyntaxObj *c : node->children) {
    if (c && c->is_terminal && c->text == ";") continue;
    collect_seq_stmts(c, form_macros, out);
  }
}

void collect_param_binders(syntax::SyntaxObj *node, std::vector<syntax::SyntaxObj *> &out) {
  if (!node) return;
  if (node->is_terminal) {
    if (node->text != ",") out.push_back(node);
    return;
  }
  for (syntax::SyntaxObj *c : node->children) collect_param_binders(c, out);
}

}  // namespace

syntax::SyntaxObj *MacroContext::parse_template(
    std::string_view start_symbol, std::string_view template_src,
    const std::unordered_map<std::string, syntax::SyntaxObj *> &splices) const {
  parse::CstNode *cst = grammar_.parse(template_src, start_symbol);
  if (!cst) {
    std::fprintf(stderr, "langwb: failed to parse macro template '%.*s' as '%.*s'\n",
                 static_cast<int>(template_src.size()), template_src.data(),
                 static_cast<int>(start_symbol.size()), start_symbol.data());
    return nullptr;
  }
  syntax::SyntaxObj *stx = syntax::SyntaxObj::from_cst(cst);
  if (!splices.empty()) {
    stx = apply_splices(stx, splices);
  }
  return stx;
}

syntax::SyntaxObj *MacroContext::unhygienic_ident(std::string_view name) const {
  auto *id = new syntax::SyntaxObj();
  id->node_id = syntax::SyntaxObj::next_node_id();
  id->symbol = "[a-zA-Z_][a-zA-Z0-9_]*";
  id->text = std::string(name);
  id->is_terminal = true;
  id->scopes = call_site_scopes_;
  return id;
}

syntax::SyntaxObj *MacroContext::local_expand(syntax::SyntaxObj *stx) const {
  if (!stx || !expander_) return stx;
  syntax::BindingEnv<uint64_t> empty_env;
  const auto &env = lexical_env_ ? *lexical_env_ : empty_env;
  return expander_->expand_node(stx, grammar_, scopes_, attrs_, depth_ + 1, max_depth_,
                                active_gdef_, env);
}

void MacroExpander::register_form_macro(std::string symbol_name, MacroFn fn) {
  form_macros_[std::move(symbol_name)] = std::move(fn);
}

void MacroExpander::register_call_macro(std::string callee_name, MacroFn fn) {
  call_macros_[std::move(callee_name)] = std::move(fn);
}

std::vector<syntax::SyntaxObj *> MacroExpander::collect_call_args(syntax::SyntaxObj *args_node) {
  std::vector<syntax::SyntaxObj *> out;
  collect_args_rec(args_node, out);
  return out;
}

namespace {

// Unwrap transparent single-child wrappers (`Ident`, `PassThrough`) to see if
// an expression in callee position is a bare identifier (for call macros).
const syntax::SyntaxObj *unwrap_ident(const syntax::SyntaxObj *node) {
  while (node && !node->is_terminal && node->children.size() == 1 &&
         (node->core.kind == parse::CoreFormKind::Ident ||
          node->core.kind == parse::CoreFormKind::PassThrough ||
          node->core.kind == parse::CoreFormKind::None)) {
    node = node->children[0];
  }
  return (node && node->is_terminal) ? node : nullptr;
}

}  // namespace

syntax::SyntaxObj *MacroExpander::apply_datum_and_app_hooks(
    syntax::SyntaxObj *node, const parse::DynamicGrammar &grammar, syntax::ScopeAllocator &scopes,
    const ifa_bridge::AnalysisAttrs *attrs, parse::GrammarDef *active_gdef) const {
  if (!node) return nullptr;

  using parse::CoreFormKind;

  // Datum interposition (`#%datum`).
  if (datum_hook_ && node->core.kind == CoreFormKind::Datum) {
    syntax::ScopeId intro = scopes.fresh();
    syntax::SyntaxObj *flipped_in = node->flip_scope(intro);
    MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this, 0, 64, active_gdef);
    syntax::SyntaxObj *rewritten = datum_hook_(flipped_in, ctx);
    return rewritten ? rewritten->flip_scope(intro)->with_origin("hook:datum", node->line, node->col, node->file)
                     : nullptr;
  }

  // Record interposition (`#%record`).
  if (record_hook_ && node->core.kind == CoreFormKind::Record && node->core.fields >= 0 &&
      static_cast<size_t>(node->core.fields) < node->children.size()) {
    size_t fidx = static_cast<size_t>(node->core.fields);
    syntax::SyntaxObj *with_children = node->clone();
    with_children->children[fidx] = apply_datum_and_app_hooks(
        with_children->children[fidx], grammar, scopes, attrs, active_gdef);
    if (!with_children->children[fidx]) return nullptr;
    syntax::ScopeId intro = scopes.fresh();
    syntax::SyntaxObj *flipped_in = with_children->flip_scope(intro);
    MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this, 0, 64, active_gdef);
    syntax::SyntaxObj *rewritten = record_hook_(flipped_in, ctx);
    return rewritten
               ? rewritten->flip_scope(intro)->with_origin("hook:record", node->line, node->col, node->file)
               : nullptr;
  }

  // Field interposition (`#%field`).
  if (field_hook_ && node->core.kind == CoreFormKind::Field && node->core.obj >= 0 &&
      static_cast<size_t>(node->core.obj) < node->children.size()) {
    size_t oidx = static_cast<size_t>(node->core.obj);
    syntax::SyntaxObj *with_children = node->clone();
    with_children->children[oidx] = apply_datum_and_app_hooks(
        with_children->children[oidx], grammar, scopes, attrs, active_gdef);
    if (!with_children->children[oidx]) return nullptr;
    syntax::ScopeId intro = scopes.fresh();
    syntax::SyntaxObj *flipped_in = with_children->flip_scope(intro);
    MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this, 0, 64, active_gdef);
    syntax::SyntaxObj *rewritten = field_hook_(flipped_in, ctx);
    return rewritten ? rewritten->flip_scope(intro)->with_origin("hook:field", node->line, node->col, node->file)
                     : nullptr;
  }

  // App interposition (`#%app`): non-macro application (supports 0-arg calls where core.args < 0).
  if (app_hook_ && node->core.kind == CoreFormKind::App && node->core.fn >= 0 &&
      static_cast<size_t>(node->core.fn) < node->children.size()) {
    size_t fn_idx = static_cast<size_t>(node->core.fn);
    const syntax::SyntaxObj *callee_id = unwrap_ident(node->children[fn_idx]);
    bool is_call_macro = find_call_macro(callee_id) != nullptr;
    if (!is_call_macro) {
      syntax::SyntaxObj *with_children = node->clone();
      with_children->children[fn_idx] = apply_datum_and_app_hooks(
          with_children->children[fn_idx], grammar, scopes, attrs, active_gdef);
      if (!with_children->children[fn_idx]) return nullptr;
      if (node->core.args >= 0 && static_cast<size_t>(node->core.args) < node->children.size()) {
        size_t args_idx = static_cast<size_t>(node->core.args);
        with_children->children[args_idx] = apply_datum_and_app_hooks(
            with_children->children[args_idx], grammar, scopes, attrs, active_gdef);
        if (!with_children->children[args_idx]) return nullptr;
      }
      syntax::ScopeId intro = scopes.fresh();
      syntax::SyntaxObj *flipped_in = with_children->flip_scope(intro);
      MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this, 0, 64, active_gdef);
      syntax::SyntaxObj *rewritten = app_hook_(flipped_in, ctx);
      return rewritten ? rewritten->flip_scope(intro)->with_origin("hook:app", node->line, node->col, node->file)
                       : nullptr;
    }
  }

  syntax::SyntaxObj *out = node->clone();
  for (size_t i = 0; i < out->children.size(); ++i) {
    out->children[i] =
        apply_datum_and_app_hooks(out->children[i], grammar, scopes, attrs, active_gdef);
    if (!out->children[i]) return nullptr;
  }
  return out;
}

syntax::SyntaxObj *MacroExpander::apply_module_begin(syntax::SyntaxObj *root,
                                                     const parse::DynamicGrammar &grammar,
                                                     syntax::ScopeAllocator &scopes,
                                                     const ifa_bridge::AnalysisAttrs *attrs,
                                                     parse::GrammarDef *active_gdef) const {
  if (!root || !module_begin_hook_) return root;
  syntax::ScopeId intro = scopes.fresh();
  syntax::SyntaxObj *flipped_in = root->flip_scope(intro);
  MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this, 0, 64, active_gdef);
  syntax::SyntaxObj *wrapped = module_begin_hook_(flipped_in, ctx);
  if (!wrapped) return nullptr;
  return wrapped->flip_scope(intro);
}

syntax::SyntaxObj *MacroExpander::expand(syntax::SyntaxObj *root,
                                         const parse::DynamicGrammar &grammar,
                                         syntax::ScopeAllocator &scopes,
                                         const ifa_bridge::AnalysisAttrs *attrs, int max_depth,
                                         bool run_hooks, bool run_module_begin,
                                         parse::GrammarDef *active_gdef) const {
  if (!root) return nullptr;
  syntax::SyntaxObj *cur = root;

  if (run_hooks) {
    // Apply datum / app / field / record interposition hooks once over the original surface AST
    // (before macro expansion, so macro-introduced literals/calls aren't re-hooked).
    if (datum_hook_ || app_hook_ || field_hook_ || record_hook_) {
      cur = apply_datum_and_app_hooks(cur, grammar, scopes, attrs, active_gdef);
      if (!cur) return nullptr;
    }

    // Apply module_begin interposition hook once at the root.
    if (run_module_begin && module_begin_hook_) {
      cur = apply_module_begin(cur, grammar, scopes, attrs, active_gdef);
      if (!cur) return nullptr;
    }
  }

  syntax::BindingEnv<uint64_t> lexical_env;
  return expand_node(cur, grammar, scopes, attrs, 0, max_depth, active_gdef, lexical_env);
}

syntax::SyntaxObj *MacroExpander::expand_node(
    syntax::SyntaxObj *node, const parse::DynamicGrammar &grammar, syntax::ScopeAllocator &scopes,
    const ifa_bridge::AnalysisAttrs *attrs, int depth, int max_depth,
    parse::GrammarDef *active_gdef, const syntax::BindingEnv<uint64_t> &lexical_env) const {
  if (!node) return nullptr;
  if (depth > max_depth) {
    std::fprintf(stderr, "langwb: macro expansion exceeded max depth (%d)\n", max_depth);
    return nullptr;
  }

  // 1. Check form-based macro (e.g. dynamically added grammar non-terminal).
  if (auto it = form_macros_.find(node->symbol); it != form_macros_.end()) {
    syntax::ScopeId intro = scopes.fresh();
    syntax::SyntaxObj *flipped_in = node->flip_scope(intro);
    MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this, depth, max_depth,
                     active_gdef, &lexical_env);
    syntax::SyntaxObj *expanded = it->second(flipped_in, ctx);
    if (!expanded) {
      std::fprintf(stderr, "langwb: form macro '%s' failed at %d:%d\n", node->symbol.c_str(),
                   node->line, node->col);
      return nullptr;
    }
    if (expanded != flipped_in) {
      syntax::SyntaxObj *res =
          expanded->flip_scope(intro)->with_origin(node->symbol, node->line, node->col, node->file);
      return expand_node(res, grammar, scopes, attrs, depth + 1, max_depth, active_gdef,
                         lexical_env);
    }
    // Macro declined rewrite (returned its input unchanged): fall through to expand children.
  }

  using parse::CoreFormKind;

  // 2. Check call-based macro via CoreFormKind::App.
  if (node->core.kind == CoreFormKind::App && node->core.fn >= 0 &&
      static_cast<size_t>(node->core.fn) < node->children.size()) {
    const syntax::SyntaxObj *callee =
        unwrap_ident(node->children[static_cast<size_t>(node->core.fn)]);
    if (callee) {
      if (const MacroFn *macro_fn = find_call_macro(callee)) {
        syntax::ScopeId intro = scopes.fresh();
        syntax::SyntaxObj *flipped_in = node->flip_scope(intro);
        MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this, depth, max_depth,
                         active_gdef, &lexical_env);
        syntax::SyntaxObj *expanded = (*macro_fn)(flipped_in, ctx);
        if (!expanded) {
          std::fprintf(stderr, "langwb: macro '%s' failed at %d:%d\n", callee->text.c_str(),
                       node->line, node->col);
          return nullptr;
        }
        if (expanded != flipped_in) {
          syntax::SyntaxObj *res =
              expanded->flip_scope(intro)->with_origin(callee->text, node->line, node->col, node->file);
          return expand_node(res, grammar, scopes, attrs, depth + 1, max_depth, active_gdef,
                             lexical_env);
        }
        // Macro declined rewrite (returned its input unchanged): fall through to expand children.
      }
    }
  }

  // 2.5. Lexically scoped syntax region: `using syntax <ext> in { <body> }`.
  if (node->core.kind == CoreFormKind::UsingSyntax && using_syntax_handler_ &&
      node->core.bind >= 0 && node->core.body >= 0) {
    size_t bi = static_cast<size_t>(node->core.bind);
    size_t bodyi = static_cast<size_t>(node->core.body);
    if (bi < node->children.size() && bodyi < node->children.size()) {
      const syntax::SyntaxObj *ext_id = unwrap_ident(node->children[bi]);
      std::string_view raw = node->children[bodyi]->text;
      if (raw.size() >= 2 && raw.front() == '{' && raw.back() == '}') {
        raw.remove_prefix(1);
        raw.remove_suffix(1);
      }
      if (ext_id) {
        return using_syntax_handler_(ext_id->text, raw, node->scopes, scopes, attrs, depth + 1,
                                     max_depth, lexical_env);
      }
    }
  }

  // 2.6. Tier-2 (Expansion-tier) GLR ambiguity resolution (`CoreFormKind::Amb`).
  // Expand each parse alternative with GrammarDef snapshot/rollback so losing
  // alternatives cannot leak grammar mutations; deduplicate alpha-equivalent
  // surviving expansions so duplicate derivations collapse cleanly.
  if (node->core.kind == CoreFormKind::Amb) {
    auto is_expanded_core = [](const syntax::SyntaxObj *n) {
      while (n && !n->is_terminal && n->children.size() == 1 &&
             (n->core.kind == CoreFormKind::PassThrough || n->core.kind == CoreFormKind::None)) {
        n = n->children[0];
      }
      if (!n) return false;
      return n->is_terminal || n->core.kind != CoreFormKind::None;
    };

    std::function<void(const syntax::SyntaxObj *,
                       std::unordered_map<syntax::ScopeId, uint32_t> &, std::string &)>
        sig_rec = [&](const syntax::SyntaxObj *n,
                      std::unordered_map<syntax::ScopeId, uint32_t> &sn, std::string &out) {
          if (!n) {
            out += "null;";
            return;
          }
          out += n->symbol;
          out.push_back(':');
          if (n->is_terminal) {
            out += n->text;
            out.push_back('{');
            const auto &raw = n->scopes.raw();
            for (size_t i = 0; i < raw.size(); ++i) {
              if (i > 0) out.push_back(',');
              auto [it, ins] = sn.emplace(raw[i], static_cast<uint32_t>(sn.size()));
              out += std::to_string(it->second);
            }
            out.push_back('}');
          } else {
            out.push_back('(');
            for (const syntax::SyntaxObj *c : n->children) {
              sig_rec(c, sn, out);
              out.push_back(',');
            }
            out.push_back(')');
          }
        };

    parse::GrammarDef saved_gdef;
    if (active_gdef) saved_gdef = *active_gdef;
    parse::GrammarDef winning_gdef;
    bool has_winning_gdef = false;

    std::vector<syntax::SyntaxObj *> surviving;
    std::vector<std::string> seen_sigs;
    for (syntax::SyntaxObj *alt : node->children) {
      if (active_gdef) *active_gdef = saved_gdef;
      syntax::SyntaxObj *exp =
          expand_node(alt, grammar, scopes, attrs, depth, max_depth, active_gdef, lexical_env);
      if (exp && is_expanded_core(exp)) {
        std::string sig;
        std::unordered_map<syntax::ScopeId, uint32_t> sn;
        sig_rec(exp, sn, sig);
        bool dup = false;
        for (const auto &s : seen_sigs) {
          if (s == sig) {
            dup = true;
            break;
          }
        }
        if (!dup) {
          seen_sigs.push_back(std::move(sig));
          surviving.push_back(exp);
          if (active_gdef && surviving.size() == 1) {
            winning_gdef = *active_gdef;
            has_winning_gdef = true;
          }
        }
      }
    }
    if (active_gdef) {
      *active_gdef = (surviving.size() == 1 && has_winning_gdef) ? winning_gdef : saved_gdef;
    }
    if (surviving.empty()) {
      std::fprintf(stderr,
                   "langwb: %s%d:%d: unresolved syntax ambiguity for '%s' (0 valid expansions)\n",
                   syntax::file_prefix(node).c_str(), node->line, node->col, node->text.c_str());
      return nullptr;
    }
    if (surviving.size() == 1) {
      return surviving[0];
    }
    syntax::SyntaxObj *out = node->clone();
    out->children.clear();
    for (syntax::SyntaxObj *s : surviving) out->children.push_back(s);
    return out;
  }

  // 3a. Core binding form: Let (fresh scope on binder and body, NOT on RHS).
  if (node->core.kind == CoreFormKind::Let && node->core.bind >= 0 && node->core.rhs >= 0 &&
      node->core.body >= 0) {
    size_t bi = static_cast<size_t>(node->core.bind);
    size_t ri = static_cast<size_t>(node->core.rhs);
    size_t bodyi = static_cast<size_t>(node->core.body);
    if (bi < node->children.size() && ri < node->children.size() && bodyi < node->children.size()) {
      syntax::SyntaxObj *out = node->clone();
      out->children[ri] = expand_node(out->children[ri], grammar, scopes, attrs, depth, max_depth,
                                      active_gdef, lexical_env);
      if (!out->children[ri]) return nullptr;

      syntax::ScopeId let_scope = scopes.fresh();
      out->children[bi] = out->children[bi]->add_scope(let_scope);
      syntax::BindingEnv<uint64_t> inner_env = lexical_env;
      if (const syntax::SyntaxObj *bid = unwrap_ident(out->children[bi])) {
        inner_env.bind(bid->text, bid->scopes, bid->node_id);
      }
      syntax::SyntaxObj *scoped_body = out->children[bodyi]->add_scope(let_scope);
      out->children[bodyi] = expand_node(scoped_body, grammar, scopes, attrs, depth, max_depth,
                                         active_gdef, inner_env);
      if (!out->children[bodyi]) return nullptr;
      return out;
    }
  }

  // 3b. Core binding form: LetRec (fresh scope on binder, RHS, AND body).
  if (node->core.kind == CoreFormKind::LetRec && node->core.bind >= 0 && node->core.rhs >= 0 &&
      node->core.body >= 0) {
    size_t bi = static_cast<size_t>(node->core.bind);
    size_t ri = static_cast<size_t>(node->core.rhs);
    size_t bodyi = static_cast<size_t>(node->core.body);
    if (bi < node->children.size() && ri < node->children.size() && bodyi < node->children.size()) {
      syntax::SyntaxObj *out = node->clone();
      syntax::ScopeId rec_scope = scopes.fresh();
      out->children[bi] = out->children[bi]->add_scope(rec_scope);
      syntax::BindingEnv<uint64_t> inner_env = lexical_env;
      if (const syntax::SyntaxObj *bid = unwrap_ident(out->children[bi])) {
        inner_env.bind(bid->text, bid->scopes, bid->node_id);
      }
      syntax::SyntaxObj *scoped_rhs = out->children[ri]->add_scope(rec_scope);
      out->children[ri] = expand_node(scoped_rhs, grammar, scopes, attrs, depth, max_depth,
                                      active_gdef, inner_env);
      if (!out->children[ri]) return nullptr;
      syntax::SyntaxObj *scoped_body = out->children[bodyi]->add_scope(rec_scope);
      out->children[bodyi] = expand_node(scoped_body, grammar, scopes, attrs, depth, max_depth,
                                         active_gdef, inner_env);
      if (!out->children[bodyi]) return nullptr;
      return out;
    }
  }

  // 3c. Core binding form: Lambda (fresh scope on parameters and body).
  if (node->core.kind == CoreFormKind::Lambda && node->core.bind >= 0 && node->core.body >= 0) {
    size_t pi = static_cast<size_t>(node->core.bind);
    size_t bodyi = static_cast<size_t>(node->core.body);
    if (pi < node->children.size() && bodyi < node->children.size()) {
      syntax::SyntaxObj *out = node->clone();
      syntax::ScopeId fn_scope = scopes.fresh();
      out->children[pi] = out->children[pi]->add_scope(fn_scope);
      syntax::BindingEnv<uint64_t> inner_env = lexical_env;
      std::vector<syntax::SyntaxObj *> pbinders;
      collect_param_binders(out->children[pi], pbinders);
      for (syntax::SyntaxObj *pb : pbinders) {
        inner_env.bind(pb->text, pb->scopes, pb->node_id);
      }
      syntax::SyntaxObj *scoped_body = out->children[bodyi]->add_scope(fn_scope);
      out->children[bodyi] = expand_node(scoped_body, grammar, scopes, attrs, depth, max_depth,
                                         active_gdef, inner_env);
      if (!out->children[bodyi]) return nullptr;
      return out;
    }
  }

  // 3d. Core binding form: Seq / definition context with Racket-style partial expansion.
  if (node->core.kind == CoreFormKind::Seq) {
    std::vector<syntax::SyntaxObj *> raw_stmts;
    if (node->core.inner >= 0 && static_cast<size_t>(node->core.inner) < node->children.size()) {
      collect_seq_stmts(node->children[static_cast<size_t>(node->core.inner)], form_macros_,
                        raw_stmts);
    } else {
      for (syntax::SyntaxObj *c : node->children) {
        if (c) raw_stmts.push_back(c);
      }
    }

    syntax::ScopeId block_scope = scopes.fresh();
    std::vector<syntax::SyntaxObj *> partial;
    partial.reserve(raw_stmts.size());
    syntax::BindingEnv<uint64_t> inner_env = lexical_env;
    for (syntax::SyntaxObj *s : raw_stmts) {
      syntax::SyntaxObj *scoped = s->add_scope(block_scope);
      syntax::SyntaxObj *head =
          expand_head(scoped, grammar, scopes, attrs, depth, max_depth, active_gdef, &lexical_env);
      if (!head) return nullptr;
      if (head->core.kind == CoreFormKind::Define && head->core.bind >= 0 &&
          static_cast<size_t>(head->core.bind) < head->children.size()) {
        if (const syntax::SyntaxObj *bid =
                unwrap_ident(head->children[static_cast<size_t>(head->core.bind)])) {
          inner_env.bind(bid->text, bid->scopes, bid->node_id);
        }
      }
      partial.push_back(head);
    }

    auto *out = node->clone();
    out->core = parse::CoreRuleMeta{.kind = CoreFormKind::Seq, .inner = -1};
    out->children.clear();
    out->children.reserve(partial.size());
    for (syntax::SyntaxObj *s : partial) {
      if (s->core.kind == CoreFormKind::Define && s->core.bind >= 0 && s->core.rhs >= 0 &&
          static_cast<size_t>(s->core.bind) < s->children.size() &&
          static_cast<size_t>(s->core.rhs) < s->children.size()) {
        syntax::SyntaxObj *def_out = s->clone();
        size_t ri = static_cast<size_t>(s->core.rhs);
        def_out->children[ri] = expand_node(def_out->children[ri], grammar, scopes, attrs, depth,
                                            max_depth, active_gdef, inner_env);
        if (!def_out->children[ri]) return nullptr;
        out->children.push_back(def_out);
      } else {
        syntax::SyntaxObj *exp_s =
            expand_node(s, grammar, scopes, attrs, depth, max_depth, active_gdef, inner_env);
        if (!exp_s) return nullptr;
        out->children.push_back(exp_s);
      }
    }
    return out;
  }

  // 4. Default: recursively expand children.
  syntax::SyntaxObj *out = node->clone();
  for (size_t i = 0; i < out->children.size(); ++i) {
    out->children[i] = expand_node(out->children[i], grammar, scopes, attrs, depth, max_depth,
                                   active_gdef, lexical_env);
    if (!out->children[i]) return nullptr;
  }
  return out;
}

syntax::SyntaxObj *MacroExpander::expand_head(
    syntax::SyntaxObj *node, const parse::DynamicGrammar &grammar, syntax::ScopeAllocator &scopes,
    const ifa_bridge::AnalysisAttrs *attrs, int depth, int max_depth,
    parse::GrammarDef *active_gdef, const syntax::BindingEnv<uint64_t> *lexical_env) const {
  if (!node) return nullptr;
  if (depth > max_depth) {
    std::fprintf(stderr, "langwb: macro expansion exceeded max depth (%d)\n", max_depth);
    return nullptr;
  }

  using parse::CoreFormKind;

  // Unwrap PassThrough wrappers (e.g. `stmt -> expr`) so head macros are visible.
  if (node->core.kind == CoreFormKind::PassThrough && node->children.size() == 1) {
    return expand_head(node->children[0], grammar, scopes, attrs, depth, max_depth, active_gdef,
                       lexical_env);
  }
  if (node->core.kind == CoreFormKind::None && !node->is_terminal && node->children.size() == 1 &&
      !node->children[0]->is_terminal) {
    return expand_head(node->children[0], grammar, scopes, attrs, depth, max_depth, active_gdef,
                       lexical_env);
  }

  // Stop at Define or any non-App core form.
  if (node->core.kind == CoreFormKind::Define) return node;

  // Form macro at head?
  if (auto it = form_macros_.find(node->symbol); it != form_macros_.end()) {
    syntax::ScopeId intro = scopes.fresh();
    syntax::SyntaxObj *flipped_in = node->flip_scope(intro);
    MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this, depth, max_depth,
                     active_gdef, lexical_env);
    syntax::SyntaxObj *expanded = it->second(flipped_in, ctx);
    if (!expanded) {
      std::fprintf(stderr, "langwb: form macro '%s' failed at %d:%d\n", node->symbol.c_str(),
                   node->line, node->col);
      return nullptr;
    }
    if (expanded != flipped_in) {
      syntax::SyntaxObj *res =
          expanded->flip_scope(intro)->with_origin(node->symbol, node->line, node->col, node->file);
      return expand_head(res, grammar, scopes, attrs, depth + 1, max_depth, active_gdef,
                         lexical_env);
    }
  }

  // Call macro at head?
  if (node->core.kind == CoreFormKind::App && node->core.fn >= 0 &&
      static_cast<size_t>(node->core.fn) < node->children.size()) {
    const syntax::SyntaxObj *callee =
        unwrap_ident(node->children[static_cast<size_t>(node->core.fn)]);
    if (callee) {
      if (const MacroFn *macro_fn = find_call_macro(callee)) {
        syntax::ScopeId intro = scopes.fresh();
        syntax::SyntaxObj *flipped_in = node->flip_scope(intro);
        MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this, depth, max_depth,
                         active_gdef, lexical_env);
        syntax::SyntaxObj *expanded = (*macro_fn)(flipped_in, ctx);
        if (!expanded) {
          std::fprintf(stderr, "langwb: macro '%s' failed at %d:%d\n", callee->text.c_str(),
                       node->line, node->col);
          return nullptr;
        }
        if (expanded != flipped_in) {
          syntax::SyntaxObj *res =
              expanded->flip_scope(intro)->with_origin(callee->text, node->line, node->col, node->file);
          return expand_head(res, grammar, scopes, attrs, depth + 1, max_depth, active_gdef,
                             lexical_env);
        }
      }
    }
  }

  return node;
}

}  // namespace langwb::macro
