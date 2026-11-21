#pragma once

#include <cstdint>
#include <memory>

#include "ifa.h"
#include "ifa_bridge/attrs.h"
#include "ifa_bridge/prim_registry.h"
#include "macro/expander.h"
#include "parse/parse.h"
#include "syntax/syntax.h"

struct LLVMJitModule;

namespace langwb::ifa_bridge {

class WbSymbol : public IFASymbol {
 public:
  cchar *filename = "<input>";
  int line_no = 0;
  int col_no = 0;

  WbSymbol() = default;
  cchar *pathname() override { return filename; }
  int column() override { return col_no; }
  int line() override { return line_no; }
  int source_line() override { return line_no; }
  IFASymbol *copy() override;
};

class WbAST : public IFAAST {
 public:
  cchar *filename = "<input>";
  int line_no = 0;
  int col_no = 0;
  Sym *sym_ptr = nullptr;

  WbAST() = default;
  cchar *pathname() override { return filename; }
  int column() override { return col_no; }
  int line() override { return line_no; }
  int source_line() override { return line_no; }
  Sym *symbol() override { return sym_ptr; }
  IFAAST *copy_tree(ASTCopyContext *context) override;
  IFAAST *copy_node(ASTCopyContext *context) override;
};

class WbCallbacks : public IFACallbacks {
 public:
  Sym *new_Sym(cchar *name) override;
};

// Optional analysis facts captured before ifa_reset().
struct CompileInfo {
  bool has_inferred_const_i64 = false;
  int64_t inferred_const_i64 = 0;
};

// Runs IFA whole-program flow analysis on `root`, snapshots inferred attributes
// for every lowered SyntaxObj::node_id into *out_attrs, and calls ifa_reset().
bool analyze_syntax(const langwb::syntax::SyntaxObj *root, PrimRegistry *prims,
                    AnalysisAttrs *out_attrs);

// Lowers a hygienic SyntaxObj expression tree to IF1, installs any
// frontend-registered primitives from `prims`, runs IFA analysis + optimization,
// emits LLVM IR, executes via LLJIT, writes the int64_t result to *out_result,
// and calls ifa_reset().
bool compile_and_jit_i64(const langwb::syntax::SyntaxObj *root, int64_t *out_result,
                         PrimRegistry *prims = nullptr, CompileInfo *out_info = nullptr);

// In-process (non-forking) LLJIT execution for Phase-1 guest macros and
// transfer functions that call `_CG_*` host Meta-ABI primitives directly.
bool compile_and_run_in_process_i64(const langwb::syntax::SyntaxObj *root, int64_t *out_result,
                                    PrimRegistry *prims = nullptr,
                                    CompileInfo *out_info = nullptr);

// Pre-compiles `root` through IFA + LLVM LLJIT into a persistent native handle
// (calling ifa_reset() before returning) so it can be invoked repeatedly inside
// FA::analyze without re-entering IFA.
std::shared_ptr<LLVMJitModule> compile_persistent_i64(const langwb::syntax::SyntaxObj *root,
                                                      PrimRegistry *prims = nullptr);
bool run_persistent_i64(const std::shared_ptr<LLVMJitModule> &mod, int64_t *out_result = nullptr);

// Convenience overload accepting a raw CstNode* directly.
bool compile_and_jit_i64(const langwb::parse::CstNode *root, int64_t *out_result,
                         PrimRegistry *prims = nullptr, CompileInfo *out_info = nullptr);

// Full staged analysis-driven metaprogramming pipeline:
//   Stage-0 Macros -> IFA Analyze (extract AnalysisAttrs) ->
//   Stage-1 Attribute-Driven Macros -> Final IFA + LLVM LLJIT.
bool compile_staged_i64(langwb::syntax::SyntaxObj *root, const langwb::parse::DynamicGrammar &gram,
                        langwb::syntax::ScopeAllocator &scopes,
                        const langwb::macro::MacroExpander &stage0_macros,
                        const langwb::macro::MacroExpander &stage1_macros, PrimRegistry *prims,
                        int64_t *out_result, CompileInfo *out_info = nullptr);

}  // namespace langwb::ifa_bridge
