#include "parse/parse.h"

#include "diag/diagnostic.h"

#include <cctype>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>

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
    case CoreFormKind::UsingSyntax: return "using_syntax";
    case CoreFormKind::Amb: return "amb";
    case CoreFormKind::Assign: return "assign";
    case CoreFormKind::SetField: return "set_field";
    case CoreFormKind::While: return "while";
  }
  return "none";
}

bool is_core_form_name(std::string_view name) {
  static constexpr CoreFormKind kAll[] = {
      CoreFormKind::Let,       CoreFormKind::LetRec,  CoreFormKind::Lambda,
      CoreFormKind::If,        CoreFormKind::App,     CoreFormKind::BinOp,
      CoreFormKind::Paren,     CoreFormKind::Record,  CoreFormKind::Field,
      CoreFormKind::Datum,     CoreFormKind::String,  CoreFormKind::Ident,
      CoreFormKind::Seq,       CoreFormKind::Define,  CoreFormKind::Assign,
      CoreFormKind::SetField,  CoreFormKind::While,
  };
  // Keep in sync with CoreFormKind (21 enumerators total minus 4 internal ones:
  // None, PassThrough, UsingSyntax, Amb).
  static_assert(sizeof(kAll) / sizeof(kAll[0]) == static_cast<size_t>(CoreFormKind::While) + 1 - 4,
                "Update kAll when adding a CoreFormKind");
  for (CoreFormKind k : kAll) {
    if (name == core_form_name(k)) return true;
  }
  return false;
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
      else if (tok == "using_syntax") meta.kind = CoreFormKind::UsingSyntax;
      else if (tok == "amb") meta.kind = CoreFormKind::Amb;
      else if (tok == "assign") meta.kind = CoreFormKind::Assign;
      else if (tok == "set_field") meta.kind = CoreFormKind::SetField;
      else if (tok == "while") meta.kind = CoreFormKind::While;
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

struct ParseAmbState {
  const DynamicGrammar *gram = nullptr;
  const D_ParserTables *tables = nullptr;
  std::unordered_map<D_ParseNode *, std::vector<CstNode *, GcAlloc<CstNode *>>> amb_alts;
};

CstNode *build_cst(const DynamicGrammar *gram, const D_ParserTables *tables, D_ParseNode *pn,
                   const ParseAmbState *amb_state = nullptr);

CstNode *build_cst_single(const DynamicGrammar *gram, const D_ParserTables *tables, D_ParseNode *pn,
                          const ParseAmbState *amb_state) {
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
  node->end_line = node->line;
  node->end_col = node->col;
  for (char ch : node->text) {
    if (ch == '\n') {
      ++node->end_line;
      node->end_col = 0;
    } else {
      ++node->end_col;
    }
  }
  node->rule_index = d_get_production(pn);
  if (gram && !node->is_terminal && node->rule_index >= 0) {
    node->core = gram->lookup_rule_meta(node->symbol, node->rule_index);
  }

  int n = d_get_number_of_children(pn);
  node->children.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    D_ParseNode *child = d_get_child(pn, i);
    if (CstNode *c = build_cst(gram, tables, child, amb_state)) {
      node->children.push_back(c);
    }
  }
  return node;
}

