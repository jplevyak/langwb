#pragma once

#include <cstdint>

#include "ifa.h"
#include "ifa_bridge/prim_registry.h"
#include "parse/parse.h"
#include "syntax/syntax.h"

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

// Lowers a hygienic SyntaxObj expression tree (integers, arithmetic/comparisons,
// parens, if..then..else, let..in, primitive calls) to IF1, installs any
// frontend-registered primitives from `prims`, runs IFA analysis + optimization,
// emits LLVM IR, executes via LLJIT, writes the int64_t result to *out_result,
// and calls ifa_reset().
bool compile_and_jit_i64(const langwb::syntax::SyntaxObj *root, int64_t *out_result,
                         PrimRegistry *prims = nullptr, CompileInfo *out_info = nullptr);

// Convenience overload accepting a raw CstNode* directly.
bool compile_and_jit_i64(const langwb::parse::CstNode *root, int64_t *out_result,
                         PrimRegistry *prims = nullptr, CompileInfo *out_info = nullptr);

}  // namespace langwb::ifa_bridge
