#pragma once

#include <cstdint>

#include "ifa.h"
#include "parse/parse.h"

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

// Lowers a Phase-0 expression CST (integers, +, -, *, /, parens, let..in) to
// IF1, runs IFA analysis + optimization, emits LLVM IR, executes via LLJIT,
// writes the int64_t result to *out_result, and calls ifa_reset().
bool compile_and_jit_i64(const langwb::parse::CstNode *root, int64_t *out_result);

}  // namespace langwb::ifa_bridge
