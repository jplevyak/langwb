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

struct WbCons : public gc {
  int64_t head;
  WbCons *tail;
  WbCons(int64_t h, WbCons *t) : head(h), tail(t) {}
};

extern "C" int64_t _CG_wb_nil() { return 0; }

extern "C" int64_t _CG_wb_cons(int64_t h, int64_t t) {
  auto *cell = new WbCons(h, reinterpret_cast<WbCons *>(static_cast<uintptr_t>(t)));
  return static_cast<int64_t>(reinterpret_cast<uintptr_t>(cell));
}

extern "C" int64_t _CG_wb_head(int64_t xs) {
  auto *cell = reinterpret_cast<WbCons *>(static_cast<uintptr_t>(xs));
  return cell ? cell->head : 0;
}

extern "C" int64_t _CG_wb_tail(int64_t xs) {
  auto *cell = reinterpret_cast<WbCons *>(static_cast<uintptr_t>(xs));
  return cell ? static_cast<int64_t>(reinterpret_cast<uintptr_t>(cell->tail)) : 0;
}

extern "C" int64_t _CG_wb_is_nil(int64_t xs) { return xs == 0 ? 1 : 0; }

extern "C" int64_t _CG_wb_list_len(int64_t xs) {
  int64_t n = 0;
  for (auto *c = reinterpret_cast<WbCons *>(static_cast<uintptr_t>(xs)); c; c = c->tail) ++n;
  return n;
}

extern "C" void _CG_wb_emit_raw(const char *s);
extern "C" void _CG_wb_emit_i64(int64_t value);

extern "C" void _CG_wb_emit_list(int64_t xs) {
  _CG_wb_emit_raw("[");
  bool first = true;
  for (auto *c = reinterpret_cast<WbCons *>(static_cast<uintptr_t>(xs)); c; c = c->tail) {
    if (!first) _CG_wb_emit_raw(", ");
    first = false;
    _CG_wb_emit_i64(c->head);
  }
  _CG_wb_emit_raw("]");
}

extern "C" int64_t _CG_wb_match_fail() {
  std::fprintf(stderr, "langwb: non-exhaustive match\n");
  std::fflush(stderr);
  std::exit(1);
}

