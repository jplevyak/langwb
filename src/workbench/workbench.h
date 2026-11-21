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
  const EvalStats &stats() const { return stats_; }
  void reset_stats() { stats_ = EvalStats{}; }

  // Compiles (or fetches from cache) the current GrammarDef.
  std::shared_ptr<parse::DynamicGrammar> compiled_grammar();
  size_t grammar_cache_size() const { return cache_.size(); }

  // End-to-end parse -> Stage-0 (incl. comptime LLJIT) -> IFA analyze ->
  // Stage-1 (attr-driven) -> final IFA + LLVM LLJIT.
  bool eval_i64(std::string_view src, int64_t *out_result, const EvalOptions &opts = {},
                ifa_bridge::CompileInfo *out_info = nullptr);

  // Utility: strips `# ...` line comments from source text.
  static std::string strip_comments(std::string_view src);

 private:
  void init_default_minilang();

  parse::GrammarDef grammar_def_;
  parse::GrammarCache cache_;
  ifa_bridge::PrimRegistry prims_;
  macro::MacroExpander stage0_;
  macro::MacroExpander stage1_;
  EvalStats stats_;
};

}  // namespace langwb
