#include "workbench/workbench.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ifa_bridge/meta_abi.h"
#include "llvm_internal.h"

extern "C" int64_t _CG_wb_pow_i64(int64_t base, int64_t exp) {
  if (exp < 0) return 0;
  uint64_t res = 1;
  uint64_t b = static_cast<uint64_t>(base);
  uint64_t e = static_cast<uint64_t>(exp);
  while (e > 0) {
    if (e & 1ULL) res *= b;
    e >>= 1;
    if (e > 0) b *= b;
  }
  return static_cast<int64_t>(res);
}

namespace langwb {

namespace {

void dump_attrs_tree(const syntax::SyntaxObj *node, const ifa_bridge::AnalysisAttrs &attrs,
                     FILE *fp, int indent = 0) {
  if (!node) return;
  const auto *a = attrs.get(node);
  if (a && a->analyzed) {
    for (int i = 0; i < indent; ++i) std::fputc(' ', fp);
    std::fprintf(fp, "%s \"%s\" [id=%llu", node->symbol.c_str(), node->text.c_str(),
                 static_cast<unsigned long long>(node->node_id));
    if (!a->type_names.empty()) {
      std::fprintf(fp, " type=");
      for (size_t i = 0; i < a->type_names.size(); ++i) {
        if (i > 0) std::fputc('|', fp);
        std::fprintf(fp, "%s", a->type_names[i].c_str());
      }
    }
    if (a->has_const_i64) {
      std::fprintf(fp, " const=%lld", static_cast<long long>(a->const_i64));
    }
    if (!a->field_names.empty()) {
      std::fprintf(fp, " fields={");
      for (size_t i = 0; i < a->field_names.size(); ++i) {
        if (i > 0) std::fputc(',', fp);
        std::fprintf(fp, "%s", a->field_names[i].c_str());
      }
      std::fputc('}', fp);
    }
    if (a->contour_count > 1) {
      std::fprintf(fp, " ctr=%d", a->contour_count);
    }
    std::fprintf(fp, "]\n");
  }
  for (const syntax::SyntaxObj *c : node->children) {
    dump_attrs_tree(c, attrs, fp, indent + 2);
  }
}

std::string_view trim_ws(std::string_view sv) {
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
  return sv;
}

bool match_keyword(std::string_view sv, std::string_view kw) {
  if (!sv.starts_with(kw)) return false;
  if (sv.size() == kw.size()) return true;
  unsigned char next = static_cast<unsigned char>(sv[kw.size()]);
  return std::isspace(next);
}

// Finds the next `;` outside of string literals and nested parens/braces.
size_t find_decl_terminator(std::string_view sv) {
  bool in_str = false;
  int depth = 0;
  for (size_t i = 0; i < sv.size(); ++i) {
    char c = sv[i];
    if (in_str) {
      if (c == '\\' && i + 1 < sv.size()) {
        ++i;
      } else if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      in_str = true;
    } else if (c == '(' || c == '{') {
      ++depth;
    } else if (c == ')' || c == '}') {
      if (depth > 0) --depth;
    } else if (c == ';' && depth == 0) {
      return i;
    }
  }
  return std::string_view::npos;
}

bool parse_quoted_string(std::string_view &sv, std::string &out) {
  sv = trim_ws(sv);
  if (sv.empty() || sv.front() != '"') return false;
  sv.remove_prefix(1);
  out.clear();
  while (!sv.empty()) {
    char c = sv.front();
    sv.remove_prefix(1);
    if (c == '\\' && !sv.empty()) {
      char next = sv.front();
      sv.remove_prefix(1);
      out.push_back(next);
    } else if (c == '"') {
      return true;
    } else {
      out.push_back(c);
    }
  }
  return false;
}

bool parse_ident(std::string_view &sv, std::string &out) {
  sv = trim_ws(sv);
  if (sv.empty() || !(std::isalpha(static_cast<unsigned char>(sv[0])) || sv[0] == '_')) {
    return false;
  }
  size_t i = 1;
  while (i < sv.size() && (std::isalnum(static_cast<unsigned char>(sv[i])) || sv[i] == '_')) {
    ++i;
  }
  out.assign(sv.substr(0, i));
  sv.remove_prefix(i);
  return true;
}

}  // namespace

Workbench::Workbench()
    : cache_(std::make_shared<parse::GrammarCache>()),
      stats_(std::make_shared<EvalStats>()),
      lang_search_paths_{".", "langs", "examples/langs"} {
  init_decl_grammar();
  init_builtin_prims();
  init_default_minilang();
}

Workbench::Workbench(BareTag)
    : cache_(std::make_shared<parse::GrammarCache>()),
      stats_(std::make_shared<EvalStats>()),
      lang_search_paths_{".", "langs", "examples/langs"} {
  init_decl_grammar();
  init_builtin_prims();
}

Workbench Workbench::create_bare() { return Workbench(BareTag{}); }

Workbench::Workbench(const Workbench &parent, bool /*share_cache*/)
    : grammar_def_(parent.grammar_def_),
      decl_grammar_(parent.decl_grammar_),
      cache_(parent.cache_),
      prims_(parent.prims_),
      stage0_(parent.stage0_),
      stage1_(parent.stage1_),
      stats_(parent.stats_),
      lang_search_paths_(parent.lang_search_paths_),
      for_syntax_defs_(parent.for_syntax_defs_) {}

void Workbench::init_decl_grammar() {
  decl_grammar_.add_rule("decl", "decl_grammar");
  decl_grammar_.add_rule("decl", "decl_prim");
  decl_grammar_.add_rule("decl", "decl_hook");
  decl_grammar_.add_rule("decl", "decl_for_syntax");
  decl_grammar_.add_rule("decl", "decl_syntax_rule");
  decl_grammar_.add_rule("decl", "decl_macro");

  decl_grammar_.add_rule("decl_grammar", "'grammar' ident '+=' str str? core_ann? ';'");
  decl_grammar_.add_rule("core_ann", "'core' '(' core_body ')'");
  decl_grammar_.add_rule("core_body", "\"[^)]+\"");

  decl_grammar_.add_rule("decl_prim", "'prim' ident 'c_call' str ('transfer' '=>' body_text)? ';'");
  decl_grammar_.add_rule("decl_hook", "'hook' ident '=>' body_text ';'");
  decl_grammar_.add_rule("decl_for_syntax", "'for_syntax' ident '=>' body_text ';'");
  decl_grammar_.add_rule("decl_syntax_rule",
                         "'syntax_rule' 'form'? ident '(' slot_list? ')' '=>' str ';'");
  decl_grammar_.add_rule("slot_list", "\"[^)]+\"");
  decl_grammar_.add_rule("decl_macro", "macro_kw ident '=>' body_text ';'");
  decl_grammar_.add_rule("macro_kw", "'macro'");
  decl_grammar_.add_rule("macro_kw", "'form_macro'");
  decl_grammar_.add_rule("macro_kw", "'stage1_macro'");
  decl_grammar_.add_rule("macro_kw", "'stage1_form_macro'");

  decl_grammar_.add_rule("body_text", "body_atom+");
  decl_grammar_.add_rule("body_atom", "'(' nested_text? ')'");
  decl_grammar_.add_rule("body_atom", "'{' nested_text? '}'");
  decl_grammar_.add_rule("body_atom", "str");
  decl_grammar_.add_rule("body_atom", "\"[^;(){}\\\"]+\"");

  decl_grammar_.add_rule("nested_text", "nested_atom+");
  decl_grammar_.add_rule("nested_atom", "'(' nested_text? ')'");
  decl_grammar_.add_rule("nested_atom", "'{' nested_text? '}'");
  decl_grammar_.add_rule("nested_atom", "str");
  decl_grammar_.add_rule("nested_atom", "\"[^(){}\\\"]+\"");

  decl_grammar_.add_rule("ident", R"d("[a-zA-Z_][a-zA-Z0-9_]*")d");
  decl_grammar_.add_rule("str", R"d("\"([^\"\\]|\\[^])*\"")d");
}

std::shared_ptr<parse::DynamicGrammar> Workbench::compiled_decl_grammar() {
  return cache_->compile_or_get(decl_grammar_);
}

std::string Workbench::strip_comments(std::string_view src) {
  std::string out;
  out.reserve(src.size());
  bool in_comment = false;
  bool in_str = false;
  for (size_t i = 0; i < src.size(); ++i) {
    char c = src[i];
    if (in_comment) {
      if (c == '\n') {
        in_comment = false;
        out.push_back('\n');
      }
      continue;
    }
    if (in_str) {
      out.push_back(c);
      if (c == '\\' && i + 1 < src.size()) {
        out.push_back(src[++i]);
      } else if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      in_str = true;
      out.push_back(c);
    } else if (c == '#') {
      in_comment = true;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool Workbench::starts_with_decl(std::string_view src) {
  src = trim_ws(src);
  return match_keyword(src, "grammar") || match_keyword(src, "macro") ||
         match_keyword(src, "form_macro") || match_keyword(src, "stage1_macro") ||
         match_keyword(src, "stage1_form_macro") || match_keyword(src, "prim") ||
         match_keyword(src, "hook") || match_keyword(src, "syntax_rule") ||
         match_keyword(src, "for_syntax");
}

bool Workbench::extract_lang_header(std::string_view &src, std::string *out_lang) {
  std::string_view sv = src;
  while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' ||
                         sv.front() == '\n')) {
    sv.remove_prefix(1);
  }
  if (!sv.starts_with("#lang")) return false;
  if (sv.size() > 5 && !std::isspace(static_cast<unsigned char>(sv[5]))) return false;

  size_t eol = sv.find('\n');
  std::string_view header = (eol == std::string_view::npos) ? sv : sv.substr(0, eol);
  std::string_view rest = (eol == std::string_view::npos) ? std::string_view{} : sv.substr(eol + 1);

  header.remove_prefix(5);
  header = trim_ws(header);
  if (header.empty()) return false;
  if (out_lang) out_lang->assign(header);
  src = rest;
  return true;
}

bool Workbench::load_lang(std::string_view lang_name, int depth) {
  if (depth > 8) {
    std::fprintf(stderr, "langwb: #lang inheritance cycle or depth exceeded\n");
    return false;
  }

  namespace fs = std::filesystem;
  std::string found_path;
  std::vector<fs::path> candidates;
  candidates.emplace_back(std::string(lang_name));
  candidates.emplace_back(std::string(lang_name) + ".lang");
  for (const auto &dir : lang_search_paths_) {
    candidates.emplace_back(fs::path(dir) / (std::string(lang_name) + ".lang"));
    candidates.emplace_back(fs::path(dir) / std::string(lang_name));
  }
  for (const auto &p : candidates) {
    if (fs::is_regular_file(p)) {
      found_path = p.string();
      break;
    }
  }
  if (found_path.empty()) {
    std::fprintf(stderr, "langwb: cannot find #lang module '%.*s'\n",
                 static_cast<int>(lang_name.size()), lang_name.data());
    return false;
  }

  std::ifstream ifs(found_path);
  if (!ifs) return false;
  std::ostringstream oss;
  oss << ifs.rdbuf();
  std::string raw = oss.str();

  std::string_view sv(raw);
  std::string parent_lang;
  if (extract_lang_header(sv, &parent_lang)) {
    if (!load_lang(parent_lang, depth + 1)) return false;
  }

  auto decl_gram = compiled_decl_grammar();
  if (!decl_gram) return false;

  std::string cleaned = strip_comments(sv);
  std::string_view rest = trim_ws(cleaned);
  while (!rest.empty()) {
    size_t consumed = 0;
    parse::CstNode *cst = decl_gram->parse_prefix(rest, &consumed, "decl", /*quiet=*/true);
    if (!cst || consumed == 0) {
      std::fprintf(stderr, "langwb: invalid declaration in '%s' near '%.*s'\n", found_path.c_str(),
                   static_cast<int>(std::min<size_t>(rest.size(), 60)), rest.data());
      return false;
    }
    if (!apply_decl_cst(cst)) {
      std::fprintf(stderr, "langwb: failed to apply declaration in '%s': '%.*s'\n",
                   found_path.c_str(), static_cast<int>(consumed), rest.data());
      return false;
    }
    rest = trim_ws(rest.substr(consumed));
  }
  return true;
}

namespace {

const parse::CstNode *find_cst_sym(const parse::CstNode *n, std::string_view sym) {
  if (!n) return nullptr;
  if (n->symbol == sym) return n;
  for (const parse::CstNode *c : n->children) {
    if (const parse::CstNode *found = find_cst_sym(c, sym)) return found;
  }
  return nullptr;
}

void collect_cst_sym(const parse::CstNode *n, std::string_view sym,
                     std::vector<const parse::CstNode *> &out) {
  if (!n) return;
  if (n->symbol == sym) {
    out.push_back(n);
    return;
  }
  for (const parse::CstNode *c : n->children) {
    collect_cst_sym(c, sym, out);
  }
}

bool unquote_cst_str(std::string_view quoted, std::string &out) {
  return parse_quoted_string(quoted, out);
}

}  // namespace

bool Workbench::apply_decl_cst(const parse::CstNode *decl_cst) {
  if (!decl_cst) return false;
  const parse::CstNode *d =
      (decl_cst->symbol == "decl" && decl_cst->children.size() == 1) ? decl_cst->children[0]
                                                                     : decl_cst;
  if (!d) return false;

  // 1. grammar <prod> += "<rhs>" ["<modifier>"] [core(<kind>, role=idx, ...)];
  if (d->symbol == "decl_grammar") {
    const parse::CstNode *id = find_cst_sym(d, "ident");
    std::vector<const parse::CstNode *> strs;
    collect_cst_sym(d, "str", strs);
    if (!id || strs.empty() || strs.size() > 2) return false;
    std::string rhs;
    if (!unquote_cst_str(strs[0]->text, rhs)) return false;
    std::string mod;
    if (strs.size() == 2 && !unquote_cst_str(strs[1]->text, mod)) return false;
    parse::CoreRuleMeta core;
    if (const parse::CstNode *cb = find_cst_sym(d, "core_body")) {
      if (!parse::parse_core_annotation(cb->text, &core)) return false;
    }
    grammar_def_.add_rule(id->text, std::move(rhs), std::move(mod), core);
    ++stats_->decls_applied;
    return true;
  }

  // 2. prim <name> c_call "<c_sym>" [transfer => <minilang-expr>];
  if (d->symbol == "decl_prim") {
    const parse::CstNode *id = find_cst_sym(d, "ident");
    const parse::CstNode *s = find_cst_sym(d, "str");
    if (!id || !s) return false;
    std::string c_sym;
    if (!unquote_cst_str(s->text, c_sym)) return false;
    std::string transfer_src;
    if (const parse::CstNode *bt = find_cst_sym(d, "body_text")) {
      transfer_src = std::string(trim_ws(bt->text));
    }
    if (!register_guest_prim(id->text, std::move(c_sym), std::move(transfer_src))) return false;
    ++stats_->decls_applied;
    return true;
  }

  // 3. hook <module_begin|datum|app|field|record> => <minilang-expr>;
  if (d->symbol == "decl_hook") {
    const parse::CstNode *id = find_cst_sym(d, "ident");
    const parse::CstNode *bt = find_cst_sym(d, "body_text");
    if (!id || !bt) return false;
    if (!register_guest_hook(id->text, std::string(trim_ws(bt->text)))) return false;
    ++stats_->decls_applied;
    return true;
  }

  // 4. for_syntax <name> => <minilang-expr>;
  if (d->symbol == "decl_for_syntax") {
    const parse::CstNode *id = find_cst_sym(d, "ident");
    const parse::CstNode *bt = find_cst_sym(d, "body_text");
    if (!id || !bt) return false;
    add_for_syntax(id->text, std::string(trim_ws(bt->text)));
    ++stats_->decls_applied;
    return true;
  }

  // 5. syntax_rule [form] <name>(<slots...>) => "<template>";
  if (d->symbol == "decl_syntax_rule") {
    bool is_form = false;
    for (const parse::CstNode *c : d->children) {
      if (c && c->text == "form") is_form = true;
    }
    const parse::CstNode *id = find_cst_sym(d, "ident");
    const parse::CstNode *s = find_cst_sym(d, "str");
    if (!id || !s) return false;
    std::string tmpl;
    if (!unquote_cst_str(s->text, tmpl)) return false;

    std::vector<std::pair<std::string, std::string>> slots;
    bool trailing_variadic = false;
    if (const parse::CstNode *sl = find_cst_sym(d, "slot_list")) {
      std::string_view slot_list = trim_ws(sl->text);
      while (!slot_list.empty()) {
        slot_list = trim_ws(slot_list);
        if (slot_list.empty()) break;
        if (slot_list.front() == ',') {
          slot_list.remove_prefix(1);
          continue;
        }
        if (trailing_variadic) return false;  // variadic slot must be last
        if (slot_list.front() == '_') {
          slots.emplace_back("", "");
          slot_list.remove_prefix(1);
          continue;
        }
        if (slot_list.front() == '$') {
          slot_list.remove_prefix(1);
          std::string vname;
          if (!parse_ident(slot_list, vname)) return false;
          std::string sclass;
          if (!slot_list.empty() && slot_list.front() == ':') {
            slot_list.remove_prefix(1);
            if (!parse_ident(slot_list, sclass)) return false;
          }
          if (slot_list.starts_with("...")) {
            slot_list.remove_prefix(3);
            trailing_variadic = true;
          }
          slots.emplace_back(std::move(vname), std::move(sclass));
          continue;
        }
        return false;
      }
    }
    if (!register_syntax_rule(is_form, id->text, std::move(slots), std::move(tmpl),
                              trailing_variadic)) {
      return false;
    }
    ++stats_->decls_applied;
    return true;
  }

  // 6. [stage1_][form_]macro <name> => <minilang-expr>;
  if (d->symbol == "decl_macro") {
    const parse::CstNode *kw = find_cst_sym(d, "macro_kw");
    const parse::CstNode *id = find_cst_sym(d, "ident");
    const parse::CstNode *bt = find_cst_sym(d, "body_text");
    if (!kw || !id || !bt) return false;
    std::string body(trim_ws(bt->text));
    if (kw->text == "macro") {
      register_guest_stage0_macro(id->text, std::move(body));
    } else if (kw->text == "form_macro") {
      register_guest_stage0_form_macro(id->text, std::move(body));
    } else if (kw->text == "stage1_macro") {
      register_guest_stage1_macro(id->text, std::move(body));
    } else if (kw->text == "stage1_form_macro") {
      register_guest_stage1_form_macro(id->text, std::move(body));
    } else {
      return false;
    }
    ++stats_->decls_applied;
    return true;
  }

  return false;
}

bool Workbench::apply_decl(std::string_view decl_src) {
  std::string_view sv = trim_ws(decl_src);
  if (sv.empty()) return false;
  std::string with_semi(sv);
  if (with_semi.back() != ';') with_semi.push_back(';');
  auto decl_gram = compiled_decl_grammar();
  if (!decl_gram) return false;
  parse::CstNode *cst = decl_gram->parse(with_semi, "decl", /*quiet=*/true);
  if (!cst) return false;
  return apply_decl_cst(cst);
}

void Workbench::init_default_minilang() {
  using namespace ifa_bridge;
  using namespace macro;
  using namespace syntax;

  // Core minilang grammar with declarative `CoreRuleMeta` role annotations.
  using parse::CoreFormKind;
  using parse::CoreRuleMeta;
  const CoreRuleMeta kBinOp{.kind = CoreFormKind::BinOp, .rhs = 2, .lhs = 0, .op = 1};

  grammar_def_.add_rule("expr", "'let' \"[a-zA-Z_][a-zA-Z0-9_]*\" '=' expr 'in' expr", "$right 0",
                        CoreRuleMeta{.kind = CoreFormKind::Let, .bind = 1, .rhs = 3, .body = 5});
  grammar_def_.add_rule("expr", "'rec' \"[a-zA-Z_][a-zA-Z0-9_]*\" '=' expr 'in' expr", "$right 0",
                        CoreRuleMeta{.kind = CoreFormKind::LetRec, .bind = 1, .rhs = 3, .body = 5});
  grammar_def_.add_rule("expr", "'if' expr 'then' expr 'else' expr", "$right 0",
                        CoreRuleMeta{.kind = CoreFormKind::If, .test = 1, .then_b = 3, .else_b = 5});
  grammar_def_.add_rule("expr", "'fn' '(' params ')' '=>' expr", "$right 0",
                        CoreRuleMeta{.kind = CoreFormKind::Lambda, .bind = 2, .body = 5});
  grammar_def_.add_rule("expr", "expr '==' expr", "$binary_op_left 1", kBinOp);
  grammar_def_.add_rule("expr", "expr '!=' expr", "$binary_op_left 1", kBinOp);
  grammar_def_.add_rule("expr", "expr '<' expr", "$binary_op_left 2", kBinOp);
  grammar_def_.add_rule("expr", "expr '<=' expr", "$binary_op_left 2", kBinOp);
  grammar_def_.add_rule("expr", "expr '>' expr", "$binary_op_left 2", kBinOp);
  grammar_def_.add_rule("expr", "expr '>=' expr", "$binary_op_left 2", kBinOp);
  grammar_def_.add_rule("expr", "expr '+' expr", "$binary_op_left 3", kBinOp);
  grammar_def_.add_rule("expr", "expr '-' expr", "$binary_op_left 3", kBinOp);
  grammar_def_.add_rule("expr", "expr '*' expr", "$binary_op_left 4", kBinOp);
  grammar_def_.add_rule("expr", "expr '/' expr", "$binary_op_left 4", kBinOp);
  grammar_def_.add_rule("expr", "expr '.' \"[a-zA-Z_][a-zA-Z0-9_]*\"", "$left 5",
                        CoreRuleMeta{.kind = CoreFormKind::Field, .obj = 0, .field = 2});
  grammar_def_.add_rule("expr", "'{' fields '}'", {},
                        CoreRuleMeta{.kind = CoreFormKind::Record, .fields = 1});
  grammar_def_.add_rule("expr", "'{' stmts '}'", {},
                        CoreRuleMeta{.kind = CoreFormKind::Seq, .inner = 1});
  grammar_def_.add_rule("expr", "expr '(' args ')'", "$left 5",
                        CoreRuleMeta{.kind = CoreFormKind::App, .fn = 0, .args = 2});
  grammar_def_.add_rule("expr", "expr '(' ')'", "$left 5",
                        CoreRuleMeta{.kind = CoreFormKind::App, .fn = 0});
  grammar_def_.add_rule("expr", "'(' expr ')'", {},
                        CoreRuleMeta{.kind = CoreFormKind::Paren, .inner = 1});
  grammar_def_.add_rule("expr", "\"\\\"[^\\\"]*\\\"\"", {},
                        CoreRuleMeta{.kind = CoreFormKind::String});
  grammar_def_.add_rule("expr", "\"[0-9]+\"", {}, CoreRuleMeta{.kind = CoreFormKind::Datum});
  grammar_def_.add_rule("expr", "\"[a-zA-Z_][a-zA-Z0-9_]*\"", {},
                        CoreRuleMeta{.kind = CoreFormKind::Ident});
  grammar_def_.add_rule("args", "expr (',' expr)*");
  grammar_def_.add_rule("params", "\"[a-zA-Z_][a-zA-Z0-9_]*\" (',' \"[a-zA-Z_][a-zA-Z0-9_]*\")*");
  grammar_def_.add_rule("fields", "field_init (',' field_init)*");
  grammar_def_.add_rule("field_init", "\"[a-zA-Z_][a-zA-Z0-9_]*\" ':' expr");
  grammar_def_.add_rule("stmts", "stmt (';' stmt)*");
  grammar_def_.add_rule("stmt", "'def' \"[a-zA-Z_][a-zA-Z0-9_]*\" '=' expr", "$right 0",
                        CoreRuleMeta{.kind = CoreFormKind::Define, .bind = 1, .rhs = 3});
  grammar_def_.add_rule("stmt", "expr", {},
                        CoreRuleMeta{.kind = CoreFormKind::PassThrough, .inner = 0});

  // Composable syntax extensions: `unless` and anaphoric `aif`.
  parse::GrammarDef ext;
  ext.add_rule("expr", "unless_expr", {},
               CoreRuleMeta{.kind = CoreFormKind::PassThrough, .inner = 0});
  ext.add_rule("expr", "aif_expr", {}, CoreRuleMeta{.kind = CoreFormKind::PassThrough, .inner = 0});
  ext.add_rule("unless_expr", "'unless' expr 'then' expr 'else' expr", "$right 0");
  ext.add_rule("aif_expr", "'aif' expr 'then' expr 'else' expr", "$right 0");
  grammar_def_.extend(ext);

  // Minilang-specific Stage-0 macros (can also be defined in `prelude.lang`).
  stage0_.register_call_macro("with_tmp", [](SyntaxObj *call, MacroContext &ctx) -> SyntaxObj * {
    auto args = MacroExpander::collect_call_args(call->children[2]);
    if (args.size() != 2) return nullptr;
    return ctx.parse_template("expr", "let tmp = __val__ in tmp + __body__",
                              {{"__val__", args[0]}, {"__body__", args[1]}});
  });

  stage0_.register_form_macro("unless_expr", [](SyntaxObj *form, MacroContext &ctx) -> SyntaxObj * {
    if (form->children.size() != 6) return nullptr;
    return ctx.parse_template("expr", "if __c__ then __e__ else __t__",
                              {{"__c__", form->children[1]},
                               {"__t__", form->children[3]},
                               {"__e__", form->children[5]}});
  });

  stage0_.register_form_macro("aif_expr", [](SyntaxObj *form, MacroContext &ctx) -> SyntaxObj * {
    if (form->children.size() != 6) return nullptr;
    SyntaxObj *it_id = ctx.unhygienic_ident("it");
    return ctx.parse_template("expr", "let __it__ = __c__ in if __it__ != 0 then __t__ else __e__",
                              {{"__it__", it_id},
                               {"__c__", form->children[1]},
                               {"__t__", form->children[3]},
                               {"__e__", form->children[5]}});
  });
}

void Workbench::init_builtin_prims() {
  using namespace ifa_bridge;
  using namespace macro;
  using namespace syntax;

  // Builtin frontend-injected primitives.
  prims_.register_prim(
      "clamp",
      PrimitiveHandler{
          .transfer =
              [](PNode *pn, EntrySet *es) {
                int64_t x = 0, lo = 0, hi = 0;
                if (try_get_const_i64(prim_arg_avar(pn, es, 0), &x) &&
                    try_get_const_i64(prim_arg_avar(pn, es, 1), &lo) &&
                    try_get_const_i64(prim_arg_avar(pn, es, 2), &hi)) {
                  prim_emit_result(pn, es, make_const_i64_type(std::clamp(x, lo, hi)));
                } else {
                  prim_emit_result(pn, es, make_abstract_type(sym_int64));
                }
              },
          .emit_c =
              [](FILE *fp, PNode *pn, Fun * /*f*/) {
                if (pn->lvals.n > 0 && pn->lvals[0]->live) {
                  std::fprintf(fp, "%s = (%s < %s ? %s : (%s > %s ? %s : %s));\n",
                               pn->lvals[0]->cg_string, pn->rvals[2]->cg_string,
                               pn->rvals[3]->cg_string, pn->rvals[3]->cg_string,
                               pn->rvals[2]->cg_string, pn->rvals[4]->cg_string,
                               pn->rvals[4]->cg_string, pn->rvals[2]->cg_string);
                }
              },
          .emit_llvm =
              [](PNode *pn, Fun * /*f*/) {
                llvm::Value *x = llvm_emit_get_var(pn->rvals[2]);
                llvm::Value *lo = llvm_emit_get_var(pn->rvals[3]);
                llvm::Value *hi = llvm_emit_get_var(pn->rvals[4]);
                llvm::Value *lt_lo = Builder->CreateICmpSLT(x, lo, "clamp.lt");
                llvm::Value *clamped_lo = Builder->CreateSelect(lt_lo, lo, x, "clamp.lo");
                llvm::Value *gt_hi = Builder->CreateICmpSGT(clamped_lo, hi, "clamp.gt");
                llvm::Value *res = Builder->CreateSelect(gt_hi, hi, clamped_lo, "clamp.res");
                if (pn->lvals.n > 0) {
                  llvm_emit_put_result(pn->lvals[0], res);
                }
              },
          .is_functional = true,
          .is_visible = true,
      });

  prims_.register_prim(
      "runtime_val",
      PrimitiveHandler{
          .transfer =
              [](PNode *pn, EntrySet *es) {
                prim_emit_result(pn, es, make_abstract_type(sym_int64));
              },
          .emit_llvm =
              [](PNode *pn, Fun * /*f*/) {
                llvm::Value *v = llvm_emit_get_var(pn->rvals[2]);
                if (pn->lvals.n > 0) llvm_emit_put_result(pn->lvals[0], v);
              },
          .is_functional = true,
          .is_visible = true,
      });

  prims_.register_prim(
      "c_call",
      PrimitiveHandler{
          .transfer =
              [](PNode *pn, EntrySet *es) {
                prim_emit_result(pn, es, make_abstract_type(sym_int64));
              },
          .emit_llvm =
              [](PNode *pn, Fun * /*f*/) {
                if (pn->rvals.n < 3 || !pn->rvals[2] || !pn->rvals[2]->sym ||
                    !pn->rvals[2]->sym->constant) {
                  return;
                }
                cchar *fn_name = pn->rvals[2]->sym->constant;
                std::vector<llvm::Type *> param_tys;
                std::vector<llvm::Value *> args;
                for (int i = 3; i < pn->rvals.n; ++i) {
                  llvm::Value *v = llvm_emit_get_var(pn->rvals[i]);
                  if (!v) return;
                  args.push_back(v);
                  param_tys.push_back(v->getType());
                }
                llvm::Type *i64_ty = llvm::Type::getInt64Ty(*TheContext);
                llvm::FunctionType *ft = llvm::FunctionType::get(i64_ty, param_tys, false);
                llvm::FunctionCallee fn = TheModule->getOrInsertFunction(fn_name, ft);
                llvm::Value *res = Builder->CreateCall(ft, fn.getCallee(), args, "ccall");
                if (pn->lvals.n > 0) llvm_emit_put_result(pn->lvals[0], res);
              },
          .is_functional = false,
          .is_visible = true,
      });

  auto make_i64_placeholder = []() {
    return PrimitiveHandler{
        .transfer =
            [](PNode *pn, EntrySet *es) {
              prim_emit_result(pn, es, make_abstract_type(sym_int64));
            },
        .is_functional = true,
        .is_visible = true,
    };
  };
  prims_.register_prim(
      "pow_opt",
      PrimitiveHandler{
          .transfer =
              [](PNode *pn, EntrySet *es) {
                prim_emit_result(pn, es, make_abstract_type(sym_int64));
              },
          .emit_llvm =
              [](PNode *pn, Fun * /*f*/) {
                llvm::Value *b = llvm_emit_get_var(pn->rvals[2]);
                llvm::Value *e = llvm_emit_get_var(pn->rvals[3]);
                if (!b || !e) return;
                llvm::Type *i64_ty = llvm::Type::getInt64Ty(*TheContext);
                llvm::FunctionType *ft = llvm::FunctionType::get(i64_ty, {i64_ty, i64_ty}, false);
                llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_pow_i64", ft);
                llvm::Value *res = Builder->CreateCall(ft, fn.getCallee(), {b, e}, "pow.fallback");
                if (pn->lvals.n > 0) llvm_emit_put_result(pn->lvals[0], res);
              },
          .is_functional = true,
          .is_visible = true,
      });
  prims_.register_prim("if_const", make_i64_placeholder());
  prims_.register_prim("sum_fields", make_i64_placeholder());
  register_meta_abi_prims(prims_);

  // Builtin Stage-0 comptime macro (LLJIT compile-time evaluation).
  stage0_.register_call_macro("comptime", [this](SyntaxObj *call, MacroContext &ctx) -> SyntaxObj * {
    auto args = MacroExpander::collect_call_args(call->children[2]);
    if (args.size() != 1) return nullptr;

    ScopeAllocator ct_scopes;
    SyntaxObj *ct_expanded = stage0_.expand(args[0]->clone(), ctx.grammar(), ct_scopes);
    if (!ct_expanded) return nullptr;

    int64_t val = 0;
    if (!compile_and_jit_i64(ct_expanded, &val, &prims_)) {
      std::fprintf(stderr, "langwb: comptime evaluation failed\n");
      return nullptr;
    }
    ++stats_->comptime_evals;
    if (val < 0) {
      return ctx.parse_template("expr", "(0 - " + std::to_string(-val) + ")");
    }
    return ctx.parse_template("expr", std::to_string(val));
  });

  // Stage-1 attribute-driven macros (controlled by IFA AnalysisAttrs).
  stage1_.register_call_macro("pow_opt", [this](SyntaxObj *call, MacroContext &ctx) -> SyntaxObj * {
    auto args = MacroExpander::collect_call_args(call->children[2]);
    if (args.size() != 2 || !ctx.attrs()) {
      std::fprintf(stderr, "langwb: stage1 macro 'pow_opt' expects 2 arguments at %d:%d\n",
                   call->line, call->col);
      return nullptr;
    }

    int64_t exp_val = 0;
    if (ctx.attrs()->is_const_i64(args[1], &exp_val) && exp_val >= 0 && exp_val <= 16) {
      ++stats_->stage1_unrolls;
      if (exp_val == 0) return ctx.parse_template("expr", "1");
      std::string body = "b";
      for (int64_t i = 1; i < exp_val; ++i) body += " * b";
      return ctx.parse_template("expr", "let b = __base__ in " + body, {{"__base__", args[0]}});
    }
    // Decline rewrite: leave `pow_opt(base, exp)` in place for the generic runtime fallback.
    return call;
  });

  stage1_.register_call_macro("if_const", [](SyntaxObj *call, MacroContext &ctx) -> SyntaxObj * {
    auto args = MacroExpander::collect_call_args(call->children[2]);
    if (args.size() != 3 || !ctx.attrs()) {
      std::fprintf(stderr, "langwb: stage1 macro 'if_const' expects 3 arguments at %d:%d\n",
                   call->line, call->col);
      return nullptr;
    }
    return ctx.attrs()->is_const_i64(args[0]) ? args[1]->clone() : args[2]->clone();
  });

  stage1_.register_call_macro(
      "sum_fields", [this](SyntaxObj *call, MacroContext &ctx) -> SyntaxObj * {
        auto args = MacroExpander::collect_call_args(call->children[2]);
        if (args.size() != 1 || !ctx.attrs()) {
          std::fprintf(stderr, "langwb: stage1 macro 'sum_fields' expects 1 argument at %d:%d\n",
                       call->line, call->col);
          return nullptr;
        }
        const auto *attr = ctx.attrs()->get(args[0]);
        if (!attr || attr->field_names.empty()) {
          std::fprintf(stderr,
                       "langwb: stage1 macro 'sum_fields' declined at %d:%d: argument has no known "
                       "record fields\n",
                       call->line, call->col);
          return nullptr;
        }

        ++stats_->stage1_reflections;
        std::string sum;
        for (size_t i = 0; i < attr->field_names.size(); ++i) {
          if (i > 0) sum += " + ";
          sum += "r." + attr->field_names[i];
        }
        return ctx.parse_template("expr", "let r = __rec__ in " + sum, {{"__rec__", args[0]}});
      });
}

std::string Workbench::wrap_for_syntax(std::string_view body) const {
  if (for_syntax_defs_.empty()) return std::string(body);
  std::string out;
  for (const auto &[name, expr] : for_syntax_defs_) {
    out += "let " + name + " = " + expr + " in\n";
  }
  out.append(body);
  return out;
}

void Workbench::add_for_syntax(std::string name, std::string expr_src) {
  for_syntax_defs_.emplace_back(std::move(name), std::move(expr_src));
}

bool Workbench::register_syntax_rule(
    bool is_form, std::string name,
    std::vector<std::pair<std::string, std::string>> pattern_slots, std::string template_src,
    bool trailing_variadic) {
  auto matches_class = [](const syntax::SyntaxObj *node, const std::string &sclass) -> bool {
    if (!node) return false;
    if (sclass.empty()) return true;
    if (sclass == "ident") {
      const syntax::SyntaxObj *t =
          (!node->is_terminal && node->children.size() == 1) ? node->children[0] : node;
      return t->is_terminal && !t->text.empty() &&
             (std::isalpha(static_cast<unsigned char>(t->text[0])) || t->text[0] == '_');
    }
    if (sclass == "int") {
      const syntax::SyntaxObj *t =
          (!node->is_terminal && node->children.size() == 1) ? node->children[0] : node;
      return t->is_terminal && !t->text.empty() &&
             std::isdigit(static_cast<unsigned char>(t->text[0]));
    }
    return node->symbol == sclass;
  };

  auto fn = [is_form, slots = std::move(pattern_slots), raw_tmpl = std::move(template_src),
             trailing_variadic, matches_class](syntax::SyntaxObj *node,
                                               macro::MacroContext &ctx) -> syntax::SyntaxObj * {
    std::vector<syntax::SyntaxObj *> items;
    if (is_form) {
      items.assign(node->children.begin(), node->children.end());
    } else if (node->core.kind == parse::CoreFormKind::App) {
      if (node->core.args >= 0 && static_cast<size_t>(node->core.args) < node->children.size()) {
        items = macro::MacroExpander::collect_call_args(
            node->children[static_cast<size_t>(node->core.args)]);
      }
    } else {
      if (node->children.size() == 4) {
        items = macro::MacroExpander::collect_call_args(node->children[2]);
      } else if (node->children.size() != 3) {
        return nullptr;
      }
    }

    size_t fixed_count = trailing_variadic && !slots.empty() ? slots.size() - 1 : slots.size();
    if (trailing_variadic) {
      if (items.size() < fixed_count) {
        std::fprintf(stderr, "langwb: syntax_rule arity mismatch (got %zu, expected >= %zu)\n",
                     items.size(), fixed_count);
        return nullptr;
      }
    } else if (items.size() != slots.size()) {
      std::fprintf(stderr, "langwb: syntax_rule arity mismatch (got %zu, expected %zu)\n",
                   items.size(), slots.size());
      return nullptr;
    }

    // Choose a placeholder prefix guaranteed absent from `raw_tmpl` so generated
    // splice keys can never collide with user identifiers in the template.
    std::string pfx = "__wb_h_";
    int pfx_id = 0;
    while (raw_tmpl.find(pfx) != std::string::npos) {
      pfx = "__wb_h" + std::to_string(++pfx_id) + "_";
    }

    std::string expanded_tmpl = raw_tmpl;
    std::unordered_map<std::string, syntax::SyntaxObj *> splices;

    // 1. Expand trailing variadic slot `$var <sep> ...` or `$var...` if present.
    if (trailing_variadic && !slots.empty()) {
      const auto &[vname, sclass] = slots.back();
      size_t var_count = items.size() - fixed_count;
      for (size_t k = 0; k < var_count; ++k) {
        syntax::SyntaxObj *it = items[fixed_count + k];
        if (!matches_class(it, sclass)) {
          std::fprintf(stderr, "langwb: syntax_rule class mismatch for $%s:%s...\n", vname.c_str(),
                       sclass.c_str());
          return nullptr;
        }
        splices[pfx + vname + "_" + std::to_string(k) + "__"] = it;
      }

      std::string needle = "$" + vname;
      size_t pos = 0;
      while ((pos = expanded_tmpl.find(needle, pos)) != std::string::npos) {
        size_t end = pos + needle.size();
        if (end < expanded_tmpl.size() &&
            (std::isalnum(static_cast<unsigned char>(expanded_tmpl[end])) ||
             expanded_tmpl[end] == '_')) {
          pos = end;
          continue;
        }
        // Look ahead for optional separator + `...` (e.g. `$xs + ...` or `$xs...`).
        size_t dots = expanded_tmpl.find("...", end);
        if (dots == std::string::npos) {
          pos = end;
          continue;
        }
        std::string_view between =
            trim_ws(std::string_view(expanded_tmpl).substr(end, dots - end));
        std::string sep = between.empty() ? ", " : (" " + std::string(between) + " ");
        std::string joined;
        if (var_count == 0) {
          if (between == "+") joined = "0";
          else if (between == "*") joined = "1";
        } else {
          for (size_t k = 0; k < var_count; ++k) {
            if (k > 0) joined += sep;
            joined += pfx + vname + "_" + std::to_string(k) + "__";
          }
        }
        expanded_tmpl.replace(pos, (dots + 3) - pos, joined);
        pos += joined.size();
      }
    }

    // 2. Bind fixed slots `$var` -> `<pfx>var__`.
    for (size_t i = 0; i < fixed_count; ++i) {
      const auto &[vname, sclass] = slots[i];
      if (vname.empty()) continue;  // `_` wildcard
      if (!matches_class(items[i], sclass)) {
        std::fprintf(stderr, "langwb: syntax_rule class mismatch for $%s:%s (got %s '%s')\n",
                     vname.c_str(), sclass.c_str(), items[i]->symbol.c_str(),
                     items[i]->text.c_str());
        return nullptr;
      }
      std::string repl = pfx + vname + "__";
      splices[repl] = items[i];
      std::string needle = "$" + vname;
      size_t pos = 0;
      while ((pos = expanded_tmpl.find(needle, pos)) != std::string::npos) {
        size_t end = pos + needle.size();
        if (end < expanded_tmpl.size() &&
            (std::isalnum(static_cast<unsigned char>(expanded_tmpl[end])) ||
             expanded_tmpl[end] == '_')) {
          pos = end;
          continue;
        }
        expanded_tmpl.replace(pos, needle.size(), repl);
        pos += repl.size();
      }
    }
    return ctx.parse_template("expr", expanded_tmpl, splices);
  };

  if (is_form) {
    stage0_.register_form_macro(std::move(name), std::move(fn));
  } else {
    stage0_.register_call_macro(std::move(name), std::move(fn));
  }
  return true;
}

void Workbench::register_guest_macro_impl(macro::MacroExpander &target, bool is_form,
                                          bool is_stage1, std::string name,
                                          std::string guest_src) {
  if (is_stage1 && !is_form && !prims_.has(name)) {
    prims_.register_prim(name, ifa_bridge::PrimitiveHandler{
                                   .transfer =
                                       [](PNode *pn, EntrySet *es) {
                                         ifa_bridge::prim_emit_result(
                                             pn, es, make_abstract_type(sym_int64));
                                       },
                                   .is_functional = true,
                                   .is_visible = true,
                               });
  }
  std::string wrapped_src = wrap_for_syntax(guest_src);
  // Lazily compile on first invocation (and cache the persistent LLJIT module)
  // so macros may reference other macros declared later in the same module.
  auto cached_mod = std::make_shared<std::shared_ptr<LLVMJitModule>>();
  auto fn = [this, src = std::move(wrapped_src), cached_mod = std::move(cached_mod)](
                syntax::SyntaxObj *call, macro::MacroContext &ctx) -> syntax::SyntaxObj * {
    if (!*cached_mod) {
      parse::CstNode *cst = ctx.grammar().parse(src);
      if (!cst) return nullptr;
      syntax::ScopeAllocator m_scopes;
      syntax::SyntaxObj *mstx = stage0_.expand(
          syntax::SyntaxObj::from_cst(cst), ctx.grammar(), m_scopes, nullptr, 64,
          /*run_hooks=*/false, /*run_module_begin=*/false, ctx.active_grammar_def());
      if (!mstx) return nullptr;
      *cached_mod = ifa_bridge::compile_persistent_i64(mstx, &prims_);
      if (!*cached_mod) return nullptr;
    }
    parse::GrammarDef *active_gdef =
        ctx.active_grammar_def() ? ctx.active_grammar_def() : &grammar_def_;
    ifa_bridge::MetaSession session(call, &ctx, active_gdef);
    ifa_bridge::ScopedMetaSession guard(&session);
    int64_t handle = 0;
    if (!ifa_bridge::run_persistent_i64(*cached_mod, &handle)) return nullptr;
    return session.lookup(handle);
  };
  if (is_form) {
    target.register_form_macro(std::move(name), std::move(fn));
  } else {
    target.register_call_macro(std::move(name), std::move(fn));
  }
}

void Workbench::register_guest_stage0_macro(std::string name, std::string guest_src) {
  register_guest_macro_impl(stage0_, false, false, std::move(name), std::move(guest_src));
}

void Workbench::register_guest_stage1_macro(std::string name, std::string guest_src) {
  register_guest_macro_impl(stage1_, false, true, std::move(name), std::move(guest_src));
}

void Workbench::register_guest_stage0_form_macro(std::string symbol_name, std::string guest_src) {
  register_guest_macro_impl(stage0_, true, false, std::move(symbol_name), std::move(guest_src));
}

void Workbench::register_guest_stage1_form_macro(std::string symbol_name, std::string guest_src) {
  register_guest_macro_impl(stage1_, true, true, std::move(symbol_name), std::move(guest_src));
}

bool Workbench::register_guest_hook(std::string_view hook_kind, std::string guest_src) {
  std::string wrapped_src = wrap_for_syntax(guest_src);
  auto cached_mod = std::make_shared<std::shared_ptr<LLVMJitModule>>();
  auto fn = [this, src = std::move(wrapped_src), cached_mod = std::move(cached_mod)](
                syntax::SyntaxObj *node, macro::MacroContext &ctx) -> syntax::SyntaxObj * {
    if (!*cached_mod) {
      parse::CstNode *cst = ctx.grammar().parse(src);
      if (!cst) return nullptr;
      syntax::ScopeAllocator m_scopes;
      syntax::SyntaxObj *mstx = stage0_.expand(
          syntax::SyntaxObj::from_cst(cst), ctx.grammar(), m_scopes, nullptr, 64,
          /*run_hooks=*/false, /*run_module_begin=*/false, ctx.active_grammar_def());
      if (!mstx) return nullptr;
      *cached_mod = ifa_bridge::compile_persistent_i64(mstx, &prims_);
      if (!*cached_mod) return nullptr;
    }
    parse::GrammarDef *active_gdef =
        ctx.active_grammar_def() ? ctx.active_grammar_def() : &grammar_def_;
    ifa_bridge::MetaSession session(node, &ctx, active_gdef);
    ifa_bridge::ScopedMetaSession guard(&session);
    int64_t handle = 0;
    if (!ifa_bridge::run_persistent_i64(*cached_mod, &handle)) return nullptr;
    return session.lookup(handle);
  };

  if (hook_kind == "module_begin") {
    stage0_.set_module_begin_hook(std::move(fn));
    return true;
  }
  if (hook_kind == "datum") {
    stage0_.set_datum_hook(std::move(fn));
    return true;
  }
  if (hook_kind == "app") {
    stage0_.set_app_hook(std::move(fn));
    return true;
  }
  if (hook_kind == "field") {
    stage0_.set_field_hook(std::move(fn));
    return true;
  }
  if (hook_kind == "record") {
    stage0_.set_record_hook(std::move(fn));
    return true;
  }
  return false;
}

bool Workbench::register_guest_prim(std::string name, std::string c_sym, std::string transfer_src) {
  std::shared_ptr<LLVMJitModule> jit_tfn;
  if (!transfer_src.empty()) {
    auto gram = compiled_grammar();
    if (!gram) return false;
    std::string wrapped_src = wrap_for_syntax(transfer_src);
    parse::CstNode *cst = gram->parse(wrapped_src);
    if (!cst) return false;
    syntax::ScopeAllocator t_scopes;
    syntax::SyntaxObj *tstx = stage0_.expand(syntax::SyntaxObj::from_cst(cst), *gram, t_scopes,
                                             nullptr, 64, /*run_hooks=*/false);
    if (!tstx) return false;
    jit_tfn = ifa_bridge::compile_persistent_i64(tstx, &prims_);
    if (!jit_tfn) return false;
  }

  ifa_bridge::PrimitiveHandler handler;
  handler.is_functional = true;
  handler.is_visible = true;

  if (jit_tfn) {
    handler.transfer = [jit_tfn](PNode *pn, EntrySet *es) {
      ifa_bridge::TransferSession session{pn, es};
      ifa_bridge::ScopedTransferSession guard(&session);
      ifa_bridge::run_persistent_i64(jit_tfn);
    };
  } else {
    handler.transfer = [](PNode *pn, EntrySet *es) {
      ifa_bridge::prim_emit_result(pn, es, make_abstract_type(sym_int64));
    };
  }

  handler.emit_llvm = [sym_name = std::move(c_sym)](PNode *pn, Fun * /*f*/) {
    std::vector<llvm::Type *> param_tys;
    std::vector<llvm::Value *> args;
    for (int i = 2; i < pn->rvals.n; ++i) {
      llvm::Value *v = llvm_emit_get_var(pn->rvals[i]);
      if (!v) return;
      args.push_back(v);
      param_tys.push_back(v->getType());
    }
    llvm::Type *i64_ty = llvm::Type::getInt64Ty(*TheContext);
    llvm::FunctionType *ft = llvm::FunctionType::get(i64_ty, param_tys, false);
    llvm::FunctionCallee fn = TheModule->getOrInsertFunction(sym_name, ft);
    llvm::Value *res = Builder->CreateCall(ft, fn.getCallee(), args, "fprim");
    if (pn->lvals.n > 0) llvm_emit_put_result(pn->lvals[0], res);
  };

  prims_.register_prim(std::move(name), std::move(handler));
  return true;
}

std::shared_ptr<parse::DynamicGrammar> Workbench::compiled_grammar() {
  return cache_->compile_or_get(grammar_def_);
}

namespace {

void advance_line_col(std::string_view chunk, int &line, int &col) {
  for (char c : chunk) {
    if (c == '\n') {
      ++line;
      col = 0;
    } else {
      ++col;
    }
  }
}

std::string_view skip_ws_tracking(std::string_view sv, int &line, int &col) {
  size_t i = 0;
  while (i < sv.size() && std::isspace(static_cast<unsigned char>(sv[i]))) ++i;
  advance_line_col(sv.substr(0, i), line, col);
  return sv.substr(i);
}

void offset_syntax_loc(syntax::SyntaxObj *node, int line_delta, int first_line_col_delta) {
  if (!node) return;
  if (node->line == 1) node->col += first_line_col_delta;
  node->line += line_delta;
  for (syntax::SyntaxObj *c : node->children) {
    offset_syntax_loc(c, line_delta, first_line_col_delta);
  }
}

}  // namespace

bool Workbench::eval_i64(std::string_view src, int64_t *out_result, const EvalOptions &opts,
                         ifa_bridge::CompileInfo *out_info) {
  std::string lang_name;
  std::string_view after_lang = src;
  bool has_lang = extract_lang_header(after_lang, &lang_name);

  int base_line = 1;
  int base_col = 0;
  if (has_lang) {
    advance_line_col(src.substr(0, static_cast<size_t>(after_lang.data() - src.data())), base_line,
                     base_col);
  }

  std::string cleaned = strip_comments(after_lang);
  std::string_view rest = skip_ws_tracking(cleaned, base_line, base_col);
  while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back()))) {
    rest.remove_suffix(1);
  }

  // If the input has a `#lang` header, declarations, or multiple `;`-separated
  // top-level forms, parse and apply form-by-form inside a hermetic per-evaluation
  // Workbench clone (sharing the GrammarCache).
  bool has_top_semi = (find_decl_terminator(rest) != std::string_view::npos);
  if (has_lang || starts_with_decl(rest) || has_top_semi) {
    Workbench session(*this, /*share_cache=*/true);
    if (has_lang && !session.load_lang(lang_name)) {
      return false;
    }
    auto decl_gram = session.compiled_decl_grammar();
    if (!decl_gram) return false;

    syntax::ScopeAllocator scopes;
    syntax::ScopeId mod_scope = scopes.fresh();
    std::vector<syntax::SyntaxObj *> top_stmts;

    int cur_line = base_line;
    int cur_col = base_col;

    while (!rest.empty()) {
      if (starts_with_decl(rest)) {
        size_t consumed = 0;
        parse::CstNode *dcst = decl_gram->parse_prefix(rest, &consumed, "decl", /*quiet=*/true);
        if (!dcst || consumed == 0) {
          std::fprintf(stderr, "langwb: %d:%d: invalid declaration near '%.*s'\n", cur_line,
                       cur_col, static_cast<int>(std::min<size_t>(rest.size(), 60)), rest.data());
          return false;
        }
        if (!session.apply_decl_cst(dcst)) {
          std::fprintf(stderr, "langwb: %d:%d: failed to apply declaration '%.*s'\n", cur_line,
                       cur_col, static_cast<int>(consumed), rest.data());
          return false;
        }
        advance_line_col(rest.substr(0, consumed), cur_line, cur_col);
        rest = skip_ws_tracking(rest.substr(consumed), cur_line, cur_col);
        continue;
      }

      // Fast path: if no prior top-level statements exist and either there is no `;`
      // or the entire remainder parses as a single expression under the current grammar,
      // evaluate directly.
      auto gram = session.compiled_grammar();
      if (!gram) return false;
      if (top_stmts.empty()) {
        if (find_decl_terminator(rest) == std::string_view::npos || gram->parse_check(rest)) {
          return session.eval_single_expr_i64(rest, out_result, opts, out_info);
        }
      }

      // Grammar-driven prefix parse: parse one complete `stmt` (or `expr`) prefix
      // using the active grammar rather than splitting on `;` beforehand.
      size_t consumed = 0;
      parse::CstNode *cst = gram->parse_prefix(rest, &consumed, "stmt", /*quiet=*/true);
      if (!cst || consumed == 0) {
        cst = gram->parse_prefix(rest, &consumed, "expr", /*quiet=*/true);
      }
      if (!cst || consumed == 0) {
        // Re-run non-quiet full parse to emit a clear syntax error diagnostic.
        (void)gram->parse(rest);
        return false;
      }

      int form_line = cur_line;
      int form_col = cur_col;
      advance_line_col(rest.substr(0, consumed), cur_line, cur_col);
      rest = skip_ws_tracking(rest.substr(consumed), cur_line, cur_col);
      if (!rest.empty() && rest.front() == ';') {
        advance_line_col(rest.substr(0, 1), cur_line, cur_col);
        rest = skip_ws_tracking(rest.substr(1), cur_line, cur_col);
      } else if (!rest.empty()) {
        std::fprintf(stderr, "langwb: %d:%d: expected ';' between top-level forms\n", cur_line,
                     cur_col);
        return false;
      }

      syntax::SyntaxObj *stx = syntax::SyntaxObj::from_cst(cst);
      offset_syntax_loc(stx, form_line - 1, form_col);
      stx = stx->add_scope(mod_scope);
      // Run datum/app/field/record hooks per form, but defer module_begin to the assembled Seq.
      syntax::SyntaxObj *stx0 = session.stage0_.expand(
          stx, *gram, scopes, nullptr, 64, /*run_hooks=*/true, /*run_module_begin=*/false,
          &session.grammar_def_);
      if (!stx0) return false;
      top_stmts.push_back(stx0);
    }

    if (top_stmts.empty()) {
      std::fprintf(stderr, "langwb: expected expression after declarations\n");
      return false;
    }

    auto *seq = new syntax::SyntaxObj();
    seq->node_id = syntax::SyntaxObj::next_node_id();
    seq->symbol = "module";
    seq->core = parse::CoreRuleMeta{.kind = parse::CoreFormKind::Seq, .inner = -1};
    for (syntax::SyntaxObj *s : top_stmts) seq->children.push_back(s);

    auto gram = session.compiled_grammar();
    if (!gram) return false;
    // Apply module_begin hook once to the assembled module Seq (so prelude lets wrap
    // the entire Seq rather than wrapping individual `def` statements).
    syntax::SyntaxObj *mod_stx =
        session.stage0_.apply_module_begin(seq, *gram, scopes, nullptr, &session.grammar_def_);
    if (!mod_stx) return false;
    if (mod_stx != seq) {
      mod_stx = session.stage0_.expand(mod_stx, *gram, scopes, nullptr, 64, /*run_hooks=*/false,
                                       /*run_module_begin=*/false, &session.grammar_def_);
      if (!mod_stx) return false;
    }

    syntax::SyntaxObj *final_stx = mod_stx;
    if (opts.staged) {
      ifa_bridge::AnalysisAttrs attrs;
      if (!ifa_bridge::analyze_syntax(mod_stx, &session.prims_, &attrs)) return false;
      if (opts.dump_attrs) {
        std::fprintf(opts.diag_fp, "=== IFA AnalysisAttrs ===\n");
        dump_attrs_tree(mod_stx, attrs, opts.diag_fp);
      }
      final_stx = session.expand_stage1_contour_aware(mod_stx, *gram, scopes, attrs);
      if (!final_stx) return false;
    }
    if (opts.dump_expanded) {
      std::fprintf(opts.diag_fp, "=== Expanded SyntaxObj ===\n");
      syntax::dump_syntax(final_stx, opts.diag_fp);
    }
    return ifa_bridge::compile_and_jit_i64(final_stx, out_result, &session.prims_, out_info);
  }

  return eval_single_expr_i64(rest, out_result, opts, out_info);
}

