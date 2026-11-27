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
namespace llvm {
class Value;
}

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
  // Emits IFA's type violations as structured diagnostics (same text as
  // IFA's own `show_violations`, which this turns off).
  void report_analysis_errors(Vec<ATypeViolation *> &type_violations) override;
};

// Optional analysis facts captured before ifa_reset().
struct CompileInfo {
  bool has_inferred_const_i64 = false;
  int64_t inferred_const_i64 = 0;
};

// General runtime value returned by top-level evaluation.
struct EvalValue {
  enum class Kind { Int64, Bool, String, List, Record, Function, Other };
  Kind kind = Kind::Int64;
  int64_t i64 = 0;
  bool b = false;
  bool stdout_dirty = false;
  std::string str;

  std::string to_string() const {
    if (kind == Kind::Int64) return std::to_string(i64);
    if (kind == Kind::Bool) return b ? "true" : "false";
    return str;
  }
};

// Returns the nominal IFA type symbol for immutable runtime lists (backed by i64).
Sym *wb_list_type_sym();

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

// General-value top-level JIT execution supporting int64, string, record, and function results.
bool compile_and_jit_value(const langwb::syntax::SyntaxObj *root, EvalValue *out_value,
                           PrimRegistry *prims = nullptr, CompileInfo *out_info = nullptr,
                           const std::vector<std::string> *record_fields = nullptr);

// Emits optimized LLVM IR for `root` to `out_ll_path` for AOT native compilation.
bool compile_to_llvm_ir(const langwb::syntax::SyntaxObj *root, const std::string &out_ll_path,
                        PrimRegistry *prims = nullptr,
                        const std::vector<std::string> *record_fields = nullptr);

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

// Emits LLVM IR to print/format a value of IFA type `ty` through `_CG_wb_emit_*`.
// Used by both the top-level result printer (`wb_emit_val`) and `print`/`println`.
void emit_llvm_wb_value(Sym *ty, llvm::Value *v, bool quote_strings, int depth = 0);

// Full staged analysis-driven metaprogramming pipeline:
//   Stage-0 Macros -> IFA Analyze (extract AnalysisAttrs) ->
//   Stage-1 Attribute-Driven Macros -> Final IFA + LLVM LLJIT.
bool compile_staged_i64(langwb::syntax::SyntaxObj *root, const langwb::parse::DynamicGrammar &gram,
                        langwb::syntax::ScopeAllocator &scopes,
                        const langwb::macro::MacroExpander &stage0_macros,
                        const langwb::macro::MacroExpander &stage1_macros, PrimRegistry *prims,
                        int64_t *out_result, CompileInfo *out_info = nullptr);

}  // namespace langwb::ifa_bridge
