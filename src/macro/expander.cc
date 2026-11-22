#include "macro/expander.h"

#include <cstdio>

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
  return expander_->expand(stx, grammar_, scopes_, attrs_, 64, /*run_hooks=*/false);
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

syntax::SyntaxObj *MacroExpander::apply_datum_and_app_hooks(
    syntax::SyntaxObj *node, const parse::DynamicGrammar &grammar, syntax::ScopeAllocator &scopes,
    const ifa_bridge::AnalysisAttrs *attrs) const {
  if (!node) return nullptr;

  // Datum interposition: integer literal wrapped in a 1-child expr.
  if (datum_hook_ && node->symbol == "expr" && node->children.size() == 1 &&
      node->children[0]->is_terminal) {
    const auto &tok = node->children[0]->text;
    if (!tok.empty() && tok[0] >= '0' && tok[0] <= '9') {
      syntax::ScopeId intro = scopes.fresh();
      syntax::SyntaxObj *flipped_in = node->flip_scope(intro);
      MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this);
      syntax::SyntaxObj *rewritten = datum_hook_(flipped_in, ctx);
      return rewritten ? rewritten->flip_scope(intro) : nullptr;
    }
  }

  // Record interposition: '{' fields '}'.
  if (record_hook_ && node->children.size() == 3 && node->children[0]->text == "{" &&
      node->children[2]->text == "}") {
    syntax::SyntaxObj *with_children = node->clone();
    with_children->children[1] =
        apply_datum_and_app_hooks(with_children->children[1], grammar, scopes, attrs);
    if (!with_children->children[1]) return nullptr;
    syntax::ScopeId intro = scopes.fresh();
    syntax::SyntaxObj *flipped_in = with_children->flip_scope(intro);
    MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this);
    syntax::SyntaxObj *rewritten = record_hook_(flipped_in, ctx);
    return rewritten ? rewritten->flip_scope(intro) : nullptr;
  }

  // Field interposition: expr '.' ident.
  if (field_hook_ && node->children.size() == 3 && node->children[1]->is_terminal &&
      node->children[1]->text == ".") {
    syntax::SyntaxObj *with_children = node->clone();
    with_children->children[0] =
        apply_datum_and_app_hooks(with_children->children[0], grammar, scopes, attrs);
    if (!with_children->children[0]) return nullptr;
    syntax::ScopeId intro = scopes.fresh();
    syntax::SyntaxObj *flipped_in = with_children->flip_scope(intro);
    MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this);
    syntax::SyntaxObj *rewritten = field_hook_(flipped_in, ctx);
    return rewritten ? rewritten->flip_scope(intro) : nullptr;
  }

  // App interposition: non-macro call `ident '(' args ')'`.
  if (app_hook_ && node->children.size() == 4 && node->children[0]->is_terminal &&
      node->children[1]->text == "(" && node->children[3]->text == ")" &&
      call_macros_.find(node->children[0]->text) == call_macros_.end()) {
    syntax::SyntaxObj *with_children = node->clone();
    with_children->children[2] =
        apply_datum_and_app_hooks(with_children->children[2], grammar, scopes, attrs);
    if (!with_children->children[2]) return nullptr;
    syntax::ScopeId intro = scopes.fresh();
    syntax::SyntaxObj *flipped_in = with_children->flip_scope(intro);
    MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this);
    syntax::SyntaxObj *rewritten = app_hook_(flipped_in, ctx);
    return rewritten ? rewritten->flip_scope(intro) : nullptr;
  }

  syntax::SyntaxObj *out = node->clone();
  for (size_t i = 0; i < out->children.size(); ++i) {
    out->children[i] = apply_datum_and_app_hooks(out->children[i], grammar, scopes, attrs);
    if (!out->children[i]) return nullptr;
  }
  return out;
}

syntax::SyntaxObj *MacroExpander::expand(syntax::SyntaxObj *root,
                                         const parse::DynamicGrammar &grammar,
                                         syntax::ScopeAllocator &scopes,
                                         const ifa_bridge::AnalysisAttrs *attrs, int max_depth,
                                         bool run_hooks) const {
  if (!root) return nullptr;
  syntax::SyntaxObj *cur = root;

  if (run_hooks) {
    // Apply datum / app / field / record interposition hooks once over the original surface AST
    // (before macro expansion, so macro-introduced literals/calls aren't re-hooked).
    if (datum_hook_ || app_hook_ || field_hook_ || record_hook_) {
      cur = apply_datum_and_app_hooks(cur, grammar, scopes, attrs);
      if (!cur) return nullptr;
    }

    // Apply module_begin interposition hook once at the root.
    if (module_begin_hook_) {
      syntax::ScopeId intro = scopes.fresh();
      syntax::SyntaxObj *flipped_in = cur->flip_scope(intro);
      MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this);
      syntax::SyntaxObj *wrapped = module_begin_hook_(flipped_in, ctx);
      if (!wrapped) return nullptr;
      cur = wrapped->flip_scope(intro);
    }
  }

  return expand_node(cur, grammar, scopes, attrs, 0, max_depth);
}