namespace {

// Canonical alpha-equivalent signature of an expanded SyntaxObj (normalizing
// ScopeIds by order of first appearance) for decision-keyed contour grouping:
// contours producing identical syntax AND identical binding relationships share one clone.
void append_syntax_sig(const syntax::SyntaxObj *n,
                       std::unordered_map<syntax::ScopeId, uint32_t> &scope_norm,
                       std::string &out) {
  if (!n) {
    out += "null;";
    return;
  }
  out += n->symbol;
  out.push_back(':');
  if (n->is_terminal) {
    out += n->text;
    out.push_back('{');
    const auto &raw = n->scopes.raw();
    for (size_t i = 0; i < raw.size(); ++i) {
      if (i > 0) out.push_back(',');
      auto [it, ins] = scope_norm.emplace(raw[i], static_cast<uint32_t>(scope_norm.size()));
      out += std::to_string(it->second);
    }
    out.push_back('}');
  } else {
    out.push_back('(');
    for (const syntax::SyntaxObj *c : n->children) {
      append_syntax_sig(c, scope_norm, out);
      out.push_back(',');
    }
    out.push_back(')');
  }
}

std::string syntax_sig(const syntax::SyntaxObj *n) {
  std::string out;
  std::unordered_map<syntax::ScopeId, uint32_t> scope_norm;
  append_syntax_sig(n, scope_norm, out);
  return out;
}

syntax::SyntaxObj *unwrap_lambda_stx(syntax::SyntaxObj *n) {
  while (n) {
    if (n->core.kind == parse::CoreFormKind::Lambda) return n;
    if (n->core.kind == parse::CoreFormKind::Paren && n->core.inner >= 0 &&
        static_cast<size_t>(n->core.inner) < n->children.size()) {
      n = n->children[static_cast<size_t>(n->core.inner)];
      continue;
    }
    if (n->core.kind == parse::CoreFormKind::PassThrough && n->children.size() == 1) {
      n = n->children[0];
      continue;
    }
    break;
  }
  return nullptr;
}

struct CloneTarget {
  std::string name;
  syntax::ScopeSet scopes;  // fresh hygienic ScopeSet containing ONLY the clone's ScopeId
};

// Rewrites App nodes whose node_id is in `redirects` to call the specialized clone
// identifier using the clone's fresh ScopeSet (cannot be captured by user bindings).
syntax::SyntaxObj *rewrite_redirected_calls(
    syntax::SyntaxObj *node, const std::unordered_map<uint64_t, CloneTarget> &redirects) {
  if (!node || redirects.empty()) return node;
  if (node->core.kind == parse::CoreFormKind::App && node->core.fn >= 0 &&
      static_cast<size_t>(node->core.fn) < node->children.size()) {
    if (auto it = redirects.find(node->node_id); it != redirects.end()) {
      syntax::SyntaxObj *out = node->clone();
      size_t fn_idx = static_cast<size_t>(out->core.fn);
      syntax::SyntaxObj *fn_node = out->children[fn_idx]->with_scopes(it->second.scopes);
      fn_node->text = it->second.name;
      syntax::SyntaxObj *leaf = fn_node;
      while (leaf && !leaf->is_terminal && leaf->children.size() == 1) {
        leaf = leaf->children[0];
        leaf->text = it->second.name;
      }
      out->children[fn_idx] = fn_node;
      for (size_t i = 0; i < out->children.size(); ++i) {
        if (i == fn_idx) continue;
        out->children[i] = rewrite_redirected_calls(out->children[i], redirects);
      }
      return out;
    }
  }
  syntax::SyntaxObj *out = node->clone();
  for (size_t i = 0; i < out->children.size(); ++i) {
    out->children[i] = rewrite_redirected_calls(out->children[i], redirects);
  }
  return out;
}

// Checks whether `node` still contains any identifier referencing `name` whose
// scopes are a superset of `binder_scopes`.
bool has_residual_ref(const syntax::SyntaxObj *node, const std::string &name,
                      const syntax::ScopeSet &binder_scopes) {
  if (!node) return false;
  if (node->is_terminal) {
    return node->text == name && binder_scopes.is_subset_of(node->scopes);
  }
  for (const syntax::SyntaxObj *c : node->children) {
    if (has_residual_ref(c, name, binder_scopes)) return true;
  }
  return false;
}

// Builds a synthetic `let <clone_name> = <rhs> in <body>` node using `clone_scopes`
// on the clone binder (so it cannot collide with or be captured by user bindings).
syntax::SyntaxObj *make_clone_let(const syntax::SyntaxObj *let_tmpl, const std::string &clone_name,
                                  const syntax::ScopeSet &clone_scopes, syntax::SyntaxObj *rhs,
                                  syntax::SyntaxObj *body) {
  syntax::SyntaxObj *out = let_tmpl->clone();
  out->node_id = syntax::SyntaxObj::next_node_id();
  size_t bi = static_cast<size_t>(out->core.bind);
  size_t ri = static_cast<size_t>(out->core.rhs);
  size_t bodyi = static_cast<size_t>(out->core.body);
  out->children[bi] = out->children[bi]->with_scopes(clone_scopes);
  out->children[bi]->text = clone_name;
  syntax::SyntaxObj *leaf = out->children[bi];
  while (leaf && !leaf->is_terminal && leaf->children.size() == 1) {
    leaf = leaf->children[0];
    leaf->text = clone_name;
  }
  out->children[ri] = rhs;
  out->children[bodyi] = body;
  return out;
}

}  // namespace

