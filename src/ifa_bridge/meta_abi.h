#pragma once

#include <cstdint>
#include <vector>

#include "ifa_bridge/prim_registry.h"
#include "macro/expander.h"
#include "parse/grammar_def.h"
#include "parse/parse.h"
#include "syntax/syntax.h"

namespace langwb::ifa_bridge {

// Active Phase-1 metaprogramming session bridging in-process LLJIT-compiled
// guest code to host SyntaxObj, MacroContext, AnalysisAttrs, and GrammarDef.
struct MetaSession {
  syntax::SyntaxObj *call_site = nullptr;
  macro::MacroContext *macro_ctx = nullptr;
  parse::GrammarDef *grammar_def = nullptr;
  // GC-allocated handle table (index 0 = null, 1..N = live SyntaxObj handles).
  std::vector<syntax::SyntaxObj *, parse::GcAlloc<syntax::SyntaxObj *>> handles;

  MetaSession(syntax::SyntaxObj *call, macro::MacroContext *ctx, parse::GrammarDef *gdef = nullptr);
  int64_t intern(syntax::SyntaxObj *stx);
  syntax::SyntaxObj *lookup(int64_t handle) const;
};

// RAII guard installing a MetaSession as the active Phase-1 session (supports nesting).
class ScopedMetaSession {
 public:
  explicit ScopedMetaSession(MetaSession *session);
  ~ScopedMetaSession();

  ScopedMetaSession(const ScopedMetaSession &) = delete;
  ScopedMetaSession &operator=(const ScopedMetaSession &) = delete;

 private:
  MetaSession *prev_ = nullptr;
};

// Registers all Phase-1 `_CG_wb_*` metaprogramming host primitives into `reg`.
void register_meta_abi_prims(PrimRegistry &reg);

}  // namespace langwb::ifa_bridge
