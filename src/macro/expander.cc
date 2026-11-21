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
  id->symbol = "[a-zA-Z_][a-zA-Z0-9_]*";
  id->text = std::string(name);
  id->is_terminal = true;
  id->scopes = call_site_scopes_;
  return id;
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

syntax::SyntaxObj *MacroExpander::expand(syntax::SyntaxObj *root,
                                         const parse::DynamicGrammar &grammar,
                                         syntax::ScopeAllocator &scopes, int max_depth) const {
  return expand_node(root, grammar, scopes, 0, max_depth);
}

syntax::SyntaxObj *MacroExpander::expand_node(syntax::SyntaxObj *node,
                                              const parse::DynamicGrammar &grammar,
                                              syntax::ScopeAllocator &scopes, int depth,
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
    MacroContext ctx(grammar, scopes, flipped_in->scopes);
    syntax::SyntaxObj *expanded = it->second(flipped_in, ctx);
    if (!expanded) return nullptr;
    return expand_node(expanded->flip_scope(intro), grammar, scopes, depth + 1, max_depth);
  }

  // 2. Check call-based macro: ident '(' args ')'
  if (node->children.size() == 4 && node->children[0]->is_terminal &&
      node->children[1]->text == "(" && node->children[3]->text == ")") {
    if (auto it = call_macros_.find(node->children[0]->text); it != call_macros_.end()) {
      syntax::ScopeId intro = scopes.fresh();
      syntax::SyntaxObj *flipped_in = node->flip_scope(intro);
      MacroContext ctx(grammar, scopes, flipped_in->scopes);
      syntax::SyntaxObj *expanded = it->second(flipped_in, ctx);
      if (!expanded) return nullptr;
      return expand_node(expanded->flip_scope(intro), grammar, scopes, depth + 1, max_depth);
    }
  }

  // 3. Core binding form: 'let' ident '=' expr 'in' expr
  //    Add a fresh lexical scope to the binder and body (NOT to the RHS).
  if (node->children.size() == 6 && node->children[0]->text == "let" &&
      node->children[2]->text == "=" && node->children[4]->text == "in") {
    syntax::SyntaxObj *out = node->clone();
    out->children[3] = expand_node(out->children[3], grammar, scopes, depth, max_depth);
    if (!out->children[3]) return nullptr;

    syntax::ScopeId let_scope = scopes.fresh();
    out->children[1] = out->children[1]->add_scope(let_scope);
    syntax::SyntaxObj *scoped_body = out->children[5]->add_scope(let_scope);
    out->children[5] = expand_node(scoped_body, grammar, scopes, depth, max_depth);
    if (!out->children[5]) return nullptr;
    return out;
  }

  // 4. Default: recursively expand children.
  syntax::SyntaxObj *out = node->clone();
  for (size_t i = 0; i < out->children.size(); ++i) {
    out->children[i] = expand_node(out->children[i], grammar, scopes, depth, max_depth);
    if (!out->children[i]) return nullptr;
  }
  return out;
}

}  // namespace langwb::macro