syntax::SyntaxObj *Workbench::expand_stage1_contour_aware(syntax::SyntaxObj *node,
                                                          const parse::DynamicGrammar &gram,
                                                          syntax::ScopeAllocator &scopes,
                                                          const ifa_bridge::AnalysisAttrs &attrs) {
  if (!node) return nullptr;
  using parse::CoreFormKind;

  // Head-first Stage-1 expansion: expand any macro at `node`'s head BEFORE
  // recursing into children so discarded branches (e.g. `if_const(1, 42, sum_fields(0))`)
  // are never expanded.
  syntax::SyntaxObj *head =
      stage1_.expand_head(node, gram, scopes, &attrs, 0, 64, &grammar_def_);
  if (!head) return nullptr;
  if (head != node) {
    return expand_stage1_contour_aware(head, gram, scopes, attrs);
  }

  // Check for `let f = fn(params) => body in rest` where `f` has either:
  //   (A) multiple IFA contours (e.g. polymorphic record/type contours), OR
  //   (B) call sites passing distinct compile-time constants to a formal demanded
  //       by a Stage-1 macro.
  if (node->core.kind == CoreFormKind::Let && node->core.bind >= 0 && node->core.rhs >= 0 &&
      node->core.body >= 0) {
    size_t bi = static_cast<size_t>(node->core.bind);
    size_t ri = static_cast<size_t>(node->core.rhs);
    size_t bodyi = static_cast<size_t>(node->core.body);
    if (bi < node->children.size() && ri < node->children.size() && bodyi < node->children.size()) {
      syntax::SyntaxObj *lam = unwrap_lambda_stx(node->children[ri]);
      const ifa_bridge::FnContourInfo *finfo = lam ? attrs.get_fn_info(lam->node_id) : nullptr;
      if (lam && finfo && !finfo->call_sites.empty()) {
        std::string orig_name = node->children[bi]->text;
        const syntax::ScopeSet orig_binder_scopes = node->children[bi]->scopes;
        static constexpr size_t kMaxValueSplits = 8;

        // Case A: IFA already split `f` into K > 1 contours.
        if (finfo->contour_count > 1) {
          std::vector<std::string> group_sigs;
          std::vector<syntax::SyntaxObj *> group_lams;
          std::unordered_map<int, size_t> contour_to_group;
          bool all_ok = true;

          for (int ci = 0; ci < finfo->contour_count; ++ci) {
            ifa_bridge::AnalysisAttrs::ScopedView view(&attrs, ci);
            syntax::SyntaxObj *exp_lam =
                expand_stage1_contour_aware(lam->clone(), gram, scopes, attrs);
            if (!exp_lam) {
              all_ok = false;
              break;
            }
            std::string sig = syntax_sig(exp_lam);
            size_t gid = group_sigs.size();
            for (size_t g = 0; g < group_sigs.size(); ++g) {
              if (group_sigs[g] == sig) {
                gid = g;
                break;
              }
            }
            if (gid == group_sigs.size()) {
              group_sigs.push_back(std::move(sig));
              group_lams.push_back(exp_lam);
            }
            contour_to_group[ci] = gid;
          }

          if (all_ok && group_lams.size() > 1) {
            // Only redirect call sites that (1) target this function exclusively
            // (call_target_count == 1) and (2) agree on a single decision group
            // across all reachable contours.
            std::unordered_map<uint64_t, std::set<size_t>> call_to_groups;
            for (const auto &cs : finfo->call_sites) {
              if (attrs.call_target_count(cs.call_node_id) != 1) continue;
              if (auto it = contour_to_group.find(cs.contour_idx); it != contour_to_group.end()) {
                call_to_groups[cs.call_node_id].insert(it->second);
              }
            }

            std::vector<CloneTarget> clones;
            clones.reserve(group_lams.size());
            for (size_t g = 0; g < group_lams.size(); ++g) {
              syntax::ScopeSet cs;
              cs.add(scopes.fresh());
              clones.push_back(
                  CloneTarget{.name = "__c_" + orig_name + "_" + std::to_string(g), .scopes = cs});
            }

            std::unordered_map<uint64_t, CloneTarget> redirects;
            for (const auto &[cid, groups] : call_to_groups) {
              if (groups.size() == 1) {
                redirects[cid] = clones[*groups.begin()];
              }
            }

            if (!redirects.empty()) {
              syntax::SyntaxObj *body_rewritten =
                  rewrite_redirected_calls(node->children[bodyi], redirects);
              bool need_default = has_residual_ref(body_rewritten, orig_name, orig_binder_scopes);
              syntax::SyntaxObj *default_lam = nullptr;
              if (need_default) {
                // Keep a valid default binding for residual value/multi-contour uses.
                default_lam = expand_stage1_contour_aware(lam->clone(), gram, scopes, attrs);
              }
              if (!need_default || default_lam) {
                syntax::SyntaxObj *exp_body =
                    expand_stage1_contour_aware(body_rewritten, gram, scopes, attrs);
                if (!exp_body) return nullptr;

                syntax::SyntaxObj *cur =
                    need_default
                        ? make_clone_let(node, orig_name, orig_binder_scopes, default_lam, exp_body)
                        : exp_body;
                for (size_t g = group_lams.size(); g-- > 0;) {
                  cur = make_clone_let(node, clones[g].name, clones[g].scopes, group_lams[g], cur);
                }
                stats_->contour_clones += static_cast<int>(group_lams.size());
                return cur;
              }
            }
          }
        }

        // Case B: Demand-driven constant splitting (whenever Case A did not already
        // produce distinct clones and we have >= 2 call sites).
        if (finfo->call_sites.size() >= 2 && !finfo->param_node_ids.empty()) {
          attrs.begin_demand_recording();
          syntax::SyntaxObj *default_lam =
              expand_stage1_contour_aware(lam->clone(), gram, scopes, attrs);
          auto demands = attrs.end_demand_recording();

          std::vector<size_t> demanded_params;
          for (size_t pi = 0; pi < finfo->param_node_ids.size(); ++pi) {
            uint64_t pid = finfo->param_node_ids[pi];
            for (uint64_t did : demands) {
              if (did == pid || attrs.binder_of(did) == pid) {
                demanded_params.push_back(pi);
                break;
              }
            }
          }

          if (default_lam && !demanded_params.empty()) {
            std::string default_sig = syntax_sig(default_lam);
            std::vector<std::string> group_sigs;
            std::vector<syntax::SyntaxObj *> group_lams;
            std::vector<CloneTarget> clones;
            std::unordered_map<uint64_t, CloneTarget> redirects;

            for (const auto &cs : finfo->call_sites) {
              // Only specialize call sites proven to target `f` exclusively.
              if (attrs.call_target_count(cs.call_node_id) != 1) continue;

              std::unordered_map<uint64_t, ifa_bridge::NodeAttr> overrides;
              bool all_const = true;
              for (size_t pi : demanded_params) {
                if (pi >= cs.arg_node_ids.size()) {
                  all_const = false;
                  break;
                }
                const ifa_bridge::NodeAttr *arg_attr = attrs.get_raw(cs.arg_node_ids[pi]);
                if (!arg_attr || !arg_attr->has_const_i64) {
                  all_const = false;
                  break;
                }
                ifa_bridge::NodeAttr ov;
                ov.analyzed = true;
                ov.is_monomorphic = true;
                ov.has_const_i64 = true;
                ov.const_i64 = arg_attr->const_i64;
                ov.type_names.push_back("int64");
                ov.contour_count = 1;
                overrides[finfo->param_node_ids[pi]] = std::move(ov);
              }
              if (!all_const) continue;

              ifa_bridge::AnalysisAttrs::ScopedView view(&attrs, 0, std::move(overrides));
              syntax::SyntaxObj *spec_lam =
                  expand_stage1_contour_aware(lam->clone(), gram, scopes, attrs);
              if (!spec_lam) continue;
              std::string sig = syntax_sig(spec_lam);
              if (sig == default_sig) continue;

              size_t gid = group_sigs.size();
              for (size_t g = 0; g < group_sigs.size(); ++g) {
                if (group_sigs[g] == sig) {
                  gid = g;
                  break;
                }
              }
              if (gid == group_sigs.size()) {
                if (group_sigs.size() >= kMaxValueSplits) continue;
                group_sigs.push_back(std::move(sig));
                group_lams.push_back(spec_lam);
                syntax::ScopeSet cscope;
                cscope.add(scopes.fresh());
                clones.push_back(CloneTarget{
                    .name = "__c_" + orig_name + "_" + std::to_string(gid), .scopes = cscope});
              }
              redirects[cs.call_node_id] = clones[gid];
            }

            if (!group_lams.empty()) {
              syntax::SyntaxObj *body_rewritten =
                  rewrite_redirected_calls(node->children[bodyi], redirects);
              syntax::SyntaxObj *exp_body =
                  expand_stage1_contour_aware(body_rewritten, gram, scopes, attrs);
              if (!exp_body) return nullptr;

              syntax::SyntaxObj *cur =
                  make_clone_let(node, orig_name, orig_binder_scopes, default_lam, exp_body);
              for (size_t g = group_lams.size(); g-- > 0;) {
                cur = make_clone_let(node, clones[g].name, clones[g].scopes, group_lams[g], cur);
              }
              stats_->contour_clones += static_cast<int>(group_lams.size());
              return cur;
            }
          }
        }
      }
    }
  }

  // Recurse into children (head was already expanded above).
  syntax::SyntaxObj *out = node->clone();
  for (size_t i = 0; i < out->children.size(); ++i) {
    if (out->children[i] && !out->children[i]->is_terminal) {
      out->children[i] = expand_stage1_contour_aware(out->children[i], gram, scopes, attrs);
      if (!out->children[i]) return nullptr;
    }
  }
  return out;
}

