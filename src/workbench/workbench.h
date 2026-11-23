#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ifa_bridge/attrs.h"
#include "ifa_bridge/bridge.h"
#include "ifa_bridge/prim_registry.h"
#include "macro/expander.h"
#include "parse/grammar_def.h"
#include "parse/parse.h"
#include "syntax/syntax.h"

namespace langwb {

struct EvalOptions {
  bool staged = true;
  bool dump_cst = false;
  bool dump_expanded = false;
  bool dump_attrs = false;
  FILE *diag_fp = stderr;
};

struct EvalStats {
  int comptime_evals = 0;
  int stage1_unrolls = 0;
  int stage1_reflections = 0;
  int contour_clones = 0;
  int decls_applied = 0;
};

struct InspectResult {
  bool ok = false;
  syntax::SyntaxObj *expanded = nullptr;
  ifa_bridge::AnalysisAttrs attrs;
  std::string diagnostics;
};

// Reusable workbench engine packaging an extensible DParser grammar,
// PrimRegistry, Stage-0 hygienic/comptime macros, IFA analysis-attribute
// extraction, Stage-1 attribute-driven macros, and LLVM LLJIT execution.
class Workbench {
 public:
  Workbench();
  // Creates a bare Workbench with zero C++ grammar rules (only runtime prims
  // and Phase-1 Meta-ABI), intended to boot completely from `#lang prelude`.
  static Workbench create_bare();

  parse::GrammarDef &grammar_def() { return grammar_def_; }
  parse::GrammarDef &decl_grammar() { return decl_grammar_; }
  ifa_bridge::PrimRegistry &prims() { return prims_; }
  macro::MacroExpander &stage0_macros() { return stage0_; }
  macro::MacroExpander &stage1_macros() { return stage1_; }
  const EvalStats &stats() const { return *stats_; }
  void reset_stats() { *stats_ = EvalStats{}; }

  // Registers a procedural macro written purely in the guest language (minilang)
  // that executes in-process via IFA + LLVM LLJIT using the Phase-1 Meta-ABI.
  void register_guest_stage0_macro(std::string name, std::string guest_src);
  void register_guest_stage1_macro(std::string name, std::string guest_src);
  void register_guest_stage0_form_macro(std::string symbol_name, std::string guest_src);
  void register_guest_stage1_form_macro(std::string symbol_name, std::string guest_src);

  // Registers a declarative Stage-1 static check (`check <target> => <expr>;`)
  // evaluated over IFA AnalysisAttrs; `<target>` matches a core form name
  // (`binop`, `app`, `field`, `record`, ...) or a called function name.
  void register_static_check(std::string target_name, std::string guest_src);

  // Registers a foreign C-backed primitive with an optional custom IFA transfer
  // function written purely in minilang (pre-compiled once to native LLJIT code).
  bool register_guest_prim(std::string name, std::string c_sym, std::string transfer_src = {});

  // Registers a Racket-style core interposition hook (`module_begin`, `datum`,
  // `app`, `field`, or `record`) written purely in minilang.
  bool register_guest_hook(std::string_view hook_kind, std::string guest_src);

  // Registers a declarative pattern-matching macro (`syntax_rule`), using
  // DParser non-terminals as optional syntax classes (`$x:expr`, `$id:ident`,
  // and trailing variadic `$xs:expr...`).
  bool register_syntax_rule(bool is_form, std::string name,
                            std::vector<std::pair<std::string, std::string>> pattern_slots,
                            std::string template_src, bool trailing_variadic = false);

  // Adds a shared Phase-1 helper binding (`for_syntax name => expr;`) visible
  // to all subsequent guest macros, hooks, and transfer functions.
  void add_for_syntax(std::string name, std::string expr_src);

  // Loads a `#lang <name>` language module file along `lang_search_paths_`
  // (supporting nested `#lang` inheritance) and applies all of its declarations.
  bool load_lang(std::string_view lang_name, int depth = 0);
  void add_lang_search_path(std::string dir) { lang_search_paths_.push_back(std::move(dir)); }

  // Applies a single top-level declaration (`grammar`, `macro`, `form_macro`,
  // `stage1_macro`, `stage1_form_macro`, `prim`, `hook`).
  bool apply_decl(std::string_view decl_src);

  // Returns true if `src` starts with a top-level declaration keyword.
  static bool starts_with_decl(std::string_view src);
  // If `src` begins with a `#lang <name>` header line, writes `<name>` to
  // *out_lang, advances `src` past the header line, and returns true.
  static bool extract_lang_header(std::string_view &src, std::string *out_lang);

  // Compiles (or fetches from cache) the current GrammarDef.
  std::shared_ptr<parse::DynamicGrammar> compiled_grammar();
  size_t grammar_cache_size() const { return cache_->size(); }

  // End-to-end: apply any leading in-language grammar/macro declarations on a
  // hermetic per-evaluation session, then parse -> Stage-0 (incl. comptime LLJIT)
  // -> IFA analyze -> Stage-1 (attr-driven) -> final IFA + LLVM LLJIT.
  bool eval_i64(std::string_view src, int64_t *out_result, const EvalOptions &opts = {},
                ifa_bridge::CompileInfo *out_info = nullptr);

