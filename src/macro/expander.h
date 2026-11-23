#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "parse/parse.h"
#include "syntax/syntax.h"

namespace langwb::parse {
class GrammarDef;
}

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
               const MacroExpander *expander = nullptr, int depth = 0, int max_depth = 64,
               parse::GrammarDef *active_gdef = nullptr,
               const syntax::BindingEnv<uint64_t> *lexical_env = nullptr)
      : grammar_(grammar),
        scopes_(scopes),
        call_site_scopes_(call_site_scopes),
        attrs_(attrs),
        expander_(expander),
        depth_(depth),
        max_depth_(max_depth),
        active_gdef_(active_gdef),
        lexical_env_(lexical_env) {}

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
  // active MacroExpander, sharing the caller's depth budget.
  syntax::SyntaxObj *local_expand(syntax::SyntaxObj *stx) const;

  const parse::DynamicGrammar &grammar() const { return grammar_; }
  syntax::ScopeAllocator &scopes() const { return scopes_; }
  // IFA analysis attributes (non-null during Stage-1 attribute-driven expansion).
  const ifa_bridge::AnalysisAttrs *attrs() const { return attrs_; }
  parse::GrammarDef *active_grammar_def() const { return active_gdef_; }
  const MacroExpander *expander() const { return expander_; }
  const syntax::BindingEnv<uint64_t> *lexical_env() const { return lexical_env_; }

 private:
  const parse::DynamicGrammar &grammar_;
  syntax::ScopeAllocator &scopes_;
  syntax::ScopeSet call_site_scopes_;
  const ifa_bridge::AnalysisAttrs *attrs_ = nullptr;
  const MacroExpander *expander_ = nullptr;
  int depth_ = 0;
  int max_depth_ = 64;
  parse::GrammarDef *active_gdef_ = nullptr;
  const syntax::BindingEnv<uint64_t> *lexical_env_ = nullptr;
};

using MacroFn = std::function<syntax::SyntaxObj *(syntax::SyntaxObj *call_site, MacroContext &ctx)>;
using UsingSyntaxFn = std::function<syntax::SyntaxObj *(
    std::string_view ext_name, std::string_view body_src, const syntax::ScopeSet &enclosing_scopes,
    syntax::ScopeAllocator &scopes, const ifa_bridge::AnalysisAttrs *attrs, int depth,
    int max_depth, const syntax::BindingEnv<uint64_t> &lexical_env)>;

// Hygienic procedural macro expander over SyntaxObj trees using Flatt's
// sets-of-scopes algorithm (flip intro scope on input and output; add fresh
// lexical scope on binding forms).
class MacroExpander {
 public:
  // Matches any SyntaxObj whose grammar symbol == `symbol_name`.
  void register_form_macro(std::string symbol_name, MacroFn fn);
  bool has_form_macro(const std::string &symbol_name) const {
    return form_macros_.find(symbol_name) != form_macros_.end();
  }

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
  void set_using_syntax_handler(UsingSyntaxFn fn) { using_syntax_handler_ = std::move(fn); }

  // Overlays all macros and non-empty hooks from `other` onto this expander.
  void merge_from(const MacroExpander &other) {
    for (const auto &[k, v] : other.form_macros_) form_macros_[k] = v;
    for (const auto &[k, v] : other.call_macros_) call_macros_[k] = v;
    if (other.module_begin_hook_) module_begin_hook_ = other.module_begin_hook_;
    if (other.datum_hook_) datum_hook_ = other.datum_hook_;
    if (other.app_hook_) app_hook_ = other.app_hook_;
    if (other.field_hook_) field_hook_ = other.field_hook_;
    if (other.record_hook_) record_hook_ = other.record_hook_;
  }

  // Copies a single named macro (call or form) from `other` if present.
  bool copy_macro_from(const MacroExpander &other, const std::string &name) {
    bool copied = false;
    if (auto it = other.form_macros_.find(name); it != other.form_macros_.end()) {
      form_macros_[name] = it->second;
      copied = true;
    }
    if (auto it = other.call_macros_.find(name); it != other.call_macros_.end()) {
      call_macros_[name] = it->second;
      copied = true;
    }
    return copied;
  }

  syntax::SyntaxObj *expand(syntax::SyntaxObj *root, const parse::DynamicGrammar &grammar,
                            syntax::ScopeAllocator &scopes,
                            const ifa_bridge::AnalysisAttrs *attrs = nullptr, int max_depth = 64,
                            bool run_hooks = true, bool run_module_begin = true,
                            parse::GrammarDef *active_gdef = nullptr) const;

  // Applies the module_begin interposition hook once at `root` (no-op if unset).
  syntax::SyntaxObj *apply_module_begin(syntax::SyntaxObj *root,
                                        const parse::DynamicGrammar &grammar,
                                        syntax::ScopeAllocator &scopes,
                                        const ifa_bridge::AnalysisAttrs *attrs = nullptr,
                                        parse::GrammarDef *active_gdef = nullptr) const;

  // Expands only head macros on `node` until a non-macro head is reached (does not
  // recurse into children). Used by Seq partial expansion and head-first Stage-1.
  syntax::SyntaxObj *expand_head(syntax::SyntaxObj *node, const parse::DynamicGrammar &grammar,
                                 syntax::ScopeAllocator &scopes,
                                 const ifa_bridge::AnalysisAttrs *attrs, int depth, int max_depth,
                                 parse::GrammarDef *active_gdef = nullptr,
                                 const syntax::BindingEnv<uint64_t> *lexical_env = nullptr) const;

  // Helper to extract call arguments from an `args` subtree.
  static std::vector<syntax::SyntaxObj *> collect_call_args(syntax::SyntaxObj *args_node);

  // Internal recursive entry point shared by MacroContext::local_expand.
  syntax::SyntaxObj *expand_node(syntax::SyntaxObj *node, const parse::DynamicGrammar &grammar,
                                 syntax::ScopeAllocator &scopes,
                                 const ifa_bridge::AnalysisAttrs *attrs, int depth, int max_depth,
                                 parse::GrammarDef *active_gdef,
                                 const syntax::BindingEnv<uint64_t> &lexical_env) const;

 private:
  syntax::SyntaxObj *apply_datum_and_app_hooks(syntax::SyntaxObj *node,
                                               const parse::DynamicGrammar &grammar,
                                               syntax::ScopeAllocator &scopes,
                                               const ifa_bridge::AnalysisAttrs *attrs,
                                               parse::GrammarDef *active_gdef) const;

  std::unordered_map<std::string, MacroFn> form_macros_;
  std::unordered_map<std::string, MacroFn> call_macros_;
  MacroFn module_begin_hook_;
  MacroFn datum_hook_;
  MacroFn app_hook_;
  MacroFn field_hook_;
  MacroFn record_hook_;
  UsingSyntaxFn using_syntax_handler_;
};

}  // namespace langwb::macro
