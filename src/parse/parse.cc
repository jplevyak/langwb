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

namespace {

CstNode *build_cst(const D_ParserTables *tables, D_ParseNode *pn) {
  if (!pn) return nullptr;

  auto *node = new CstNode();
  if (pn->symbol >= 0 && static_cast<unsigned int>(pn->symbol) < tables->nsymbols) {
    const D_Symbol &sym = tables->symbols[pn->symbol];
    if (sym.name) {
      node->symbol.assign(sym.name, sym.name_len > 0 ? sym.name_len : std::strlen(sym.name));
    }
    node->is_terminal =
        (sym.kind == D_SYMBOL_STRING || sym.kind == D_SYMBOL_REGEX || sym.kind == D_SYMBOL_TOKEN);
  }

  if (pn->start_loc.s && pn->end && pn->end >= pn->start_loc.s) {
    node->text.assign(pn->start_loc.s, static_cast<size_t>(pn->end - pn->start_loc.s));
  }
  node->line = pn->start_loc.line;
  node->col = pn->start_loc.col;

  int n = d_get_number_of_children(pn);
  node->children.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    D_ParseNode *child = d_get_child(pn, i);
    if (CstNode *c = build_cst(tables, child)) {
      node->children.push_back(c);
    }
  }
  return node;
}

int find_start_state(const D_ParserTables *tables, std::string_view sym_name) {
  for (unsigned int i = 0; i < tables->nsymbols; ++i) {
    const D_Symbol &sym = tables->symbols[i];
    if (!sym.name) continue;
    std::string_view name(sym.name, sym.name_len > 0 ? sym.name_len : std::strlen(sym.name));
    if (name == sym_name && sym.start_symbol >= 0) {
      return sym.start_symbol;
    }
  }
  return -1;
}

}  // namespace

void dump_cst(const CstNode *node, FILE *fp, int indent) {
  if (!node) return;
  for (int i = 0; i < indent; ++i) std::fputc(' ', fp);
  std::fprintf(fp, "%s \"%s\" (%d:%d)\n", node->symbol.c_str(), node->text.c_str(), node->line,
               node->col);
  for (const CstNode *child : node->children) {
    dump_cst(child, fp, indent + 2);
  }
}

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
  g->states_for_all_nterms = 1;  // allow parsing starting at any non-terminal
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

static void quiet_syntax_error(struct D_Parser *) {}

CstNode *DynamicGrammar::parse(std::string_view input, std::string_view start_symbol,
                               bool quiet) const {
  if (!tables_ || !tables_->parser_tables_gram) return nullptr;

  D_Parser *p = new_D_Parser(tables_->parser_tables_gram, 0);
  p->save_parse_tree = 1;
  if (quiet) {
    p->syntax_error_fn = quiet_syntax_error;
  }
  if (!start_symbol.empty()) {
    int st = find_start_state(tables_->parser_tables_gram, start_symbol);
    if (st < 0) {
      free_D_Parser(p);
      return nullptr;
    }
    p->start_state = st;
  }

  std::string copy(input);
  D_ParseNode *pn = dparse(p, copy.data(), static_cast<int>(copy.size()));
  CstNode *root = nullptr;
  if (pn && p->syntax_errors == 0) {
    root = build_cst(tables_->parser_tables_gram, pn);
  }
  if (pn) free_D_ParseNode(p, pn);
  free_D_Parser(p);
  return root;
}

bool DynamicGrammar::parse_check(std::string_view input) const {
  return parse(input, {}, /*quiet=*/true) != nullptr;
}

}  // namespace langwb::parse