namespace langwb {

namespace {

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
  init_using_syntax_hook();
}

Workbench::Workbench(BareTag)
    : cache_(std::make_shared<parse::GrammarCache>()),
      stats_(std::make_shared<EvalStats>()),
      lang_search_paths_{".", "langs", "examples/langs"} {
  init_decl_grammar();
  init_builtin_prims();
  init_using_syntax_hook();
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
      for_syntax_defs_(parent.for_syntax_defs_),
      syntax_exts_(parent.syntax_exts_),
      static_checks_(parent.static_checks_) {
  init_using_syntax_hook();
}

void Workbench::init_using_syntax_hook() {
  stage0_.set_using_syntax_handler(
      [this](std::string_view ext_name, std::string_view body_src,
             const syntax::ScopeSet &enclosing_scopes, syntax::ScopeAllocator &scopes,
             const ifa_bridge::AnalysisAttrs *attrs, int depth, int max_depth,
             const syntax::BindingEnv<uint64_t> &lexical_env) -> syntax::SyntaxObj * {
        if (depth > max_depth) {
          std::fprintf(stderr, "langwb: using syntax nesting depth exceeded\n");
          return nullptr;
        }
        Workbench child(*this, /*share_cache=*/true);
        auto it = syntax_exts_.find(std::string(ext_name));
        if (it != syntax_exts_.end()) {
          std::string cleaned = strip_comments(it->second);
          std::string_view rest = trim_ws(cleaned);
          while (!rest.empty()) {
            size_t consumed = 0;
            if (!child.parse_and_apply_decl_prefix(rest, &consumed) || consumed == 0) {
              std::fprintf(stderr,
                           "langwb: invalid declaration in syntax_ext '%.*s' near '%.*s'\n",
                           static_cast<int>(ext_name.size()), ext_name.data(),
                           static_cast<int>(std::min<size_t>(rest.size(), 60)), rest.data());
              return nullptr;
            }
            rest = trim_ws(rest.substr(consumed));
          }
        } else if (!child.load_lang(ext_name)) {
          std::fprintf(stderr, "langwb: unknown syntax extension '%.*s'\n",
                       static_cast<int>(ext_name.size()), ext_name.data());
          return nullptr;
        }
        auto ext_gram = child.compiled_grammar();
        if (!ext_gram) return nullptr;
        parse::CstNode *body_cst = ext_gram->parse(trim_ws(body_src), "expr");
        if (!body_cst) {
          std::fprintf(stderr, "langwb: failed to parse body of 'using syntax %.*s'\n",
                       static_cast<int>(ext_name.size()), ext_name.data());
          return nullptr;
        }
        syntax::SyntaxObj *stx = syntax::SyntaxObj::from_cst(body_cst);
        if (!enclosing_scopes.empty()) {
          stx = stx->with_scopes(enclosing_scopes);
        }
        return child.stage0_.expand_node(stx, *ext_gram, scopes, attrs, depth, max_depth,
                                         &child.grammar_def_, lexical_env);
      });
}

void Workbench::init_decl_grammar() {
  decl_grammar_.add_rule("decl", "decl_grammar");
  decl_grammar_.add_rule("decl", "decl_prim_simple");
  decl_grammar_.add_rule("decl", "decl_prim_transfer_head");
  decl_grammar_.add_rule("decl", "decl_hook_head");
  decl_grammar_.add_rule("decl", "decl_for_syntax_head");
  decl_grammar_.add_rule("decl", "decl_syntax_rule");
  decl_grammar_.add_rule("decl", "decl_syntax_ext");
  decl_grammar_.add_rule("decl", "decl_check_head");
  decl_grammar_.add_rule("decl", "decl_macro_head");

  decl_grammar_.add_rule("decl_grammar", "'grammar' ident '+=' str str? core_ann? ';'");
  decl_grammar_.add_rule("core_ann", "'core' '(' core_body ')'");
  decl_grammar_.add_rule("core_body", "\"[^)]+\"");

  decl_grammar_.add_rule("decl_prim_simple", "'prim' ident 'c_call' str ';'");
  decl_grammar_.add_rule("decl_prim_transfer_head", "'prim' ident 'c_call' str 'transfer' '=>'");
  decl_grammar_.add_rule("decl_hook_head", "'hook' ident '=>'");
  decl_grammar_.add_rule("decl_for_syntax_head", "'for_syntax' ident '=>'");
  decl_grammar_.add_rule("decl_syntax_rule",
                         "'syntax_rule' 'form'? ident '(' slot_list? ')' '=>' str ';'");
  decl_grammar_.add_rule("decl_syntax_ext", "'syntax_ext' ident '{' nested_text? '}' ';'");
  decl_grammar_.add_rule("decl_check_head", "'check' ident '=>'");
  decl_grammar_.add_rule("slot_list", "\"[^)]+\"");
  decl_grammar_.add_rule("decl_macro_head", "macro_kw ident '=>'");
  decl_grammar_.add_rule("macro_kw", "'macro'");
  decl_grammar_.add_rule("macro_kw", "'form_macro'");
  decl_grammar_.add_rule("macro_kw", "'stage1_macro'");
  decl_grammar_.add_rule("macro_kw", "'stage1_form_macro'");

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
         match_keyword(src, "syntax_ext") || match_keyword(src, "check") ||
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

  std::string cleaned = strip_comments(sv);
  std::string_view rest = trim_ws(cleaned);
  while (!rest.empty()) {
    size_t consumed = 0;
    if (!parse_and_apply_decl_prefix(rest, &consumed) || consumed == 0) {
      std::fprintf(stderr, "langwb: invalid declaration in '%s' near '%.*s'\n", found_path.c_str(),
                   static_cast<int>(std::min<size_t>(rest.size(), 60)), rest.data());
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

bool Workbench::parse_and_apply_decl_prefix(std::string_view src, size_t *out_consumed) {
  auto decl_gram = compiled_decl_grammar();
  if (!decl_gram) return false;

  size_t head_consumed = 0;
  parse::CstNode *cst = decl_gram->parse_prefix(src, &head_consumed, "decl", /*quiet=*/true);
  if (!cst || head_consumed == 0) return false;

  const parse::CstNode *d =
      (cst->symbol == "decl" && cst->children.size() == 1) ? cst->children[0] : cst;
  if (!d) return false;

  bool needs_body = d->symbol.ends_with("_head");
  if (!needs_body) {
    if (!apply_decl_cst(cst, nullptr)) return false;
    if (out_consumed) *out_consumed = head_consumed;
    return true;
  }

  // Parse the declaration body eagerly at declaration time using the active guest grammar.
  size_t pos = head_consumed;
  while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) ++pos;
  std::string_view body_sv = src.substr(pos);

  auto guest_gram = compiled_grammar();
  if (!guest_gram) return false;
  size_t body_consumed = 0;
  parse::CstNode *body_cst =
      guest_gram->parse_prefix(body_sv, &body_consumed, "expr", /*quiet=*/false);
  if (!body_cst || body_consumed == 0) {
    return false;
  }
  pos += body_consumed;
  while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) ++pos;
  if (pos >= src.size() || src[pos] != ';') {
    // Re-parse the full body slice up to the next `;` so DParser reports the
    // actual syntax error location (e.g. `*` in `1 + * 3;`) instead of only `;`.
    size_t semi = find_decl_terminator(body_sv);
    if (semi != std::string_view::npos && semi > body_consumed) {
      (void)guest_gram->parse(body_sv.substr(0, semi), "expr", /*quiet=*/false);
    } else {
      std::fprintf(stderr, "langwb: expected ';' after declaration body near '%.*s'\n",
                   static_cast<int>(std::min<size_t>(src.size() - pos, 40)), src.data() + pos);
    }
    return false;
  }
  ++pos;  // consume ';'

  syntax::SyntaxObj *body_stx = syntax::SyntaxObj::from_cst(body_cst);
  if (!apply_decl_cst(cst, body_stx)) return false;
  if (out_consumed) *out_consumed = pos;
  return true;
}

bool Workbench::apply_decl_cst(const parse::CstNode *decl_cst, syntax::SyntaxObj *body_stx) {
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

  // 2. prim <name> c_call "<c_sym>" [transfer => <expr>];
  if (d->symbol == "decl_prim_simple" || d->symbol == "decl_prim_transfer_head") {
    const parse::CstNode *id = find_cst_sym(d, "ident");
    const parse::CstNode *s = find_cst_sym(d, "str");
    if (!id || !s) return false;
    std::string c_sym;
    if (!unquote_cst_str(s->text, c_sym)) return false;
    if (!register_guest_prim_stx(id->text, std::move(c_sym), body_stx)) return false;
    ++stats_->decls_applied;
    return true;
  }

  // 3. hook <module_begin|datum|app|field|record> => <expr>;
  if (d->symbol == "decl_hook_head") {
    const parse::CstNode *id = find_cst_sym(d, "ident");
    if (!id || !body_stx) return false;
    if (!register_guest_hook_stx(id->text, body_stx)) return false;
    ++stats_->decls_applied;
    return true;
  }

  // 4. for_syntax <name> => <expr>;
  if (d->symbol == "decl_for_syntax_head") {
    const parse::CstNode *id = find_cst_sym(d, "ident");
    if (!id || !body_stx) return false;
    for_syntax_defs_.emplace_back(id->text, body_stx);
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

  // 5b. syntax_ext <name> { <decls...> };
  if (d->symbol == "decl_syntax_ext") {
    const parse::CstNode *id = find_cst_sym(d, "ident");
    if (!id) return false;
    const parse::CstNode *nt = find_cst_sym(d, "nested_text");
    syntax_exts_[id->text] = nt ? nt->text : std::string{};
    ++stats_->decls_applied;
    return true;
  }

  // 5c. check <target> => <expr>;
  if (d->symbol == "decl_check_head") {
    const parse::CstNode *id = find_cst_sym(d, "ident");
    if (!id || !body_stx) return false;
    if (!register_static_check_stx(id->text, body_stx)) return false;
    ++stats_->decls_applied;
    return true;
  }

  // 6. [stage1_][form_]macro <name> => <expr>;
  if (d->symbol == "decl_macro_head") {
    const parse::CstNode *kw = find_cst_sym(d, "macro_kw");
    const parse::CstNode *id = find_cst_sym(d, "ident");
    if (!kw || !id || !body_stx) return false;
    bool ok = false;
    if (kw->text == "macro") {
      ok = register_guest_macro_stx(stage0_, false, false, id->text, body_stx);
    } else if (kw->text == "form_macro") {
      ok = register_guest_macro_stx(stage0_, true, false, id->text, body_stx);
    } else if (kw->text == "stage1_macro") {
      ok = register_guest_macro_stx(stage1_, false, true, id->text, body_stx);
    } else if (kw->text == "stage1_form_macro") {
      ok = register_guest_macro_stx(stage1_, true, true, id->text, body_stx);
    }
    if (!ok) return false;
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
  size_t consumed = 0;
  if (!parse_and_apply_decl_prefix(with_semi, &consumed)) return false;
  return trim_ws(std::string_view(with_semi).substr(consumed)).empty();
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
  grammar_def_.add_rule("expr", "'using' 'syntax' \"[a-zA-Z_][a-zA-Z0-9_]*\" 'in' raw_block",
                        "$right 0",
                        CoreRuleMeta{.kind = CoreFormKind::UsingSyntax, .bind = 2, .body = 4});
  grammar_def_.add_rule("raw_block", "'{' raw_text? '}'");
  grammar_def_.add_rule("raw_text", "raw_atom+");
  grammar_def_.add_rule("raw_atom", "'{' raw_text? '}'");
  grammar_def_.add_rule("raw_atom", "'(' raw_text? ')'");
  grammar_def_.add_rule("raw_atom", R"d("\"([^\"\\]|\\[^])*\"")d");
  grammar_def_.add_rule("raw_atom", "\"[^(){}\\\"]+\"");
  grammar_def_.add_rule("expr", "expr '==' expr", "$left 1", kBinOp);
  grammar_def_.add_rule("expr", "expr '!=' expr", "$left 1", kBinOp);
  grammar_def_.add_rule("expr", "expr '<' expr", "$left 2", kBinOp);
  grammar_def_.add_rule("expr", "expr '<=' expr", "$left 2", kBinOp);
  grammar_def_.add_rule("expr", "expr '>' expr", "$left 2", kBinOp);
  grammar_def_.add_rule("expr", "expr '>=' expr", "$left 2", kBinOp);
  grammar_def_.add_rule("expr", "expr '+' expr", "$left 3", kBinOp);
  grammar_def_.add_rule("expr", "expr '-' expr", "$left 3", kBinOp);
  grammar_def_.add_rule("expr", "expr '*' expr", "$left 4", kBinOp);
  grammar_def_.add_rule("expr", "expr '/' expr", "$left 4", kBinOp);
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
  grammar_def_.add_rule("expr", R"d("\"([^\"\\]|\\[^])*\"")d", {},
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

  // Composable syntax extensions: `unless`, anaphoric `aif`, lists, and `match`.
  parse::GrammarDef ext;
  ext.add_rule("expr", "unless_expr", {},
               CoreRuleMeta{.kind = CoreFormKind::PassThrough, .inner = 0});
  ext.add_rule("expr", "aif_expr", {}, CoreRuleMeta{.kind = CoreFormKind::PassThrough, .inner = 0});
  ext.add_rule("expr", "list_lit", {}, CoreRuleMeta{.kind = CoreFormKind::PassThrough, .inner = 0});
  ext.add_rule("expr", "match_expr", {},
               CoreRuleMeta{.kind = CoreFormKind::PassThrough, .inner = 0});
  ext.add_rule("expr", "quasiquote_expr", {},
               CoreRuleMeta{.kind = CoreFormKind::PassThrough, .inner = 0});
  ext.add_rule("unless_expr", "'unless' expr 'then' expr 'else' expr", "$right 0");
  ext.add_rule("aif_expr", "'aif' expr 'then' expr 'else' expr", "$right 0");
  ext.add_rule("list_lit", "'[' list_elems? ']'");
  ext.add_rule("list_elems", "expr (',' expr)*");
  ext.add_rule("match_expr", "'match' expr '{' match_arms '}'");
  ext.add_rule("match_arms", "match_arm (',' match_arm)*");
  ext.add_rule("match_arm", "match_pat '=>' expr", "$right 0");
  ext.add_rule("match_pat", "\"[0-9]+\"");
  ext.add_rule("match_pat", "'[' ']'");
  ext.add_rule("match_pat", "'[' \"[a-zA-Z_][a-zA-Z0-9_]*\" '|' \"[a-zA-Z_][a-zA-Z0-9_]*\" ']'");
  ext.add_rule("match_pat", "'{' pat_fields '}'");
  ext.add_rule("match_pat", "\"[a-zA-Z_][a-zA-Z0-9_]*\"");
  ext.add_rule("pat_fields", "pat_field (',' pat_field)*");
  ext.add_rule("pat_field", "\"[a-zA-Z_][a-zA-Z0-9_]*\" (':' \"[a-zA-Z_][a-zA-Z0-9_]*\")?");
  ext.add_rule("quasiquote_expr", "'`' '[' \"[a-zA-Z_][a-zA-Z0-9_]*\" '|' qq_body ']'");
  ext.add_rule("qq_body", "qq_atom+");
  ext.add_rule("qq_atom", "'[' qq_body? ']'");
  ext.add_rule("qq_atom", "'(' qq_body? ')'");
  ext.add_rule("qq_atom", "'{' qq_body? '}'");
  ext.add_rule("qq_atom", R"d("\"([^\"\\]|\\[^])*\"")d");
  ext.add_rule("qq_atom", R"d("[^\[\](){}\"]+")d");
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

  stage0_.register_form_macro("list_lit", [](SyntaxObj *form, MacroContext &ctx) -> SyntaxObj * {
    std::vector<SyntaxObj *> elems;
    if (form->children.size() >= 3) {
      elems = MacroExpander::collect_call_args(form->children[1]);
    }
    SyntaxObj *acc = ctx.parse_template("expr", "nil()");
    for (size_t i = elems.size(); i-- > 0;) {
      acc = ctx.parse_template("expr", "cons(__h__, __t__)",
                               {{"__h__", elems[i]}, {"__t__", acc}});
    }
    return acc;
  });

  stage0_.register_form_macro("match_expr", [](SyntaxObj *form, MacroContext &ctx) -> SyntaxObj * {
    if (form->children.size() < 5) return nullptr;
    SyntaxObj *scrut = form->children[1];
    std::vector<SyntaxObj *> arms;
    std::function<void(SyntaxObj *)> gather_arms = [&](SyntaxObj *n) {
      if (!n) return;
      if (n->symbol == "match_arm") {
        arms.push_back(n);
        return;
      }
      for (SyntaxObj *c : n->children) gather_arms(c);
    };
    gather_arms(form->children[3]);
    if (arms.empty()) return nullptr;

    SyntaxObj *cur = ctx.parse_template("expr", "wb_match_fail()");
    for (size_t i = arms.size(); i-- > 0;) {
      SyntaxObj *arm = arms[i];
      if (arm->children.size() < 3) return nullptr;
      SyntaxObj *pat = arm->children[0];
      SyntaxObj *body = arm->children[2];

      if (pat->children.size() == 1 && pat->children[0]->is_terminal) {
        const std::string &tok = pat->children[0]->text;
        if (!tok.empty() && tok[0] >= '0' && tok[0] <= '9') {
          cur = ctx.parse_template("expr", "if __m__ == __k__ then __b__ else __n__",
                                   {{"__k__", pat->children[0]},
                                    {"__b__", body},
                                    {"__n__", cur}});
        } else if (tok == "_") {
          cur = body;
        } else {
          cur = ctx.parse_template("expr", "let __v__ = __m__ in __b__",
                                   {{"__v__", pat->children[0]}, {"__b__", body}});
        }
      } else if (pat->children.size() == 2 && pat->children[0]->text == "[" &&
                 pat->children[1]->text == "]") {
        cur = ctx.parse_template("expr", "if is_nil(__m__) != 0 then __b__ else __n__",
                                 {{"__b__", body}, {"__n__", cur}});
      } else if (pat->children.size() == 5 && pat->children[0]->text == "[" &&
                 pat->children[2]->text == "|") {
        SyntaxObj *h_id = pat->children[1];
        SyntaxObj *t_id = pat->children[3];
        cur = ctx.parse_template(
            "expr",
            "if is_nil(__m__) == 0 then let __h__ = head(__m__) in let __t__ = tail(__m__) in __b__ "
            "else __n__",
            {{"__h__", h_id}, {"__t__", t_id}, {"__b__", body}, {"__n__", cur}});
      } else if (pat->children.size() == 3 && pat->children[0]->text == "{") {
        std::vector<SyntaxObj *> pfields;
        std::function<void(SyntaxObj *)> gather_pf = [&](SyntaxObj *n) {
          if (!n) return;
          if (n->symbol == "pat_field") {
            pfields.push_back(n);
            return;
          }
          for (SyntaxObj *c : n->children) gather_pf(c);
        };
        gather_pf(pat->children[1]);
        SyntaxObj *inner = body;
        for (size_t fi = pfields.size(); fi-- > 0;) {
          SyntaxObj *pf = pfields[fi];
          std::vector<SyntaxObj *> ids;
          std::function<void(SyntaxObj *)> gather_ids = [&](SyntaxObj *n) {
            if (!n) return;
            if (n->is_terminal && n->text != ":") {
              ids.push_back(n);
              return;
            }
            for (SyntaxObj *c : n->children) gather_ids(c);
          };
          gather_ids(pf);
          if (ids.empty()) continue;
          SyntaxObj *fname = ids[0];
          SyntaxObj *bname = (ids.size() >= 2) ? ids[1] : ids[0];
          inner = ctx.parse_template("expr", "let __v__ = __m__.__f__ in __b__",
                                     {{"__v__", bname}, {"__f__", fname}, {"__b__", inner}});
        }
        cur = inner;
      } else {
        return nullptr;
      }
    }
    return ctx.parse_template("expr", "let __m__ = __s__ in __body__",
                              {{"__s__", scrut}, {"__body__", cur}});
  });

  stage0_.register_form_macro("quasiquote_expr", [](SyntaxObj *form, MacroContext &ctx) -> SyntaxObj * {
    if (form->children.size() < 6) return nullptr;
    const std::string &nt = form->children[2]->text;
    std::string_view raw = trim_ws(form->children[4]->text);

    static uint64_t qq_counter = 0;
    uint64_t batch_id = ++qq_counter;

    std::string tmpl;
    tmpl.reserve(raw.size());
    std::vector<std::pair<std::string, SyntaxObj *>> splices;

    size_t i = 0;
    while (i < raw.size()) {
      if (raw[i] == '"') {
        tmpl.push_back(raw[i++]);
        while (i < raw.size()) {
          tmpl.push_back(raw[i]);
          if (raw[i] == '\\' && i + 1 < raw.size()) {
            tmpl.push_back(raw[++i]);
          } else if (raw[i] == '"') {
            ++i;
            break;
          }
          ++i;
        }
        continue;
      }
      if (raw[i] == ',' && i + 1 < raw.size() && raw[i + 1] == '(') {
        size_t expr_start = i + 2;
        int depth = 1;
        size_t j = expr_start;
        while (j < raw.size() && depth > 0) {
          if (raw[j] == '"') {
            ++j;
            while (j < raw.size()) {
              if (raw[j] == '\\' && j + 1 < raw.size()) {
                j += 2;
              } else if (raw[j] == '"') {
                ++j;
                break;
              } else {
                ++j;
              }
            }
            continue;
          }
          if (raw[j] == '(') ++depth;
          else if (raw[j] == ')') --depth;
          ++j;
        }
        if (depth != 0) {
          std::fprintf(stderr, "langwb: %d:%d: unclosed ',(' unquote in quasiquote\n", form->line,
                       form->col);
          return nullptr;
        }
        std::string_view splice_src = trim_ws(raw.substr(expr_start, (j - 1) - expr_start));
        parse::CstNode *scst = ctx.grammar().parse(splice_src, "expr");
        if (!scst) {
          std::fprintf(stderr, "langwb: %d:%d: invalid unquote expression '%.*s' in quasiquote\n",
                       form->line, form->col, static_cast<int>(splice_src.size()),
                       splice_src.data());
          return nullptr;
        }
        SyntaxObj *sstx = SyntaxObj::from_cst(scst)->with_scopes(form->scopes);
        std::string ph =
            "__wb_qq_" + std::to_string(batch_id) + "_" + std::to_string(splices.size()) + "__";
        tmpl += ph;
        splices.emplace_back(std::move(ph), sstx);
        i = j;
        continue;
      }
      tmpl.push_back(raw[i++]);
    }

    // Compile-time grammar check against the active DynamicGrammar at non-terminal `nt`.
    if (!ctx.grammar().parse(tmpl, nt, /*quiet=*/true)) {
      std::fprintf(stderr,
                   "langwb: %d:%d: quasiquote template does not parse as '%s': %s\n", form->line,
                   form->col, nt.c_str(), tmpl.c_str());
      return nullptr;
    }

    auto escape_str = [](std::string_view s) {
      std::string out;
      out.reserve(s.size() + 2);
      out.push_back('"');
      for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
      }
      out.push_back('"');
      return out;
    };

    std::string parse_call = "stx_parse(" + escape_str(nt) + ", " + escape_str(tmpl) + ")";
    SyntaxObj *acc = ctx.parse_template("expr", parse_call);
    if (!acc) return nullptr;
    for (const auto &[ph, sstx] : splices) {
      std::string sp_call = "stx_splice(__acc__, " + escape_str(ph) + ", __val__)";
      acc = ctx.parse_template("expr", sp_call, {{"__acc__", acc}, {"__val__", sstx}});
      if (!acc) return nullptr;
    }
    return acc;
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

  auto reg_rt_typed = [&](std::string name, std::string c_sym, bool is_pure, bool returns_list) {
    prims_.register_prim(
        std::move(name),
        PrimitiveHandler{
            .transfer =
                [returns_list](PNode *pn, EntrySet *es) {
                  prim_emit_result(
                      pn, es,
                      make_abstract_type(returns_list ? wb_list_type_sym() : sym_int64));
                },
            .emit_llvm =
                [c_sym = std::move(c_sym)](PNode *pn, Fun * /*f*/) {
                  llvm::Type *i64_ty = llvm::Type::getInt64Ty(*TheContext);
                  std::vector<llvm::Type *> ptys;
                  std::vector<llvm::Value *> args;
                  for (int i = 2; i < pn->rvals.n; ++i) {
                    llvm::Value *v = llvm_emit_get_var(pn->rvals[i]);
                    if (!v) return;
                    args.push_back(v);
                    ptys.push_back(v->getType());
                  }
                  llvm::FunctionType *ft = llvm::FunctionType::get(i64_ty, ptys, false);
                  llvm::FunctionCallee fn = TheModule->getOrInsertFunction(c_sym, ft);
                  llvm::Value *res = Builder->CreateCall(ft, fn.getCallee(), args, "rtcall");
                  if (pn->lvals.n > 0) llvm_emit_put_result(pn->lvals[0], res);
                },
            .is_functional = is_pure,
            .is_visible = true,
        });
  };
  reg_rt_typed("nil", "_CG_wb_nil", true, true);
  reg_rt_typed("cons", "_CG_wb_cons", false, true);
  reg_rt_typed("head", "_CG_wb_head", true, false);
  reg_rt_typed("tail", "_CG_wb_tail", true, true);
  reg_rt_typed("is_nil", "_CG_wb_is_nil", true, false);
  reg_rt_typed("list_len", "_CG_wb_list_len", true, false);
  reg_rt_typed("wb_match_fail", "_CG_wb_match_fail", false, false);

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

syntax::SyntaxObj *Workbench::wrap_for_syntax_stx(syntax::SyntaxObj *body_stx,
                                                  const parse::DynamicGrammar &gram) const {
  if (!body_stx || for_syntax_defs_.empty()) return body_stx;
  syntax::SyntaxObj *cur = body_stx;
  for (size_t i = for_syntax_defs_.size(); i-- > 0;) {
    const auto &[name, rhs_stx] = for_syntax_defs_[i];
    parse::CstNode *lcst = gram.parse("let " + name + " = 0 in 0", "expr");
    if (!lcst) return nullptr;
    syntax::SyntaxObj *let_stx = syntax::SyntaxObj::from_cst(lcst);
    if (let_stx->core.kind != parse::CoreFormKind::Let || let_stx->core.rhs < 0 ||
        let_stx->core.body < 0) {
      return nullptr;
    }
    let_stx->children[static_cast<size_t>(let_stx->core.rhs)] = rhs_stx->clone();
    let_stx->children[static_cast<size_t>(let_stx->core.body)] = cur;
    cur = let_stx;
  }
  return cur;
}

void Workbench::add_for_syntax(std::string name, std::string expr_src) {
  auto gram = compiled_grammar();
  if (!gram) return;
  parse::CstNode *cst = gram->parse(expr_src, "expr");
  if (!cst) return;
  for_syntax_defs_.emplace_back(std::move(name), syntax::SyntaxObj::from_cst(cst));
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

bool Workbench::register_guest_macro_stx(macro::MacroExpander &target, bool is_form,
                                         bool is_stage1, std::string name,
                                         syntax::SyntaxObj *body_stx) {
  if (!body_stx) return false;
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
  auto decl_gram = compiled_grammar();
  syntax::SyntaxObj *wrapped_stx =
      decl_gram ? wrap_for_syntax_stx(body_stx, *decl_gram) : body_stx;
  if (!wrapped_stx) return false;
  // Body is already parsed at declaration time (locking syntax/precedence and
  // reporting syntax errors immediately). Expansion + LLJIT compilation stay lazy
  // so macros may call other macros declared later in the same module.
  auto cached_mod = std::make_shared<std::shared_ptr<LLVMJitModule>>();
  auto fn = [this, wrapped_stx, cached_mod = std::move(cached_mod)](
                syntax::SyntaxObj *call, macro::MacroContext &ctx) -> syntax::SyntaxObj * {
    if (!*cached_mod) {
      syntax::ScopeAllocator m_scopes;
      syntax::SyntaxObj *mstx = stage0_.expand(
          wrapped_stx->clone(), ctx.grammar(), m_scopes, nullptr, 64,
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
    if (session.had_error) return nullptr;
    return session.lookup(handle);
  };
  if (is_form) {
    target.register_form_macro(std::move(name), std::move(fn));
  } else {
    target.register_call_macro(std::move(name), std::move(fn));
  }
  return true;
}

void Workbench::register_guest_macro_impl(macro::MacroExpander &target, bool is_form,
                                          bool is_stage1, std::string name,
                                          std::string guest_src) {
  auto gram = compiled_grammar();
  if (!gram) return;
  parse::CstNode *cst = gram->parse(guest_src, "expr");
  if (!cst) return;
  register_guest_macro_stx(target, is_form, is_stage1, std::move(name),
                           syntax::SyntaxObj::from_cst(cst));
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

bool Workbench::register_static_check_stx(std::string target_name, syntax::SyntaxObj *body_stx) {
  if (!body_stx) return false;
  auto decl_gram = compiled_grammar();
  syntax::SyntaxObj *wrapped_stx =
      decl_gram ? wrap_for_syntax_stx(body_stx, *decl_gram) : body_stx;
  if (!wrapped_stx) return false;
  auto cached_mod = std::make_shared<std::shared_ptr<LLVMJitModule>>();
  StaticCheckFn fn = [this, wrapped_stx, cached_mod = std::move(cached_mod)](
                         syntax::SyntaxObj *node, macro::MacroContext &ctx) -> bool {
    if (!*cached_mod) {
      syntax::ScopeAllocator m_scopes;
      syntax::SyntaxObj *mstx = stage0_.expand(
          wrapped_stx->clone(), ctx.grammar(), m_scopes, nullptr, 64,
          /*run_hooks=*/false, /*run_module_begin=*/false, ctx.active_grammar_def());
      if (!mstx) return false;
      *cached_mod = ifa_bridge::compile_persistent_i64(mstx, &prims_);
      if (!*cached_mod) return false;
    }
    parse::GrammarDef *active_gdef =
        ctx.active_grammar_def() ? ctx.active_grammar_def() : &grammar_def_;
    ifa_bridge::MetaSession session(node, &ctx, active_gdef);
    ifa_bridge::ScopedMetaSession guard(&session);
    int64_t unused = 0;
    if (!ifa_bridge::run_persistent_i64(*cached_mod, &unused)) return false;
    return !session.had_error;
  };
  static_checks_[std::move(target_name)].push_back(std::move(fn));
  return true;
}

void Workbench::register_static_check(std::string target_name, std::string guest_src) {
  auto gram = compiled_grammar();
  if (!gram) return;
  parse::CstNode *cst = gram->parse(guest_src, "expr");
  if (!cst) return;
  register_static_check_stx(std::move(target_name), syntax::SyntaxObj::from_cst(cst));
}

bool Workbench::register_guest_hook_stx(std::string_view hook_kind, syntax::SyntaxObj *body_stx) {
  if (!body_stx) return false;
  auto decl_gram = compiled_grammar();
  syntax::SyntaxObj *wrapped_stx =
      decl_gram ? wrap_for_syntax_stx(body_stx, *decl_gram) : body_stx;
  if (!wrapped_stx) return false;
  auto cached_mod = std::make_shared<std::shared_ptr<LLVMJitModule>>();
  auto fn = [this, wrapped_stx, cached_mod = std::move(cached_mod)](
                syntax::SyntaxObj *node, macro::MacroContext &ctx) -> syntax::SyntaxObj * {
    if (!*cached_mod) {
      syntax::ScopeAllocator m_scopes;
      syntax::SyntaxObj *mstx = stage0_.expand(
          wrapped_stx->clone(), ctx.grammar(), m_scopes, nullptr, 64,
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
    if (session.had_error) return nullptr;
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

bool Workbench::register_guest_hook(std::string_view hook_kind, std::string guest_src) {
  auto gram = compiled_grammar();
  if (!gram) return false;
  parse::CstNode *cst = gram->parse(guest_src, "expr");
  if (!cst) return false;
  return register_guest_hook_stx(hook_kind, syntax::SyntaxObj::from_cst(cst));
}

bool Workbench::register_guest_prim_stx(std::string name, std::string c_sym,
                                        syntax::SyntaxObj *transfer_stx) {
  std::shared_ptr<LLVMJitModule> jit_tfn;
  if (transfer_stx) {
    auto gram = compiled_grammar();
    if (!gram) return false;
    syntax::SyntaxObj *wrapped = wrap_for_syntax_stx(transfer_stx, *gram);
    if (!wrapped) return false;
    syntax::ScopeAllocator t_scopes;
    syntax::SyntaxObj *tstx =
        stage0_.expand(wrapped->clone(), *gram, t_scopes, nullptr, 64, /*run_hooks=*/false);
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

bool Workbench::register_guest_prim(std::string name, std::string c_sym, std::string transfer_src) {
  syntax::SyntaxObj *tstx = nullptr;
  if (!transfer_src.empty()) {
    auto gram = compiled_grammar();
    if (!gram) return false;
    parse::CstNode *cst = gram->parse(transfer_src, "expr");
    if (!cst) return false;
    tstx = syntax::SyntaxObj::from_cst(cst);
  }
  return register_guest_prim_stx(std::move(name), std::move(c_sym), tstx);
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
  return eval_impl(src, out_result, nullptr, opts, out_info);
}

bool Workbench::eval_value(std::string_view src, ifa_bridge::EvalValue *out_value,
                           const EvalOptions &opts, ifa_bridge::CompileInfo *out_info) {
  return eval_impl(src, nullptr, out_value, opts, out_info);
}

bool Workbench::eval_impl(std::string_view src, int64_t *out_i64, ifa_bridge::EvalValue *out_value,
                          const EvalOptions &opts, ifa_bridge::CompileInfo *out_info) {
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
      // Grammar-driven declaration detection: only treat the next form as a
      // declaration when `decl_grammar_` matches a valid declaration head, so
      // keywords like `check`, `hook`, or `syntax_ext` remain usable as ordinary
      // identifiers in guest expressions (e.g. `def check = 2; check * 21`).
      size_t head_probe = 0;
      if (decl_gram->parse_prefix(rest, &head_probe, "decl", /*quiet=*/true) && head_probe > 0) {
        size_t consumed = 0;
        if (!session.parse_and_apply_decl_prefix(rest, &consumed) || consumed == 0) {
          std::fprintf(stderr, "langwb: %d:%d: invalid declaration near '%.*s'\n", cur_line,
                       cur_col, static_cast<int>(std::min<size_t>(rest.size(), 60)), rest.data());
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
          return session.eval_single_expr(rest, out_i64, out_value, opts, out_info);
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

    return session.run_staged_and_jit(mod_stx, *gram, scopes, out_i64, out_value, opts, out_info);
  }

  return eval_single_expr(rest, out_i64, out_value, opts, out_info);
}

bool Workbench::eval_single_expr(std::string_view rest, int64_t *out_i64,
                                 ifa_bridge::EvalValue *out_value, const EvalOptions &opts,
                                 ifa_bridge::CompileInfo *out_info) {
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

  return run_staged_and_jit(stx0, *gram, scopes, out_i64, out_value, opts, out_info);
}

}  // namespace langwb
