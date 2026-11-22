#include "parse/parse.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "d.h"
#include "dparse.h"
#include "dparse_tables.h"
#include "gram.h"
#include "mkdparse.h"
#include "read_binary.h"
#include "write_tables.h"
}

namespace langwb::parse {

const char *core_form_name(CoreFormKind kind) {
  switch (kind) {
    case CoreFormKind::None: return "none";
    case CoreFormKind::Let: return "let";
    case CoreFormKind::LetRec: return "letrec";
    case CoreFormKind::Lambda: return "lambda";
    case CoreFormKind::If: return "if";
    case CoreFormKind::App: return "app";
    case CoreFormKind::BinOp: return "binop";
    case CoreFormKind::Paren: return "paren";
    case CoreFormKind::Record: return "record";
    case CoreFormKind::Field: return "field";
    case CoreFormKind::Datum: return "datum";
    case CoreFormKind::String: return "string";
    case CoreFormKind::Ident: return "ident";
    case CoreFormKind::PassThrough: return "passthrough";
    case CoreFormKind::Seq: return "seq";
    case CoreFormKind::Define: return "define";
  }
  return "none";
}

bool parse_core_annotation(std::string_view spec, CoreRuleMeta *out) {
  if (!out) return false;
  auto trim = [](std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
  };
  spec = trim(spec);
  if (spec.empty()) return false;

  CoreRuleMeta meta;
  size_t pos = 0;
  bool first = true;
  while (pos < spec.size()) {
    size_t comma = spec.find(',', pos);
    std::string_view tok =
        trim(comma == std::string_view::npos ? spec.substr(pos) : spec.substr(pos, comma - pos));
    pos = (comma == std::string_view::npos) ? spec.size() : comma + 1;
    if (tok.empty()) continue;

    if (first) {
      first = false;
      if (tok == "let") meta.kind = CoreFormKind::Let;
      else if (tok == "letrec") meta.kind = CoreFormKind::LetRec;
      else if (tok == "lambda") meta.kind = CoreFormKind::Lambda;
      else if (tok == "if") meta.kind = CoreFormKind::If;
      else if (tok == "app") meta.kind = CoreFormKind::App;
      else if (tok == "binop") meta.kind = CoreFormKind::BinOp;
      else if (tok == "paren") meta.kind = CoreFormKind::Paren;
      else if (tok == "record") meta.kind = CoreFormKind::Record;
      else if (tok == "field") meta.kind = CoreFormKind::Field;
      else if (tok == "datum") meta.kind = CoreFormKind::Datum;
      else if (tok == "string") meta.kind = CoreFormKind::String;
      else if (tok == "ident") meta.kind = CoreFormKind::Ident;
      else if (tok == "passthrough") meta.kind = CoreFormKind::PassThrough;
      else if (tok == "seq") meta.kind = CoreFormKind::Seq;
      else if (tok == "define") meta.kind = CoreFormKind::Define;
      else return false;
      continue;
    }

    size_t eq = tok.find('=');
    if (eq == std::string_view::npos) return false;
    std::string_view key = trim(tok.substr(0, eq));
    std::string_view val_s = trim(tok.substr(eq + 1));
    if (val_s.empty()) return false;
    char *endp = nullptr;
    std::string val_copy(val_s);
    long idx = std::strtol(val_copy.c_str(), &endp, 10);
    if (!endp || *endp != '\0' || idx < 0 || idx > 120) return false;
    auto i8 = static_cast<int8_t>(idx);

    if (key == "bind" || key == "params") meta.bind = i8;
    else if (key == "rhs") meta.rhs = i8;
    else if (key == "body") meta.body = i8;
    else if (key == "test") meta.test = i8;
    else if (key == "then") meta.then_b = i8;
    else if (key == "else") meta.else_b = i8;
    else if (key == "fn") meta.fn = i8;
    else if (key == "args") meta.args = i8;
    else if (key == "lhs") meta.lhs = i8;
    else if (key == "op") meta.op = i8;
    else if (key == "inner") meta.inner = i8;
    else if (key == "fields") meta.fields = i8;
    else if (key == "obj") meta.obj = i8;
    else if (key == "field") meta.field = i8;
    else return false;
  }

  if (meta.kind == CoreFormKind::None) return false;
  *out = meta;
  return true;
}

namespace {

CstNode *build_cst(const DynamicGrammar *gram, const D_ParserTables *tables, D_ParseNode *pn) {
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
  node->rule_index = d_get_production(pn);
  if (gram && !node->is_terminal && node->rule_index >= 0) {
    node->core = gram->lookup_rule_meta(node->symbol, node->rule_index);
  }

  int n = d_get_number_of_children(pn);
  node->children.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    D_ParseNode *child = d_get_child(pn, i);
    if (CstNode *c = build_cst(gram, tables, child)) {
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

  // Assign a distinct rule-index action_index to every non-internal rule so
  // DParser's find_same_reductions() never merges distinct rules of the same
  // production and d_get_production(pn) always recovers the exact rule index.
  for (unsigned int i = 0; i < g->productions.n; ++i) {
    Production *p = g->productions.v[i];
    if (p->internal || p->regex) continue;
    for (unsigned int j = 0; j < p->rules.n; ++j) {
      Rule *r = p->rules.v[j];
      if (r->action_index < 0) {
        r->action_index = p->index * D_RULE_INDEX_SCALE + static_cast<int>(j) + D_RULE_INDEX_BASE;
      }
    }
  }

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

void DynamicGrammar::set_rule_meta(std::string prod_name, int rule_index, CoreRuleMeta meta) {
  if (rule_index < 0) return;
  auto &vec = rule_meta_[std::move(prod_name)];
  if (static_cast<size_t>(rule_index) >= vec.size()) {
    vec.resize(static_cast<size_t>(rule_index) + 1);
  }
  vec[static_cast<size_t>(rule_index)] = meta;
}

CoreRuleMeta DynamicGrammar::lookup_rule_meta(const std::string &prod_name, int rule_index) const {
  if (rule_index < 0) return {};
  auto it = rule_meta_.find(prod_name);
  if (it == rule_meta_.end() || static_cast<size_t>(rule_index) >= it->second.size()) {
    return {};
  }
  return it->second[static_cast<size_t>(rule_index)];
}

static void quiet_syntax_error(struct D_Parser *) {}

CstNode *DynamicGrammar::parse(std::string_view input, std::string_view start_symbol,
                               bool quiet) const {
  if (!tables_ || !tables_->parser_tables_gram) return nullptr;

  D_Parser *p = new_D_Parser(tables_->parser_tables_gram, 0);
  p->save_parse_tree = 1;
  p->error_recovery = 0;
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
    root = build_cst(this, tables_->parser_tables_gram, pn);
  } else if (!quiet) {
    std::fprintf(stderr, "langwb: %d:%d: syntax error\n", p->loc.line, p->loc.col);
  }
  if (pn) free_D_ParseNode(p, pn);
  free_D_Parser(p);
  return root;
}

CstNode *DynamicGrammar::parse_prefix(std::string_view input, size_t *out_consumed,
                                      std::string_view start_symbol, bool quiet) const {
  if (!tables_ || !tables_->parser_tables_gram) return nullptr;

  D_Parser *p = new_D_Parser(tables_->parser_tables_gram, 0);
  p->save_parse_tree = 1;
  p->partial_parses = 1;
  p->error_recovery = 0;
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
  if (pn && p->syntax_errors == 0 && pn->end && pn->end >= copy.data()) {
    if (out_consumed) *out_consumed = static_cast<size_t>(pn->end - copy.data());
    root = build_cst(this, tables_->parser_tables_gram, pn);
  }
  if (pn) free_D_ParseNode(p, pn);
  free_D_Parser(p);
  return root;
}

bool DynamicGrammar::parse_check(std::string_view input) const {
  return parse(input, {}, /*quiet=*/true) != nullptr;
}

}  // namespace langwb::parse
