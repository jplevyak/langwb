#include "parse/parse.h"

#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "d.h"
#include "dparse.h"
#include "gram.h"
#include "mkdparse.h"
#include "read_binary.h"
#include "write_tables.h"
}

namespace langwb::parse {

DynamicGrammar::~DynamicGrammar() {
  if (tables_) {
    free_BinaryTables(tables_);
    tables_ = nullptr;
  }
}

bool DynamicGrammar::compile(std::string_view grammar_src) {
  if (tables_) {
    free_BinaryTables(tables_);
    tables_ = nullptr;
  }

  char gram_name[] = "-";
  Grammar *g = new_D_Grammar(gram_name);
  g->set_op_priority_from_rule = 0;
  g->right_recursive_BNF = 0;
  g->states_for_whitespace = 1;
  g->states_for_all_nterms = 0;
  g->tokenizer = 0;
  g->longest_match = 0;
  g->save_parse_tree = 1;
  g->scanner_blocks = 4;
  g->scanner_block_size = 0;
  std::strcpy(g->grammar_ident, "gram");
  g->write_pathname = gram_name;

  std::string copy(grammar_src);
  mkdparse_from_string(g, copy.data());

  unsigned char *buf = nullptr;
  unsigned int buf_len = 0;
  int rc = write_binary_tables_to_string(g, &buf, &buf_len);
  free_D_Grammar(g);
  if (rc < 0 || !buf) {
    return false;
  }

  tables_ = read_binary_tables_from_string(buf, buf_len, nullptr, nullptr);
  FREE(buf);
  return tables_ != nullptr && tables_->parser_tables_gram != nullptr;
}

bool DynamicGrammar::parse_check(std::string_view input) const {
  if (!tables_ || !tables_->parser_tables_gram) return false;
  D_Parser *p = new_D_Parser(tables_->parser_tables_gram, 0);
  p->save_parse_tree = 1;
  std::string copy(input);
  D_ParseNode *pn = dparse(p, copy.data(), static_cast<int>(copy.size()));
  bool ok = (pn != nullptr && p->syntax_errors == 0);
  if (pn) free_D_ParseNode(p, pn);
  free_D_Parser(p);
  return ok;
}

}  // namespace langwb::parse
