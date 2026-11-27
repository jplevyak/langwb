#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
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
  std::string source_path;
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
  std::vector<diag::Diagnostic> diagnostics;
  // The program's constructors, with where they are declared.
  std::vector<ifa_bridge::AdtVariant> variants;
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
  bool inspect_document(std::string_view src, InspectResult *out,
                        const std::string &source_path = "");

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
  bool parse_and_apply_decl_prefix(std::string_view src, size_t *out_consumed, int base_line = 1,
                                   int base_col = 0);
  bool apply_decl_cst(const parse::CstNode *decl_cst, syntax::SyntaxObj *body_stx = nullptr,
                      std::string_view raw_tmpl = {});
  syntax::SyntaxObj *wrap_for_syntax_stx(syntax::SyntaxObj *body_stx) const;
  bool register_guest_hook_stx(std::string_view hook_kind, syntax::SyntaxObj *body_stx);
  bool register_guest_prim_stx(std::string name, std::string c_sym,
                               syntax::SyntaxObj *transfer_stx);
  enum class CheckKind { Any, Core, Call };
  bool register_static_check_stx(std::string target_name, syntax::SyntaxObj *body_stx,
                                 CheckKind kind = CheckKind::Any);
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
                        int col_offset = 0,
                        syntax::ScopeAllocator *scopes_in = nullptr,
                        syntax::ScopeId file_scope = 0);
  bool prepare_staged_syntax(syntax::SyntaxObj *stx0, const parse::DynamicGrammar &gram,
                             syntax::ScopeAllocator &scopes, const EvalOptions &opts,
                             syntax::SyntaxObj **out_final);
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
  struct StaticCheckEntry {
    CheckKind kind = CheckKind::Any;
    StaticCheckFn fn;
    // Declared by a `#lang` module: a language's checks may target library
    // functions a given program never calls, so no unused-target warning.
    bool from_lang = false;
  };

  struct GrammarOp {
    bool clear = false;
    std::string prod;
    std::string rhs;
    parse::CoreRuleMeta core;
    std::string reserve;  // non-empty: `reserve '<word>'` instead of a rule
  };
  struct SyntaxExtDelta {
    std::vector<GrammarOp> gram_ops;
    macro::MacroExpander stage0_delta;
  };
  struct ImportedModule {
    syntax::ScopeId mod_scope = 0;
    bool in_progress = false;
    std::string path;
    std::unordered_set<std::string> exported_names;
    bool export_grammar = false;
    std::vector<GrammarOp> gram_ops;
    macro::MacroExpander stage0_exports;
    std::unordered_map<std::string, SyntaxExtDelta> syntax_ext_exports;
    std::unordered_set<std::string> exported_def_names;
    std::vector<ifa_bridge::AdtDef> adt_defs;
    // Constructors an importer of this module can name (FL-15): the
    // module's own exported ones and the imported ones it re-exports.
    std::unordered_set<std::string> exported_ctors;
    // The module's Workbench. Guest macros it declared capture it (`this`), and
    // copies of them run in the importer, so it must outlive the import.
    std::shared_ptr<Workbench> owner;
  };
  bool import_module_file(std::string_view spec, const std::string &importer_dir,
                          syntax::ScopeAllocator &scopes, syntax::ScopeId importer_scope,
                          std::vector<syntax::SyntaxObj *> &out_stmts,
                          std::unordered_map<std::string, ImportedModule> &imported_cache,
                          std::vector<std::string> &import_stack,
                          std::unordered_map<std::string, std::string> &imported_val_sources);

  std::vector<std::string> lang_search_paths_;
  std::vector<std::pair<std::string, syntax::SyntaxObj *>> for_syntax_defs_;
  std::unordered_map<std::string, SyntaxExtDelta> syntax_exts_;
  std::unordered_map<std::string, std::vector<StaticCheckEntry>> static_checks_;
  std::vector<GrammarOp> *recording_gram_ops_ = nullptr;
  bool in_syntax_ext_ = false;
  // Nesting depth of `load_lang` calls currently applying declarations.
  int loading_lang_depth_ = 0;
  // Source position where the declaration being applied starts (its CST's
  // positions are relative to it).
  int decl_base_line_ = 1;
  int decl_base_col_ = 0;
  // The source range of a node of the declaration being applied.
  diag::SourceRange decl_range(const parse::CstNode *n) const {
    if (!n) return {};
    int line = decl_base_line_ - 1 + n->line;
    int col = (n->line == 1) ? decl_base_col_ + n->col : n->col;
    return diag::SourceRange{cur_file_, line, col};
  }
  // Scope of the file whose declarations are being applied (the evaluated
  // program's or an imported module's `mod_scope`). Stage-0 macros declared
  // while it is set are wrapped with it, so their templates resolve to that
  // file's top-level `def`s. 0 (e.g. while loading a `.lang`) means no wrapping.
  syntax::ScopeId decl_scope_ = 0;
  // Call macros declared while `decl_scope_` is set (this file's own), so an
  // import can make the unexported ones visible to the module's exported macros.
  std::vector<std::string> declared_call_macros_;
  // Display name of the imported module whose source is being processed (null
  // for the evaluated program); prefixes declaration diagnostics.
  const char *cur_file_ = nullptr;
  std::unordered_set<std::string> module_exports_;
  bool module_export_grammar_ = false;
  // The workbench this one was (transitively) cloned from before any `#lang`
  // was loaded: an imported module with its own `#lang` header starts from it
  // instead of from the importer, whose language is already loaded. Null for
  // a root workbench (which is its own base). Only valid while that root is
  // alive, i.e. during the evaluation that created this clone.
  const Workbench *lang_base_ = nullptr;
  // Owns the pre-`#lang` snapshot taken when `load_lang` is applied directly
  // to a root workbench (`--repl`, embedders), which is then mutated in place;
  // `lang_base_` points into it. Clones share it.
  std::shared_ptr<const Workbench> own_lang_base_;
  // Per-process cache of `#lang` languages loaded onto this (pre-`#lang` base)
  // workbench, keyed by the resolved `.lang` path: the main program and every
  // `#lang` module start from a clone of the same loaded snapshot, so a
  // language (and its lazily JIT-compiled Phase-1 code) is loaded once per
  // process instead of once per module.
  mutable std::unordered_map<std::string, std::shared_ptr<Workbench>> lang_snapshots_;
  // Macros registered while a `.lang` was being loaded, unwrapped. A module
  // started from a language snapshot re-registers them wrapped with its own
  // file scope, as loading the language into the module did before snapshots
  // (macro-built code then carries the module's scope, e.g. for its private
  // constructors).
  struct LangMacroReg {
    bool is_stage1 = false;
    bool is_form = false;
    std::string name;
    macro::MacroFn fn;
  };
  std::vector<LangMacroReg> lang_macro_regs_;
  void register_scoped_macro(macro::MacroExpander &target, bool is_form, std::string name,
                             macro::MacroFn fn);
  std::shared_ptr<Workbench> lang_snapshot(std::string_view lang_name,
                                           const std::vector<std::string> &search_paths) const;
};

struct LspOptions {
  bool bare = false;  // workers use `Workbench::create_bare()`
};

// Runs the JSON-RPC 2.0 Language Server Protocol (LSP) loop over stdio. The
// server process never builds a Workbench: each document evaluation runs in
// a forked worker with a timeout (`LANGWB_LSP_TIMEOUT_MS`, default 10000).
int run_lsp_server(const LspOptions &opts);

}  // namespace langwb