syntax::SyntaxObj *MacroExpander::expand_node(syntax::SyntaxObj *node,
                                              const parse::DynamicGrammar &grammar,
                                              syntax::ScopeAllocator &scopes,
                                              const ifa_bridge::AnalysisAttrs *attrs, int depth,
                                              int max_depth) const {
  if (!node) return nullptr;
  if (depth > max_depth) {
    std::fprintf(stderr, "langwb: macro expansion exceeded max depth (%d)\n", max_depth);
    return nullptr;
  }

  // 1. Check form-based macro (e.g. dynamically added grammar non-terminal).
  if (auto it = form_macros_.find(node->symbol); it != form_macros_.end()) {
    syntax::ScopeId intro = scopes.fresh();
    syntax::SyntaxObj *flipped_in = node->flip_scope(intro);
    MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this);
    syntax::SyntaxObj *expanded = it->second(flipped_in, ctx);
    if (!expanded) {
      std::fprintf(stderr, "langwb: form macro '%s' failed at %d:%d\n", node->symbol.c_str(),
                   node->line, node->col);
      return nullptr;
    }
    if (expanded != flipped_in) {
      return expand_node(expanded->flip_scope(intro), grammar, scopes, attrs, depth + 1, max_depth);
    }
    // Macro declined rewrite (returned its input unchanged): fall through to expand children.
  }

  // 2. Check call-based macro: ident '(' args ')'
  if (node->children.size() == 4 && node->children[0]->is_terminal &&
      node->children[1]->text == "(" && node->children[3]->text == ")") {
    if (auto it = call_macros_.find(node->children[0]->text); it != call_macros_.end()) {
      syntax::ScopeId intro = scopes.fresh();
      syntax::SyntaxObj *flipped_in = node->flip_scope(intro);
      MacroContext ctx(grammar, scopes, flipped_in->scopes, attrs, this);
      syntax::SyntaxObj *expanded = it->second(flipped_in, ctx);
      if (!expanded) {
        std::fprintf(stderr, "langwb: macro '%s' failed at %d:%d\n",
                     node->children[0]->text.c_str(), node->line, node->col);
        return nullptr;
      }
      if (expanded != flipped_in) {
        return expand_node(expanded->flip_scope(intro), grammar, scopes, attrs, depth + 1,
                           max_depth);
      }
      // Macro declined rewrite (returned its input unchanged): fall through to expand children.
    }
  }

  // 3a. Core binding form: 'let' ident '=' expr 'in' expr
  //     Add a fresh lexical scope to the binder and body (NOT to the RHS).
  if (node->children.size() == 6 && node->children[0]->text == "let" &&
      node->children[2]->text == "=" && node->children[4]->text == "in") {
    syntax::SyntaxObj *out = node->clone();
    out->children[3] = expand_node(out->children[3], grammar, scopes, attrs, depth, max_depth);
    if (!out->children[3]) return nullptr;

    syntax::ScopeId let_scope = scopes.fresh();
    out->children[1] = out->children[1]->add_scope(let_scope);
    syntax::SyntaxObj *scoped_body = out->children[5]->add_scope(let_scope);
    out->children[5] = expand_node(scoped_body, grammar, scopes, attrs, depth, max_depth);
    if (!out->children[5]) return nullptr;
    return out;
  }

  // 3b. Core binding form: 'fn' '(' params ')' '=>' expr
  //     Add a fresh lexical scope to all parameter binders and the body.
  if (node->children.size() == 6 && node->children[0]->text == "fn" &&
      node->children[1]->text == "(" && node->children[3]->text == ")" &&
      node->children[4]->text == "=>") {
    syntax::SyntaxObj *out = node->clone();
    syntax::ScopeId fn_scope = scopes.fresh();
    out->children[2] = out->children[2]->add_scope(fn_scope);
    syntax::SyntaxObj *scoped_body = out->children[5]->add_scope(fn_scope);
    out->children[5] = expand_node(scoped_body, grammar, scopes, attrs, depth, max_depth);
    if (!out->children[5]) return nullptr;
    return out;
  }

  // 4. Default: recursively expand children.
  syntax::SyntaxObj *out = node->clone();
  for (size_t i = 0; i < out->children.size(); ++i) {
    out->children[i] = expand_node(out->children[i], grammar, scopes, attrs, depth, max_depth);
    if (!out->children[i]) return nullptr;
  }
  return out;
}

}  // namespace langwb::macro
