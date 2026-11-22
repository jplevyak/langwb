#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "parse/parse.h"
#include "syntax/syntax.h"

namespace langwb::ifa_bridge {
class AnalysisAttrs;
}

namespace langwb::macro {

class MacroExpander;

class MacroContext {
 public:
  MacroContext(const parse::DynamicGrammar &grammar, syntax::ScopeAllocator &scopes,
               const syntax::ScopeSet &call_site_scopes,
               const ifa_bridge::AnalysisAttrs *attrs = nullptr,
               const MacroExpander *expander = nullptr)
      : grammar_(grammar),
        scopes_(scopes),
        call_site_scopes_(call_site_scopes),
        attrs_(attrs),
        expander_(expander) {}

  // Parses `template_src` using the active grammar starting at `start_symbol`,
  // then replaces any identifier whose text matches a key in `splices` with
  // the corresponding SyntaxObj* (preserving the spliced node's ScopeSet).
  syntax::SyntaxObj *parse_template(
      std::string_view start_symbol, std::string_view template_src,
      const std::unordered_map<std::string, syntax::SyntaxObj *> &splices = {}) const;

  // Explicit hygiene opt-out (datum->syntax): constructs an identifier node
  // carrying the macro call-site's flipped scopes so that after the expander's
  // post-expansion flip it lands in the caller's lexical scope.
  syntax::SyntaxObj *unhygienic_ident(std::string_view name) const;

  // Racket-style `local-expand`: expands a sub-expression on demand using the
  // active MacroExpander so the calling macro can inspect the expanded result
  // and its syntax properties.
  syntax::SyntaxObj *local_expand(syntax::SyntaxObj *stx) const;

  const parse::DynamicGrammar &grammar() const { return grammar_; }
  syntax::ScopeAllocator &scopes() const { return scopes_; }
  // IFA analysis attributes (non-null during Stage-1 attribute-driven expansion).
  const ifa_bridge::AnalysisAttrs *attrs() const { return attrs_; }

 private:
  const parse::DynamicGrammar &grammar_;
  syntax::ScopeAllocator &scopes_;
  syntax::ScopeSet call_site_scopes_;
  const ifa_bridge::AnalysisAttrs *attrs_ = nullptr;
  const MacroExpander *expander_ = nullptr;
};

using MacroFn = std::function<syntax::SyntaxObj *(syntax::SyntaxObj *call_site, MacroContext &ctx)>;

// Hygienic procedural macro expander over SyntaxObj trees using Flatt's
// sets-of-scopes algorithm (flip intro scope on input and output; add fresh
// lexical scope on binding forms).
class MacroExpander {
 public:
  // Matches any SyntaxObj whose grammar symbol == `symbol_name`.
  void register_form_macro(std::string symbol_name, MacroFn fn);

  // Matches call syntax `ident '(' args ')'` where ident == `callee_name`.
  void register_call_macro(std::string callee_name, MacroFn fn);

  // Racket-style core interposition hooks:
  // - module_begin: transforms the top-level root SyntaxObj once before expansion.
  // - datum: transforms each original integer literal SyntaxObj once.
  // - app: transforms each non-macro call SyntaxObj once.
  // - field: transforms each field access `expr.ident` once.
  // - record: transforms each record literal `{ ... }` once.
  void set_module_begin_hook(MacroFn fn) { module_begin_hook_ = std::move(fn); }
  void set_datum_hook(MacroFn fn) { datum_hook_ = std::move(fn); }
  void set_app_hook(MacroFn fn) { app_hook_ = std::move(fn); }
  void set_field_hook(MacroFn fn) { field_hook_ = std::move(fn); }
  void set_record_hook(MacroFn fn) { record_hook_ = std::move(fn); }

  syntax::SyntaxObj *expand(syntax::SyntaxObj *root, const parse::DynamicGrammar &grammar,
                            syntax::ScopeAllocator &scopes,
                            const ifa_bridge::AnalysisAttrs *attrs = nullptr,
                            int max_depth = 64, bool run_hooks = true) const;

  // Helper to extract call arguments from an `args` subtree.
  static std::vector<syntax::SyntaxObj *> collect_call_args(syntax::SyntaxObj *args_node);

 private:
  syntax::SyntaxObj *apply_datum_and_app_hooks(syntax::SyntaxObj *node,
                                               const parse::DynamicGrammar &grammar,
                                               syntax::ScopeAllocator &scopes,
                                               const ifa_bridge::AnalysisAttrs *attrs) const;
  syntax::SyntaxObj *expand_head(syntax::SyntaxObj *node, const parse::DynamicGrammar &grammar,
                                 syntax::ScopeAllocator &scopes,
                                 const ifa_bridge::AnalysisAttrs *attrs, int depth,
                                 int max_depth) const;
  syntax::SyntaxObj *expand_node(syntax::SyntaxObj *node, const parse::DynamicGrammar &grammar,
                                 syntax::ScopeAllocator &scopes,
                                 const ifa_bridge::AnalysisAttrs *attrs, int depth,
                                 int max_depth) const;

  std::unordered_map<std::string, MacroFn> form_macros_;
  std::unordered_map<std::string, MacroFn> call_macros_;
  MacroFn module_begin_hook_;
  MacroFn datum_hook_;
  MacroFn app_hook_;
  MacroFn field_hook_;
  MacroFn record_hook_;
};

}  // namespace langwb::macro