  // General-value top-level evaluation supporting int64, string, record, and function results.
  bool eval_value(std::string_view src, ifa_bridge::EvalValue *out_value,
                  const EvalOptions &opts = {}, ifa_bridge::CompileInfo *out_info = nullptr);

  // AOT compiler driver: compiles `src` through the full staged pipeline and emits
  // either LLVM IR (`emit_llvm_only == true`) or a standalone native executable.
  bool build_aot(std::string_view src, const std::string &output_path,
                 bool emit_llvm_only = false, const EvalOptions &opts = {});

  // Runs parsing, Stage-0 expansion, and IFA analysis without JIT execution,
  // returning the expanded SyntaxObj tree, AnalysisAttrs (types/constants/fields/binders),
  // and any captured diagnostics (used by `langwb --lsp`).
  bool inspect_document(std::string_view src, InspectResult *out);

  // Utility: strips `# ...` line comments from source text.
  static std::string strip_comments(std::string_view src);

 private:
  struct BareTag {};
  explicit Workbench(BareTag);
  // Internal cloning ctor sharing the GrammarCache for hermetic per-file sessions.
  Workbench(const Workbench &parent, bool share_cache);

  void init_decl_grammar();
  void init_builtin_prims();
  void init_default_minilang();
  void init_using_syntax_hook();
  std::shared_ptr<parse::DynamicGrammar> compiled_decl_grammar();
  bool parse_and_apply_decl_prefix(std::string_view src, size_t *out_consumed);
  bool apply_decl_cst(const parse::CstNode *decl_cst, syntax::SyntaxObj *body_stx = nullptr);
  syntax::SyntaxObj *wrap_for_syntax_stx(syntax::SyntaxObj *body_stx,
                                         const parse::DynamicGrammar &gram) const;
  bool register_guest_hook_stx(std::string_view hook_kind, syntax::SyntaxObj *body_stx);
  bool register_guest_prim_stx(std::string name, std::string c_sym,
                               syntax::SyntaxObj *transfer_stx);
  bool register_static_check_stx(std::string target_name, syntax::SyntaxObj *body_stx);
  bool register_guest_macro_stx(macro::MacroExpander &target, bool is_form, bool is_stage1,
                                std::string name, syntax::SyntaxObj *body_stx);
  bool eval_impl(std::string_view src, int64_t *out_i64, ifa_bridge::EvalValue *out_value,
                 const EvalOptions &opts, ifa_bridge::CompileInfo *out_info,
                 const std::string *aot_ll_path = nullptr, InspectResult *out_inspect = nullptr);
  bool eval_single_expr(std::string_view expr_src, int64_t *out_i64,
                        ifa_bridge::EvalValue *out_value, const EvalOptions &opts,
                        ifa_bridge::CompileInfo *out_info,
                        const std::string *aot_ll_path = nullptr,
                        InspectResult *out_inspect = nullptr, int line_offset = 0,
                        int col_offset = 0);
  bool prepare_staged_syntax(syntax::SyntaxObj *stx0, const parse::DynamicGrammar &gram,
                             syntax::ScopeAllocator &scopes, const EvalOptions &opts,
                             syntax::SyntaxObj **out_final,
                             std::vector<std::string> *out_top_fields = nullptr);
  bool run_static_checks(syntax::SyntaxObj *node, const parse::DynamicGrammar &gram,
                         syntax::ScopeAllocator &scopes, const ifa_bridge::AnalysisAttrs &attrs);
  bool run_staged_and_jit(syntax::SyntaxObj *stx0, const parse::DynamicGrammar &gram,
                          syntax::ScopeAllocator &scopes, int64_t *out_i64,
                          ifa_bridge::EvalValue *out_value, const EvalOptions &opts,
                          ifa_bridge::CompileInfo *out_info,
                          const std::string *aot_ll_path = nullptr,
                          InspectResult *out_inspect = nullptr);
  syntax::SyntaxObj *expand_stage1_contour_aware(syntax::SyntaxObj *node,
                                                 const parse::DynamicGrammar &gram,
                                                 syntax::ScopeAllocator &scopes,
                                                 const ifa_bridge::AnalysisAttrs &attrs,
                                                 bool allow_demand_reanalyze = false,
                                                 bool *out_did_reanalyze_split = nullptr);
  void register_guest_macro_impl(macro::MacroExpander &target, bool is_form, bool is_stage1,
                                 std::string name, std::string guest_src);

  parse::GrammarDef grammar_def_;
  parse::GrammarDef decl_grammar_;
  std::shared_ptr<parse::GrammarCache> cache_;
  ifa_bridge::PrimRegistry prims_;
  macro::MacroExpander stage0_;
  macro::MacroExpander stage1_;
  std::shared_ptr<EvalStats> stats_;
  using StaticCheckFn = std::function<bool(syntax::SyntaxObj *, macro::MacroContext &)>;

  std::vector<std::string> lang_search_paths_;
  std::vector<std::pair<std::string, syntax::SyntaxObj *>> for_syntax_defs_;
  std::unordered_map<std::string, std::string> syntax_exts_;
  std::unordered_map<std::string, std::vector<StaticCheckFn>> static_checks_;
};

// Runs the JSON-RPC 2.0 Language Server Protocol (LSP) loop over stdio.
int run_lsp_server(Workbench &wb);

}  // namespace langwb