CstNode *build_cst(const DynamicGrammar *gram, const D_ParserTables *tables, D_ParseNode *pn,
                   const ParseAmbState *amb_state) {
  CstNode *primary = build_cst_single(gram, tables, pn, amb_state);
  if (!primary || !amb_state) return primary;

  std::vector<CstNode *> extra_alts;
  auto it = amb_state->amb_alts.find(pn);
  if (it != amb_state->amb_alts.end() && !it->second.empty()) {
    extra_alts.assign(it->second.begin(), it->second.end());
  } else {
    // DParser invokes ambiguity_fn top-down only along the winning branch, so
    // nested ambiguities inside alternative branches still sit on PNode::ambiguities.
    auto *raw =
        reinterpret_cast<PNode *>(reinterpret_cast<char *>(pn) - offsetof(PNode, parse_node));
    if (raw && raw->ambiguities) {
      std::unordered_set<D_ParseNode *> seen{pn};
      for (PNode *a = raw->ambiguities; a; a = a->ambiguities) {
        D_ParseNode *apn = &a->parse_node;
        if (seen.insert(apn).second) {
          if (CstNode *alt = build_cst_single(gram, tables, apn, amb_state)) {
            extra_alts.push_back(alt);
          }
        }
      }
    }
  }
  if (extra_alts.empty()) return primary;

  auto *amb = new CstNode();
  amb->symbol = "__amb";
  amb->text = primary->text;
  amb->line = primary->line;
  amb->col = primary->col;
  amb->end_line = primary->end_line;
  amb->end_col = primary->end_col;
  amb->core.kind = CoreFormKind::Amb;
  amb->children.reserve(1 + extra_alts.size());
  amb->children.push_back(primary);
  for (CstNode *alt : extra_alts) {
    if (alt) amb->children.push_back(alt);
  }
  return amb;
}