bool Workbench::eval_single_expr_i64(std::string_view rest, int64_t *out_result,
                                     const EvalOptions &opts, ifa_bridge::CompileInfo *out_info) {
  auto gram = compiled_grammar();
  if (!gram) return false;

  parse::CstNode *cst = gram->parse(rest);
  if (!cst) return false;
  if (opts.dump_cst) {
    std::fprintf(opts.diag_fp, "=== CST ===\n");
    parse::dump_cst(cst, opts.diag_fp);
  }

  syntax::ScopeAllocator scopes;
  syntax::SyntaxObj *stx = syntax::SyntaxObj::from_cst(cst);
  syntax::SyntaxObj *stx0 = stage0_.expand(stx, *gram, scopes, nullptr, 64, /*run_hooks=*/true,
                                           /*run_module_begin=*/true, &grammar_def_);
  if (!stx0) return false;

  syntax::SyntaxObj *final_stx = stx0;
  if (opts.staged) {
    ifa_bridge::AnalysisAttrs attrs;
    if (!ifa_bridge::analyze_syntax(stx0, &prims_, &attrs)) return false;
    if (opts.dump_attrs) {
      std::fprintf(opts.diag_fp, "=== IFA AnalysisAttrs ===\n");
      dump_attrs_tree(stx0, attrs, opts.diag_fp);
    }
    final_stx = expand_stage1_contour_aware(stx0, *gram, scopes, attrs);
    if (!final_stx) return false;
  }

  if (opts.dump_expanded) {
    std::fprintf(opts.diag_fp, "=== Expanded SyntaxObj ===\n");
    syntax::dump_syntax(final_stx, opts.diag_fp);
  }

  return ifa_bridge::compile_and_jit_i64(final_stx, out_result, &prims_, out_info);
}

}  // namespace langwb
