#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

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
  int decls_applied = 0;
};

// Reusable workbench engine packaging an extensible DParser grammar,
// PrimRegistry, Stage-0 hygienic/comptime macros, IFA analysis-attribute
// extraction, Stage-1 attribute-driven macros, and LLVM LLJIT execution.
class Workbench {
 public:
  Workbench();

  parse::GrammarDef &grammar_def() { return grammar_def_; }
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

  // Registers a foreign C-backed primitive with an optional custom IFA transfer
  // function written purely in minilang (pre-compiled once to native LLJIT code).
  bool register_guest_prim(std::string name, std::string c_sym, std::string transfer_src = {});

  // Applies a single top-level declaration (`grammar ... += ...;`,
  // `macro ... => ...;`, `form_macro ... => ...;`, `stage1_macro ... => ...;`,
  // `stage1_form_macro ... => ...;`) directly to this Workbench (used by REPL
  // and internally on per-evaluation clones).
  bool apply_decl(std::string_view decl_src);

  // Returns true if `src` starts with a top-level declaration keyword.
  static bool starts_with_decl(std::string_view src);

  // Compiles (or fetches from cache) the current GrammarDef.
  std::shared_ptr<parse::DynamicGrammar> compiled_grammar();
  size_t grammar_cache_size() const { return cache_->size(); }

  // End-to-end: apply any leading in-language grammar/macro declarations on a
  // hermetic per-evaluation session, then parse -> Stage-0 (incl. comptime LLJIT)
  // -> IFA analyze -> Stage-1 (attr-driven) -> final IFA + LLVM LLJIT.
  bool eval_i64(std::string_view src, int64_t *out_result, const EvalOptions &opts = {},
                ifa_bridge::CompileInfo *out_info = nullptr);

  // Utility: strips `# ...` line comments from source text.
  static std::string strip_comments(std::string_view src);

 private:
  // Internal cloning ctor sharing the GrammarCache for hermetic per-file sessions.
  Workbench(const Workbench &parent, bool share_cache);

  void init_default_minilang();
  void register_guest_macro_impl(macro::MacroExpander &target, bool is_form, bool is_stage1,
                                 std::string name, std::string guest_src);

  parse::GrammarDef grammar_def_;
  std::shared_ptr<parse::GrammarCache> cache_;
  ifa_bridge::PrimRegistry prims_;
  macro::MacroExpander stage0_;
  macro::MacroExpander stage1_;
  std::shared_ptr<EvalStats> stats_;
};

}  // namespace langwb