D_ParseNode *wb_ambiguity_fn(D_Parser *p, int n, D_ParseNode **v) {
  if (!p || n <= 0 || !v) return nullptr;
  auto *st = static_cast<ParseAmbState *>(p->initial_globals);
  if (st && n > 1 && st->amb_alts.find(v[0]) == st->amb_alts.end()) {
    auto &alts = st->amb_alts[v[0]];
    for (int i = 1; i < n; ++i) {
      if (CstNode *alt = build_cst(st->gram, st->tables, v[i], st)) {
        alts.push_back(alt);
      }
    }
  }
  return v[0];
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

namespace {

// DParser reports grammar errors through d_fail, which prints and exits by
// default. While `DynamicGrammar::compile` runs, failures longjmp back to it
// (DParser's allocations are GC-owned, so abandoning them is safe) and
// warnings become diagnostics.
jmp_buf *g_dparser_fail_jb = nullptr;
std::string *g_dparser_fail_msg = nullptr;

void on_dparser_fail(const char *msg) {
  if (g_dparser_fail_msg) *g_dparser_fail_msg = msg ? msg : "";
  if (g_dparser_fail_jb) std::longjmp(*g_dparser_fail_jb, 1);
}

void on_dparser_warn(const char *msg) {
  diag::emitf(diag::Severity::Warning, "grammar", diag::SourceRange{}, "%s\n",
              msg ? msg : "");
}

struct DParserHandlerScope {
  d_message_handler_t prev_fail;
  d_message_handler_t prev_warn;
  jmp_buf *prev_jb;
  std::string *prev_msg;
  DParserHandlerScope(jmp_buf *jb, std::string *msg)
      : prev_fail(d_set_fail_handler(on_dparser_fail)),
        prev_warn(d_set_warn_handler(on_dparser_warn)),
        prev_jb(g_dparser_fail_jb),
        prev_msg(g_dparser_fail_msg) {
    g_dparser_fail_jb = jb;
    g_dparser_fail_msg = msg;
  }
  ~DParserHandlerScope() {
    d_set_fail_handler(prev_fail);
    d_set_warn_handler(prev_warn);
    g_dparser_fail_jb = prev_jb;
    g_dparser_fail_msg = prev_msg;
  }
};

}  // namespace

bool DynamicGrammar::compile(std::string_view grammar_src) {
  if (tables_) {
    free_BinaryTables(tables_);
    tables_ = nullptr;
  }
  last_error_.clear();
  // Everything that must survive a d_fail longjmp is constructed before setjmp.
  std::string copy(grammar_src);
  jmp_buf jb;
  DParserHandlerScope handlers(&jb, &last_error_);
  if (setjmp(jb) != 0) {
    if (last_error_.empty()) last_error_ = "grammar compilation failed";
    return false;
  }
  return compile_tables(copy);
}

bool DynamicGrammar::compile_tables(std::string &copy) {
  char gram_name[] = "-";
  Grammar *g = new_D_Grammar(gram_name);
  g->set_op_priority_from_rule = 1;
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

namespace {
// Extra explanation for common misplaced forms, keyed on the word before the
// error position. `import` is a top-level declaration (`import "m.ml";`), so in
// an expression it is a bare identifier followed by a string: a syntax error.
const char *syntax_error_hint(const std::string &src, const char *err_pos) {
  if (!err_pos || err_pos < src.data() || err_pos > src.data() + src.size()) return "";
  size_t i = static_cast<size_t>(err_pos - src.data());
  while (i > 0 && std::isspace(static_cast<unsigned char>(src[i - 1]))) --i;
  size_t end = i;
  while (i > 0 && (std::isalnum(static_cast<unsigned char>(src[i - 1])) || src[i - 1] == '_')) --i;
  if (src.compare(i, end - i, "import") == 0 && end - i == 6) {
    return " ('import' is only allowed at the top level of a file, written as "
           "'import \"path\";')";
  }
  return "";
}
}  // namespace

CstNode *DynamicGrammar::parse(std::string_view input, std::string_view start_symbol,
                               bool quiet, int line_offset, int col_offset) const {
  if (!tables_ || !tables_->parser_tables_gram) return nullptr;

  ParseAmbState amb_state{this, tables_->parser_tables_gram, {}};
  D_Parser *p = new_D_Parser(tables_->parser_tables_gram, 0);
  p->save_parse_tree = 1;
  p->error_recovery = 0;
  p->initial_globals = &amb_state;
  p->ambiguity_fn = wb_ambiguity_fn;
  p->syntax_error_fn = quiet_syntax_error;
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
    root = build_cst(this, tables_->parser_tables_gram, pn, &amb_state);
  } else if (!quiet) {
    int err_line = p->loc.line + line_offset;
    int err_col = (p->loc.line == 1) ? (p->loc.col + col_offset) : p->loc.col;
    diag::emitf(diag::Severity::Error, "syntax", diag::SourceRange{nullptr, err_line, err_col},
                "syntax error%s\n",
                syntax_error_hint(copy, p->loc.s));
  }
  if (pn) free_D_ParseNode(p, pn);
  free_D_Parser(p);
  return root;
}

CstNode *DynamicGrammar::parse_prefix(std::string_view input, size_t *out_consumed,
                                      std::string_view start_symbol, bool quiet,
                                      int line_offset, int col_offset) const {
  if (!tables_ || !tables_->parser_tables_gram) return nullptr;

  ParseAmbState amb_state{this, tables_->parser_tables_gram, {}};
  D_Parser *p = new_D_Parser(tables_->parser_tables_gram, 0);
  p->save_parse_tree = 1;
  p->partial_parses = 1;
  p->error_recovery = 0;
  p->initial_globals = &amb_state;
  p->ambiguity_fn = wb_ambiguity_fn;
  p->syntax_error_fn = quiet_syntax_error;
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
    root = build_cst(this, tables_->parser_tables_gram, pn, &amb_state);
  } else if (!quiet) {
    int err_line = p->loc.line + line_offset;
    int err_col = (p->loc.line == 1) ? (p->loc.col + col_offset) : p->loc.col;
    diag::emitf(diag::Severity::Error, "syntax", diag::SourceRange{nullptr, err_line, err_col},
                "syntax error%s\n",
                syntax_error_hint(copy, p->loc.s));
  }
  if (pn) free_D_ParseNode(p, pn);
  free_D_Parser(p);
  return root;
}

bool DynamicGrammar::parse_check(std::string_view input) const {
  return parse(input, {}, /*quiet=*/true) != nullptr;
}

bool DynamicGrammar::has_start_symbol(std::string_view sym) const {
  return tables_ && tables_->parser_tables_gram &&
         find_start_state(tables_->parser_tables_gram, sym) >= 0;
}

}  // namespace langwb::parse
