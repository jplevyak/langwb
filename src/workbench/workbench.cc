#include "workbench/workbench.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "ifa_bridge/meta_abi.h"
#include "llvm_internal.h"

// Runtime helpers (_CG_wb_pow_i64, _CG_wb_nil/cons/head/tail/is_nil/list_len,
// _CG_wb_emit_*, _CG_wb_match_fail) are provided by src/runtime/langwb_rt.c.

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

// Directory of the running `langwb` executable (empty if unknown).
std::filesystem::path executable_dir() {
  std::filesystem::path exe_dir;
#ifdef __APPLE__
  char exe_buf[4096];
  uint32_t exe_sz = sizeof(exe_buf);
  if (_NSGetExecutablePath(exe_buf, &exe_sz) == 0) {
    exe_dir = std::filesystem::weakly_canonical(exe_buf).parent_path();
  }
#elif defined(__linux__)
  std::error_code ec;
  auto exe_p = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec) exe_dir = exe_p.parent_path();
#endif
  return exe_dir;
}

// `#lang` / import search path: the working directory (and its `langs/`,
// `examples/langs/`), then each `LANGWB_LANG_PATH` entry (`:`-separated), then
// `langs/` and `examples/langs/` next to the executable, so a program run
// from any directory finds the languages that ship with `langwb`. Evaluations
// with a source file prepend that file's directory (`with_source_dir`).
std::vector<std::string> default_lang_search_paths() {
  std::vector<std::string> paths{".", "langs", "examples/langs"};
  if (const char *env = std::getenv("LANGWB_LANG_PATH")) {
    std::string_view rest(env);
    while (!rest.empty()) {
      size_t colon = rest.find(':');
      std::string_view entry = rest.substr(0, colon);
      if (!entry.empty()) paths.emplace_back(entry);
      if (colon == std::string_view::npos) break;
      rest.remove_prefix(colon + 1);
    }
  }
  std::filesystem::path exe_dir = executable_dir();
  if (!exe_dir.empty()) {
    paths.push_back((exe_dir / "langs").string());
    paths.push_back((exe_dir / "examples" / "langs").string());
  }
  return paths;
}

// `paths` with `dir` and `dir/langs` first (no duplicates of those two).
std::vector<std::string> with_source_dir(const std::vector<std::string> &paths,
                                         const std::string &dir) {
  if (dir.empty()) return paths;
  std::vector<std::string> out{dir, (std::filesystem::path(dir) / "langs").string()};
  for (const auto &p : paths) {
    if (p != out[0] && p != out[1]) out.push_back(p);
  }
  return out;
}

}  // namespace

namespace {
void advance_line_col(std::string_view chunk, int &line, int &col);
void offset_syntax_loc(syntax::SyntaxObj *node, int line_delta, int first_line_col_delta);
}  // namespace

Workbench::Workbench()
    : cache_(std::make_shared<parse::GrammarCache>()),
      stats_(std::make_shared<EvalStats>()),
      lang_search_paths_(default_lang_search_paths()) {
  init_decl_grammar();
  init_builtin_prims();
  init_default_minilang();
  init_using_syntax_hook();
}

Workbench::Workbench(BareTag)
    : cache_(std::make_shared<parse::GrammarCache>()),
      stats_(std::make_shared<EvalStats>()),
      lang_search_paths_(default_lang_search_paths()) {
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
      static_checks_(parent.static_checks_),
      decl_scope_(parent.decl_scope_),
      cur_file_(parent.cur_file_),
      lang_base_(parent.lang_base_ ? parent.lang_base_ : &parent),
      own_lang_base_(parent.own_lang_base_) {
  init_using_syntax_hook();
}

void Workbench::init_using_syntax_hook() {
  stage0_.set_using_syntax_handler(
      [this](std::string_view ext_name, std::string_view body_src,
             const syntax::ScopeSet &enclosing_scopes, syntax::ScopeAllocator &scopes,
             const ifa_bridge::AnalysisAttrs *attrs, int depth, int max_depth,
             const syntax::BindingEnv<uint64_t> &lexical_env,
             const syntax::SyntaxObj *body_node) -> syntax::SyntaxObj * {
        if (depth > max_depth) {
          diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: using syntax nesting depth exceeded\n");
          return nullptr;
        }
        Workbench child(*this, /*share_cache=*/true);
        auto it = syntax_exts_.find(std::string(ext_name));
        if (it != syntax_exts_.end()) {
          for (const auto &op : it->second.gram_ops) {
            if (op.clear) {
              child.grammar_def_.clear_production(op.prod);
            } else {
              child.grammar_def_.add_rule(op.prod, op.rhs, "", op.core);
            }
          }
          child.stage0_.merge_from(it->second.stage0_delta);
        } else if (!child.load_lang(ext_name)) {
          diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: unknown syntax extension '%.*s'\n",
                       static_cast<int>(ext_name.size()), ext_name.data());
          return nullptr;
        }
        auto ext_gram = child.compiled_grammar();
        if (!ext_gram) return nullptr;
        // The body is parsed on its own; locate it where it starts in the
        // source (after `{` and leading whitespace), for syntax errors and
        // for the parsed nodes.
        std::string_view body = trim_ws(body_src);
        int body_line = 1, body_col = 0;
        bool located = body_node && body_node->line > 0;
        if (located) {
          body_line = body_node->line;
          body_col = body_node->col + 1;
          std::string_view lead =
              body.empty() ? body_src
                           : body_src.substr(0, static_cast<size_t>(body.data() - body_src.data()));
          advance_line_col(lead, body_line, body_col);
        }
        parse::CstNode *body_cst =
            ext_gram->parse(body, "expr", /*quiet=*/false, body_line - 1, body_col);
        if (!body_cst) {
          diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: failed to parse body of 'using syntax %.*s'\n",
                       static_cast<int>(ext_name.size()), ext_name.data());
          return nullptr;
        }
        syntax::SyntaxObj *stx = syntax::SyntaxObj::from_cst(body_cst);
        if (located) {
          offset_syntax_loc(stx, body_line - 1, body_col);
          if (body_node->file) stx->set_file(body_node->file);
        }
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
  decl_grammar_.add_rule("decl", "decl_syntax_rule_head");
  decl_grammar_.add_rule("decl", "decl_syntax_ext");
  decl_grammar_.add_rule("decl", "decl_check_head");
  decl_grammar_.add_rule("decl", "decl_macro_head");
  decl_grammar_.add_rule("decl", "decl_import");
  decl_grammar_.add_rule("decl", "decl_export");
  decl_grammar_.add_rule("decl", "decl_type");

  // Native DParser `.g`-style grammar declarations:
  //   grammar expr += expr '+' expr $left 3 core(binop, lhs=0, op=1, rhs=2);
  //   grammar int_lit : "[0-9]+" core(datum);
  //   grammar { expr += a | b; foo : 'x' expr; };
  decl_grammar_.add_rule("decl_grammar", "'grammar' gram_rule");
  decl_grammar_.add_rule("decl_grammar", "'grammar' '{' gram_rule+ '}' ';'?");
  decl_grammar_.add_rule("gram_rule", "ident gram_op gram_alts ';'");
  decl_grammar_.add_rule("gram_op", "'+='");
  decl_grammar_.add_rule("gram_op", "':='");
  decl_grammar_.add_rule("gram_op", "':'");
  decl_grammar_.add_rule("gram_alts", "gram_alt ('|' gram_alt)*");
  decl_grammar_.add_rule("gram_alt", "gram_rhs core_ann", "$right 2");
  decl_grammar_.add_rule("gram_alt", "gram_rhs", "$right 1");
  decl_grammar_.add_rule("gram_rhs", "gram_elem+");
  decl_grammar_.add_rule("gram_elem", "gram_atom gram_quant?");
  decl_grammar_.add_rule("gram_elem", "gram_mod");
  decl_grammar_.add_rule("gram_atom", "ident");
  decl_grammar_.add_rule("gram_atom", "sstr");
  decl_grammar_.add_rule("gram_atom", "str");
  decl_grammar_.add_rule("gram_atom", "'(' gram_inner ')'");
  decl_grammar_.add_rule("gram_inner", "gram_rhs ('|' gram_rhs)*");
  decl_grammar_.add_rule("gram_quant", "'*'");
  decl_grammar_.add_rule("gram_quant", "'+'");
  decl_grammar_.add_rule("gram_quant", "'?'");
  decl_grammar_.add_rule("gram_mod", R"d("\$[a-zA-Z_][a-zA-Z0-9_]*" "-?[0-9]+")d");
  decl_grammar_.add_rule("gram_mod", R"d("\$[a-zA-Z_][a-zA-Z0-9_]*")d");
  decl_grammar_.add_rule("sstr", R"d("'([^'\\]|\\[^])*'")d");
  decl_grammar_.add_rule("core_ann", "'core' '(' core_body ')'");
  decl_grammar_.add_rule("core_body", "\"[^)]+\"");

  decl_grammar_.add_rule("decl_prim_simple", "'prim' ident 'c_call' str ';'");
  decl_grammar_.add_rule("decl_prim_transfer_head", "'prim' ident 'c_call' str 'transfer' '=>'");
  decl_grammar_.add_rule("decl_hook_head", "'hook' ident '=>'");
  decl_grammar_.add_rule("decl_for_syntax_head", "'for_syntax' ident '=>'");
  decl_grammar_.add_rule("decl_syntax_rule",
                         "'syntax_rule' 'form'? ident '(' slot_list? ')' '=>' str ';'");
  decl_grammar_.add_rule("decl_syntax_rule_head",
                         "'syntax_rule' 'form'? ident '(' slot_list? ')' '=>'");
  decl_grammar_.add_rule("decl_syntax_ext", "'syntax_ext' ident '{' nested_text? '}' ';'");
  decl_grammar_.add_rule("decl_check_head", "'check' 'core' ident '=>'");
  decl_grammar_.add_rule("decl_check_head", "'check' 'call' ident '=>'");
  decl_grammar_.add_rule("decl_check_head", "'check' ident '=>'");
  decl_grammar_.add_rule("decl_import", "'import' str ';'");
  decl_grammar_.add_rule("decl_import", "'import' ident ';'");
  decl_grammar_.add_rule("decl_export", "'export' export_list ';'");
  decl_grammar_.add_rule("export_list", "export_item (',' export_item)*");
  decl_grammar_.add_rule("export_item", "'grammar'");
  decl_grammar_.add_rule("export_item", "ident");
  decl_grammar_.add_rule("decl_type", "'type' ident '=' type_variant ('|' type_variant)* ';'");
  decl_grammar_.add_rule("type_variant", "ident '(' type_fields ')'");
  decl_grammar_.add_rule("type_variant", "ident");
  decl_grammar_.add_rule("type_fields", "ident (',' ident)*");
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
  char quote = 0;
  for (size_t i = 0; i < src.size(); ++i) {
    char c = src[i];
    if (in_comment) {
      if (c == '\n') {
        in_comment = false;
        out.push_back('\n');
      }
      continue;
    }
    if (quote != 0) {
      out.push_back(c);
      if (c == '\\' && i + 1 < src.size()) {
        out.push_back(src[++i]);
      } else if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
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
         match_keyword(src, "for_syntax") || match_keyword(src, "import") ||
         match_keyword(src, "export") || match_keyword(src, "type");
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
    diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: #lang inheritance cycle or depth exceeded\n");
    return false;
  }
  // A root workbench is about to be changed in place: keep its pre-`#lang`
  // state so `#lang` modules imported later start from it (FL-12).
  if (depth == 0 && loading_lang_depth_ == 0 && !lang_base_) {
    std::shared_ptr<Workbench> snapshot(new Workbench(*this, /*share_cache=*/true));
    snapshot->lang_base_ = nullptr;
    snapshot->own_lang_base_.reset();
    own_lang_base_ = std::move(snapshot);
    lang_base_ = own_lang_base_.get();
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
    diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: cannot find #lang module '%.*s'\n",
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
  ++loading_lang_depth_;
  struct DepthGuard {
    int &d;
    ~DepthGuard() { --d; }
  } depth_guard{loading_lang_depth_};
  while (!rest.empty()) {
    size_t consumed = 0;
    if (!parse_and_apply_decl_prefix(rest, &consumed) || consumed == 0) {
      diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: invalid declaration in '%s' near '%.*s'\n", found_path.c_str(),
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

std::string single_line_snippet(std::string_view sv, size_t max_len = 60) {
  std::string out;
  size_t n = std::min(sv.size(), max_len);
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    char c = sv[i];
    out.push_back((c == '\n' || c == '\r' || c == '\t') ? ' ' : c);
  }
  return out;
}

void advance_pos(std::string_view chunk, int &line, int &col) {
  for (char c : chunk) {
    if (c == '\n') {
      ++line;
      col = 0;
    } else {
      ++col;
    }
  }
}

}  // namespace

bool Workbench::parse_and_apply_decl_prefix(std::string_view src, size_t *out_consumed,
                                            int base_line, int base_col) {
  auto decl_gram = compiled_decl_grammar();
  if (!decl_gram) return false;

  size_t head_consumed = 0;
  parse::CstNode *cst = decl_gram->parse_prefix(src, &head_consumed, "decl", /*quiet=*/true);
  if (!cst || head_consumed == 0) return false;

  const parse::CstNode *d =
      (cst->symbol == "decl" && cst->children.size() == 1) ? cst->children[0] : cst;
  if (!d) return false;

  if (d->symbol == "decl_syntax_rule_head") {
    size_t pos = head_consumed;
    while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) ++pos;
    int body_line = base_line;
    int body_col = base_col;
    advance_pos(src.substr(0, pos), body_line, body_col);
    std::string_view body_sv = src.substr(pos);

    // Same-length placeholders (`$x` -> `_x`, `$xs...` -> `_xs___`) so the
    // template can be parsed by the active guest grammar and any reported
    // syntax error column matches the original source 1-to-1.
    auto mask_placeholders = [](std::string_view text) {
      std::string checked;
      checked.reserve(text.size());
      for (size_t i = 0; i < text.size();) {
        if (text[i] == '"') {
          checked.push_back(text[i++]);
          while (i < text.size()) {
            checked.push_back(text[i]);
            if (text[i] == '\\' && i + 1 < text.size()) {
              checked.push_back(text[++i]);
            } else if (text[i] == '"') {
              ++i;
              break;
            }
            ++i;
          }
          continue;
        }
        if (text[i] == '$' && i + 1 < text.size() &&
            (std::isalpha(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '_')) {
          checked.push_back('_');
          ++i;
          while (i < text.size() &&
                 (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) {
            checked.push_back(text[i++]);
          }
          if (i + 3 <= text.size() && text.substr(i, 3) == "...") {
            checked += "___";
            i += 3;
          }
          continue;
        }
        checked.push_back(text[i++]);
      }
      return checked;
    };
    auto guest_gram = compiled_grammar();
    if (!guest_gram) return false;

    // The template ends where the guest grammar says the expression ends (and
    // a `;` follows), so keyword-delimited blocks such as `do a; b end` may
    // contain `;`. Fall back to the first `;` outside parens/braces/strings
    // for the error report.
    size_t semi = std::string_view::npos;
    {
      std::string masked = mask_placeholders(body_sv);
      size_t tmpl_consumed = 0;
      if (guest_gram->parse_prefix(masked, &tmpl_consumed, "expr", /*quiet=*/true) &&
          tmpl_consumed > 0) {
        size_t after = tmpl_consumed;
        while (after < body_sv.size() && std::isspace(static_cast<unsigned char>(body_sv[after]))) {
          ++after;
        }
        if (after < body_sv.size() && body_sv[after] == ';') semi = after;
      }
    }
    if (semi == std::string_view::npos) semi = find_decl_terminator(body_sv);
    if (semi == std::string_view::npos) {
      std::string snip = single_line_snippet(body_sv, 40);
      diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{cur_file_, body_line, body_col}, "langwb: %s%d:%d: expected ';' after syntax_rule template near '%s'\n",
                   syntax::file_prefix(cur_file_).c_str(), body_line, body_col, snip.c_str());
      return false;
    }
    std::string_view raw_tmpl = body_sv.substr(0, semi);
    while (!raw_tmpl.empty() && std::isspace(static_cast<unsigned char>(raw_tmpl.back()))) {
      raw_tmpl.remove_suffix(1);
    }
    if (raw_tmpl.empty()) return false;
    if (!guest_gram->parse(mask_placeholders(raw_tmpl), "expr", /*quiet=*/false, body_line - 1,
                           body_col)) {
      return false;
    }
    if (!apply_decl_cst(cst, nullptr, raw_tmpl)) return false;
    if (out_consumed) *out_consumed = pos + semi + 1;
    return true;
  }

  bool needs_body = d->symbol.ends_with("_head");
  if (!needs_body) {
    if (!apply_decl_cst(cst, nullptr)) return false;
    if (out_consumed) *out_consumed = head_consumed;
    return true;
  }

  // Parse the declaration body eagerly at declaration time using the active guest grammar.
  size_t pos = head_consumed;
  while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) ++pos;
  int body_line = base_line;
  int body_col = base_col;
  advance_pos(src.substr(0, pos), body_line, body_col);
  std::string_view body_sv = src.substr(pos);

  auto guest_gram = compiled_grammar();
  if (!guest_gram) return false;
  size_t body_consumed = 0;
  parse::CstNode *body_cst = guest_gram->parse_prefix(body_sv, &body_consumed, "expr",
                                                      /*quiet=*/false, body_line - 1, body_col);
  if (!body_cst || body_consumed == 0) {
    return false;
  }
  pos += body_consumed;
  while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) ++pos;
  if (pos >= src.size() || src[pos] != ';') {
    // Re-parse the full body slice up to the next `;` so DParser reports the
    // actual syntax error location (e.g. `*` in `1 + * 3;`) at the real file L:C.
    size_t semi = find_decl_terminator(body_sv);
    if (semi != std::string_view::npos && semi > body_consumed) {
      (void)guest_gram->parse(body_sv.substr(0, semi), "expr", /*quiet=*/false, body_line - 1,
                              body_col);
    } else {
      int err_line = base_line;
      int err_col = base_col;
      advance_pos(src.substr(0, pos), err_line, err_col);
      std::string snip = single_line_snippet(src.substr(pos), 40);
      diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{cur_file_, err_line, err_col}, "langwb: %s%d:%d: expected ';' after declaration body near '%s'\n",
                   syntax::file_prefix(cur_file_).c_str(), err_line, err_col, snip.c_str());
    }
    return false;
  }
  ++pos;  // consume ';'

  syntax::SyntaxObj *body_stx = syntax::SyntaxObj::from_cst(body_cst);
  if (!apply_decl_cst(cst, body_stx)) return false;
  if (out_consumed) *out_consumed = pos;
  return true;
}

bool Workbench::apply_decl_cst(const parse::CstNode *decl_cst, syntax::SyntaxObj *body_stx,
                               std::string_view raw_tmpl) {
  if (!decl_cst) return false;
  const parse::CstNode *d =
      (decl_cst->symbol == "decl" && decl_cst->children.size() == 1) ? decl_cst->children[0]
                                                                     : decl_cst;
  if (!d) return false;

  // 1. Native DParser `.g`-style grammar declaration(s):
  //    grammar <prod> (+=|:|:=) <rhs> [| <rhs>]*;
  //    grammar { <prod> (+=|:|:=) <rhs> ...; ... };
  if (d->symbol == "decl_grammar") {
    std::vector<const parse::CstNode *> rules;
    collect_cst_sym(d, "gram_rule", rules);
    if (rules.empty()) return false;
    for (const parse::CstNode *rule : rules) {
      if (rule->children.size() < 3) return false;
      const parse::CstNode *id = rule->children[0];
      const parse::CstNode *op = rule->children[1];
      if (!id || !op) return false;
      if (op->text == ":") {
        bool already_defined = false;
        for (const auto &p : grammar_def_.productions()) {
          if (p.name == id->text && !p.rules.empty()) {
            already_defined = true;
            break;
          }
        }
        if (already_defined) {
          diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{},
                       "langwb: production '%s' already exists; use '+=' to extend or ':=' to "
                       "replace it\n",
                       id->text.c_str());
          return false;
        }
      } else if (op->text == ":=") {
        grammar_def_.clear_production(id->text);
        if (recording_gram_ops_) {
          recording_gram_ops_->push_back(GrammarOp{.clear = true, .prod = id->text});
        }
      }
      std::vector<const parse::CstNode *> alts;
      collect_cst_sym(rule, "gram_alt", alts);
      if (alts.empty()) return false;
      for (const parse::CstNode *alt : alts) {
        if (alt->core.kind == parse::CoreFormKind::Amb && !alt->children.empty()) {
          const parse::CstNode *chosen = alt->children[0];
          for (const parse::CstNode *cand : alt->children) {
            if (find_cst_sym(cand, "core_body")) {
              chosen = cand;
              break;
            }
          }
          alt = chosen;
        }
        const parse::CstNode *rhs_node = nullptr;
        for (const parse::CstNode *c : alt->children) {
          if (c->symbol == "gram_rhs") {
            rhs_node = c;
            break;
          }
        }
        if (!rhs_node) return false;
        // Catch legacy quoted-string grammar rules (e.g. `grammar expr += "when_x";` or
        // `grammar when_x += "'when' expr";`) which would silently become DParser regexes.
        {
          std::string_view rt = trim_ws(rhs_node->text);
          if (rt.size() >= 2 && rt.front() == '"' && rt.back() == '"') {
            std::string inner;
            if (unquote_cst_str(rt, inner)) {
              bool has_single_quote = (inner.find('\'') != std::string::npos);
              bool plain_ident = !inner.empty() &&
                                 (std::isalpha(static_cast<unsigned char>(inner[0])) ||
                                  inner[0] == '_');
              for (char ch : inner) {
                if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) {
                  plain_ident = false;
                  break;
                }
              }
              if (has_single_quote || plain_ident) {
                diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{},
                             "langwb: grammar RHS %s looks like legacy quoted syntax; write "
                             "native .g rules without outer quotes (e.g. unquoted non-terminals "
                             "and '...' for literals)\n",
                             rhs_node->text.c_str());
                return false;
              }
            }
          }
        }
        parse::CoreRuleMeta core;
        if (const parse::CstNode *cb = find_cst_sym(alt, "core_body")) {
          if (!parse::parse_core_annotation(cb->text, &core)) return false;
        }
        grammar_def_.add_rule(id->text, rhs_node->text, "", core);
        if (recording_gram_ops_) {
          recording_gram_ops_->push_back(
              GrammarOp{.clear = false, .prod = id->text, .rhs = rhs_node->text, .core = core});
        }
      }
    }
    ++stats_->decls_applied;
    return true;
  }

  // 2. prim <name> c_call "<c_sym>" [transfer => <expr>];
  if (d->symbol == "decl_prim_simple" || d->symbol == "decl_prim_transfer_head") {
    if (in_syntax_ext_) {
      diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: 'prim' is not supported inside syntax_ext\n");
      return false;
    }
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
    if (in_syntax_ext_) {
      diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: 'for_syntax' is not supported inside syntax_ext\n");
      return false;
    }
    const parse::CstNode *id = find_cst_sym(d, "ident");
    if (!id || !body_stx) return false;
    for_syntax_defs_.emplace_back(id->text, body_stx);
    ++stats_->decls_applied;
    return true;
  }

  // 5. syntax_rule [form] <name>(<slots...>) => <template> | "<template>";
  if (d->symbol == "decl_syntax_rule" || d->symbol == "decl_syntax_rule_head") {
    bool is_form = false;
    for (const parse::CstNode *c : d->children) {
      if (c && c->text == "form") is_form = true;
    }
    const parse::CstNode *id = find_cst_sym(d, "ident");
    if (!id) return false;
    std::string tmpl;
    if (d->symbol == "decl_syntax_rule_head") {
      if (raw_tmpl.empty()) return false;
      tmpl = std::string(raw_tmpl);
    } else {
      const parse::CstNode *s = find_cst_sym(d, "str");
      if (!s || !unquote_cst_str(s->text, tmpl)) return false;
    }

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
    Workbench ext_wb(*this, /*share_cache=*/true);
    ext_wb.stage0_ = macro::MacroExpander{};
    SyntaxExtDelta delta;
    ext_wb.recording_gram_ops_ = &delta.gram_ops;
    ext_wb.in_syntax_ext_ = true;
    if (nt && !nt->text.empty()) {
      std::string cleaned = strip_comments(nt->text);
      std::string_view rest = trim_ws(cleaned);
      while (!rest.empty()) {
        size_t consumed = 0;
        if (!ext_wb.parse_and_apply_decl_prefix(rest, &consumed) || consumed == 0) {
          std::string snip = single_line_snippet(rest, 60);
          diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: invalid declaration in syntax_ext '%s' near '%s'\n",
                       id->text.c_str(), snip.c_str());
          return false;
        }
        rest = trim_ws(rest.substr(consumed));
      }
    }
    ext_wb.recording_gram_ops_ = nullptr;
    ext_wb.in_syntax_ext_ = false;
    if (!ext_wb.compiled_grammar()) return false;
    delta.stage0_delta = std::move(ext_wb.stage0_);
    syntax_exts_[id->text] = std::move(delta);
    ++stats_->decls_applied;
    return true;
  }

  // 5c. check [core|call] <target> => <expr>;
  if (d->symbol == "decl_check_head") {
    if (in_syntax_ext_) {
      diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: 'check' is not supported inside syntax_ext\n");
      return false;
    }
    CheckKind kind = CheckKind::Any;
    for (const parse::CstNode *c : d->children) {
      if (c && c->text == "core") kind = CheckKind::Core;
      if (c && c->text == "call") kind = CheckKind::Call;
    }
    const parse::CstNode *id = find_cst_sym(d, "ident");
    if (!id || !body_stx) return false;
    if (kind == CheckKind::Core && !parse::is_core_form_name(id->text)) {
      diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: unknown core form '%s' in 'check core' declaration\n",
                   id->text.c_str());
      return false;
    }
    if (!register_static_check_stx(id->text, body_stx, kind)) return false;
    ++stats_->decls_applied;
    return true;
  }

  // 5d. export <item>, ...;
  if (d->symbol == "decl_export") {
    std::vector<const parse::CstNode *> items;
    collect_cst_sym(d, "export_item", items);
    for (const parse::CstNode *it : items) {
      if (it->text == "grammar") {
        module_export_grammar_ = true;
      } else {
        module_exports_.insert(it->text);
      }
    }
    ++stats_->decls_applied;
    return true;
  }

  // 5e. type <Name> = <Variant> [(<fields...>)] | ...;
  if (d->symbol == "decl_type") {
    const parse::CstNode *type_id = find_cst_sym(d, "ident");
    if (!type_id) return false;
    ifa_bridge::AdtDef adt;
    adt.name = type_id->text;
    std::vector<const parse::CstNode *> vnodes;
    collect_cst_sym(d, "type_variant", vnodes);
    if (vnodes.empty()) return false;
    std::unordered_set<std::string> seen_variants;
    for (const parse::CstNode *vn : vnodes) {
      if (vn->children.empty()) return false;
      ifa_bridge::AdtVariant var;
      var.type_name = adt.name;
      var.name = vn->children[0]->text;
      if (!seen_variants.insert(var.name).second) {
        diag::emitf(diag::Severity::Error, "adt", diag::SourceRange{cur_file_, vn->children[0]->line, vn->children[0]->col}, "langwb: %s%d:%d: duplicate variant '%s' in type '%s'\n",
                     syntax::file_prefix(cur_file_).c_str(), vn->children[0]->line, vn->children[0]->col, var.name.c_str(),
                     adt.name.c_str());
        return false;
      }
      if (const auto *prev = prims_.find_variant(var.name); prev && prev->type_name != adt.name) {
        diag::emitf(diag::Severity::Error, "adt", diag::SourceRange{cur_file_, vn->children[0]->line, vn->children[0]->col}, "langwb: %s%d:%d: variant '%s' is already defined by type '%s'\n",
                     syntax::file_prefix(cur_file_).c_str(), vn->children[0]->line, vn->children[0]->col, var.name.c_str(),
                     prev->type_name.c_str());
        return false;
      }
      if (const parse::CstNode *tf = find_cst_sym(vn, "type_fields")) {
        std::vector<const parse::CstNode *> fids;
        collect_cst_sym(tf, "ident", fids);
        std::unordered_set<std::string> seen_fields;
        for (const parse::CstNode *fid : fids) {
          if (fid->text == "_tag") {
            diag::emitf(diag::Severity::Error, "adt", diag::SourceRange{cur_file_, fid->line, fid->col},
                         "langwb: %s%d:%d: '_tag' cannot be a field of variant '%s': it is reserved "
                         "for the variant of ADT values\n",
                         syntax::file_prefix(cur_file_).c_str(), fid->line, fid->col, var.name.c_str());
            return false;
          }
          if (!seen_fields.insert(fid->text).second) {
            diag::emitf(diag::Severity::Error, "adt", diag::SourceRange{cur_file_, fid->line, fid->col}, "langwb: %s%d:%d: duplicate field '%s' in variant '%s'\n",
                         syntax::file_prefix(cur_file_).c_str(), fid->line, fid->col, fid->text.c_str(), var.name.c_str());
            return false;
          }
          var.fields.push_back(fid->text);
        }
      }
      adt.variants.push_back(std::move(var));
    }
    prims_.register_adt(std::move(adt));
    ++stats_->decls_applied;
    return true;
  }

  // 6. [stage1_][form_]macro <name> => <expr>;
  if (d->symbol == "decl_macro_head") {
    const parse::CstNode *kw = find_cst_sym(d, "macro_kw");
    const parse::CstNode *id = find_cst_sym(d, "ident");
    if (!kw || !id || !body_stx) return false;
    if (in_syntax_ext_ && (kw->text == "stage1_macro" || kw->text == "stage1_form_macro")) {
      diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: '%s' is not supported inside syntax_ext\n", kw->text.c_str());
      return false;
    }
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
  grammar_def_.add_rule("expr", "'while' expr 'do' expr", "$right 0",
                        CoreRuleMeta{.kind = CoreFormKind::While, .body = 3, .test = 1});
  grammar_def_.add_rule("expr", R"d("[a-zA-Z_][a-zA-Z0-9_]*" '=' expr)d", "$right 0",
                        CoreRuleMeta{.kind = CoreFormKind::Assign, .rhs = 2, .lhs = 0});
  grammar_def_.add_rule("expr", R"d(field_lhs '.' "[a-zA-Z_][a-zA-Z0-9_]*" '=' expr)d", "$right 0",
                        CoreRuleMeta{.kind = CoreFormKind::SetField, .rhs = 4, .obj = 0, .field = 2});
  grammar_def_.add_rule("field_lhs", "\"[a-zA-Z_][a-zA-Z0-9_]*\"", {},
                        CoreRuleMeta{.kind = CoreFormKind::Ident});
  grammar_def_.add_rule("field_lhs", "'(' expr ')'", {},
                        CoreRuleMeta{.kind = CoreFormKind::Paren, .inner = 1});
  grammar_def_.add_rule("field_lhs", R"d(field_lhs '.' "[a-zA-Z_][a-zA-Z0-9_]*")d", {},
                        CoreRuleMeta{.kind = CoreFormKind::Field, .obj = 0, .field = 2});
  grammar_def_.add_rule("field_lhs", "field_lhs '(' args ')'", {},
                        CoreRuleMeta{.kind = CoreFormKind::App, .fn = 0, .args = 2});
  grammar_def_.add_rule("field_lhs", "field_lhs '(' ')'", {},
                        CoreRuleMeta{.kind = CoreFormKind::App, .fn = 0});
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
  ext.add_rule("match_pat", R"d("[a-zA-Z_][a-zA-Z0-9_]*" '(' pat_args ')')d");
  ext.add_rule("match_pat", R"d("[a-zA-Z_][a-zA-Z0-9_]*" '(' ')')d");
  ext.add_rule("match_pat", "\"[a-zA-Z_][a-zA-Z0-9_]*\"");
  ext.add_rule("pat_args", R"d("[a-zA-Z_][a-zA-Z0-9_]*" (',' "[a-zA-Z_][a-zA-Z0-9_]*")*)d");
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
        } else if (!tok.empty() && tok[0] >= 'A' && tok[0] <= 'Z') {
          std::string tmpl =
              "if __wb_is_variant(__m__, \"" + tok + "\", 0) then __b__ else __n__";
          cur = ctx.parse_template("expr", tmpl, {{"__b__", body}, {"__n__", cur}});
          if (cur && cur->core.test >= 0 &&
              static_cast<size_t>(cur->core.test) < cur->children.size()) {
            auto *t = cur->children[static_cast<size_t>(cur->core.test)];
            t->line = pat->children[0]->line;
            t->col = pat->children[0]->col;
            t->scopes = pat->children[0]->scopes;
          }
        } else {
          cur = ctx.parse_template("expr", "let __v__ = __m__ in __b__",
                                   {{"__v__", pat->children[0]}, {"__b__", body}});
        }
      } else if (pat->children.size() == 3 && pat->children[1]->text == "(" &&
                 pat->children[2]->text == ")") {
        const std::string &ctor = pat->children[0]->text;
        std::string tmpl =
            "if __wb_is_variant(__m__, \"" + ctor + "\", 0) then __b__ else __n__";
        cur = ctx.parse_template("expr", tmpl, {{"__b__", body}, {"__n__", cur}});
        if (cur && cur->core.test >= 0 &&
            static_cast<size_t>(cur->core.test) < cur->children.size()) {
          auto *t = cur->children[static_cast<size_t>(cur->core.test)];
          t->line = pat->children[0]->line;
          t->col = pat->children[0]->col;
          t->scopes = pat->children[0]->scopes;
        }
      } else if (pat->children.size() == 4 && pat->children[1]->text == "(" &&
                 pat->children[3]->text == ")") {
        const std::string &ctor = pat->children[0]->text;
        std::vector<SyntaxObj *> pvars;
        std::function<void(SyntaxObj *)> gather_pv = [&](SyntaxObj *n) {
          if (!n) return;
          if (n->is_terminal && n->text != ",") {
            pvars.push_back(n);
            return;
          }
          for (SyntaxObj *c : n->children) gather_pv(c);
        };
        gather_pv(pat->children[2]);
        SyntaxObj *inner = body;
        for (size_t pi = pvars.size(); pi-- > 0;) {
          if (pvars[pi]->text == "_") continue;
          std::string bind_tmpl = "let __v__ = __wb_variant_field(__m__, \"" + ctor + "\", " +
                                  std::to_string(pi) + ") in __b__";
          inner = ctx.parse_template("expr", bind_tmpl, {{"__v__", pvars[pi]}, {"__b__", inner}});
        }
        std::string shown = ctor + "(";
        for (size_t pi = 0; pi < pvars.size(); ++pi) {
          if (pi > 0) shown += ", ";
          shown += pvars[pi]->text;
        }
        shown += ")";
        std::string guard_tmpl = "if __wb_is_variant(__m__, \"" + ctor + "\", " +
                                 std::to_string(pvars.size()) + ", \"" + shown +
                                 "\") then __b__ else __n__";
        cur = ctx.parse_template("expr", guard_tmpl, {{"__b__", inner}, {"__n__", cur}});
        if (cur && cur->core.test >= 0 &&
            static_cast<size_t>(cur->core.test) < cur->children.size()) {
          auto *t = cur->children[static_cast<size_t>(cur->core.test)];
          t->line = pat->children[0]->line;
          t->col = pat->children[0]->col;
          t->scopes = pat->children[0]->scopes;
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
          diag::emitf(diag::Severity::Error, "workbench", syntax::range_of(form), "langwb: %s%d:%d: unclosed ',(' unquote in quasiquote\n",
                       syntax::file_prefix(form).c_str(), form->line,
                       form->col);
          return nullptr;
        }
        std::string_view splice_src = trim_ws(raw.substr(expr_start, (j - 1) - expr_start));
        parse::CstNode *scst = ctx.grammar().parse(splice_src, "expr");
        if (!scst) {
          diag::emitf(diag::Severity::Error, "workbench", syntax::range_of(form), "langwb: %s%d:%d: invalid unquote expression '%.*s' in quasiquote\n",
                       syntax::file_prefix(form).c_str(), form->line, form->col, static_cast<int>(splice_src.size()),
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
      diag::emitf(diag::Severity::Error, "workbench", syntax::range_of(form),
                   "langwb: %s%d:%d: quasiquote template does not parse as '%s': %s\n",
                   syntax::file_prefix(form).c_str(), form->line,
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


namespace {
// ADT variant classes are records named `Type::Variant` (or
// `module::Type::Variant`); nullary variants carry only the synthesized `_tag`.
bool is_adt_variant_class(Sym *c) {
  return c && c->type_kind == Type_RECORD && c->name && std::strstr(c->name, "::");
}
bool is_nullary_variant_class(Sym *c) {
  return is_adt_variant_class(c) && c->has.n == 1 && c->has[0] && c->has[0]->name &&
         std::strcmp(c->has[0]->name, "_tag") == 0;
}
std::string adt_type_of(Sym *c) {
  std::string_view n(c->name);
  return std::string(n.substr(0, n.rfind("::")));
}

// What an `==` operand can be, for the constructor-equality rule.
struct AdtEqShape {
  bool any_adt = false;      // some value is an ADT constructor
  bool all_adt = true;       // every value is a constructor of one ADT
  bool all_nullary = true;   // every value is a nullary constructor
  std::string type_name;
};
AdtEqShape adt_eq_shape(AVar *av) {
  AdtEqShape sh;
  for (CreationSet *cs : *av->out) {
    if (!cs || !cs->sym) continue;
    Sym *c = cs->sym;
    if (!is_adt_variant_class(c)) {
      sh.all_adt = false;
      sh.all_nullary = false;
      continue;
    }
    sh.any_adt = true;
    std::string t = adt_type_of(c);
    if (sh.type_name.empty()) {
      sh.type_name = t;
    } else if (sh.type_name != t) {
      sh.all_adt = false;
    }
    if (!is_nullary_variant_class(c)) sh.all_nullary = false;
  }
  if (!sh.any_adt) sh.all_adt = false;
  return sh;
}

// The variant classes a codegen-time type can hold, or {} if it is not an ADT.
std::vector<Sym *> adt_variant_members(Sym *ty) {
  std::vector<Sym *> out;
  if (!ty) return out;
  if (ty->type_kind == Type_SUM) {
    for (Sym *m : ty->has) {
      if (!is_adt_variant_class(m)) return {};
      out.push_back(m);
    }
  } else if (is_adt_variant_class(ty)) {
    out.push_back(ty);
  }
  return out;
}

// `v1 == v2` for ADT values where one side is only nullary constructors: true
// iff both are the same nullary variant. A side whose variant is statically
// known is decided at compile time; otherwise its classtag is tested.
llvm::Value *emit_adt_variant_eq(Var *v1, const std::vector<Sym *> &m1, Var *v2,
                                 const std::vector<Sym *> &m2) {
  llvm::Type *i1_ty = llvm::Type::getInt1Ty(*TheContext);
  auto all_nullary = [](const std::vector<Sym *> &ms) {
    for (Sym *m : ms) if (!is_nullary_variant_class(m)) return false;
    return true;
  };
  const std::vector<Sym *> &cands = all_nullary(m1) ? m1 : m2;
  llvm::Value *lv = nullptr, *rv = nullptr;
  auto is_variant = [&](Var *v, const std::vector<Sym *> &ms, llvm::Value *&val,
                        Sym *m) -> llvm::Value * {
    // By name, not Sym identity: every clone of a variant class is that variant,
    // and each is tested by its own classtag.
    std::vector<Sym *> hits;
    for (Sym *x : ms) {
      if (x == m || std::strcmp(x->name, m->name) == 0) hits.push_back(x);
    }
    if (hits.empty()) return llvm::ConstantInt::getFalse(i1_ty);
    if (hits.size() == ms.size()) return llvm::ConstantInt::getTrue(i1_ty);
    if (!val) val = llvm_emit_get_var(v);
    llvm::Value *any = llvm::ConstantInt::getFalse(i1_ty);
    for (Sym *x : hits) {
      llvm::Value *t = val ? llvm_emit_classtag_eq(x, val) : nullptr;
      if (t) any = Builder->CreateOr(any, t);
    }
    return any;
  };
  llvm::Value *res = llvm::ConstantInt::getFalse(i1_ty);
  for (Sym *m : cands) {
    llvm::Value *both = Builder->CreateAnd(is_variant(v1, m1, lv, m), is_variant(v2, m2, rv, m));
    res = Builder->CreateOr(res, both);
  }
  return res;
}
}  // namespace
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
  // Tagged heterogeneous cons: records element tag (INT/BOOL/STR/LIST/OBJ) in WbCons.
  prims_.register_prim(
      "cons",
      PrimitiveHandler{
          .transfer =
              [](PNode *pn, EntrySet *es) {
                prim_emit_result(pn, es, make_abstract_type(wb_list_type_sym()));
              },
          .emit_llvm =
              [](PNode *pn, Fun * /*f*/) {
                if (pn->rvals.n < 4) return;
                Var *elem_var = pn->rvals[2];
                llvm::Value *elem_v = llvm_emit_get_var(elem_var);
                llvm::Value *tail_v = llvm_emit_get_var(pn->rvals[3]);
                if (!elem_v || !tail_v) return;
                llvm::Type *i64_ty = llvm::Type::getInt64Ty(*TheContext);
                int64_t tag = 4;  // WB_TAG_OBJ
                Sym *esym = elem_var->type;
                if (esym == wb_list_type_sym()) {
                  tag = 3;  // WB_TAG_LIST
                } else if (esym == sym_bool) {
                  tag = 1;  // WB_TAG_BOOL
                } else if (esym == sym_string) {
                  tag = 2;  // WB_TAG_STR
                } else if (esym && (esym == sym_int64 || esym == sym_int ||
                                    esym->num_kind == IF1_NUM_KIND_INT)) {
                  tag = 0;  // WB_TAG_INT
                } else if (!esym && elem_v->getType()->isIntegerTy(64)) {
                  tag = 0;  // WB_TAG_INT
                } else if (!esym && elem_v->getType()->isIntegerTy(1)) {
                  tag = 1;  // WB_TAG_BOOL
                }
                llvm::Value *payload = elem_v;
                if (payload->getType()->isPointerTy()) {
                  payload = Builder->CreatePtrToInt(payload, i64_ty, "cons_p2i");
                } else if (payload->getType()->isIntegerTy() &&
                           payload->getType()->getIntegerBitWidth() < 64) {
                  payload = (tag == 1) ? Builder->CreateZExt(payload, i64_ty, "cons_zext")
                                       : Builder->CreateSExt(payload, i64_ty, "cons_sext");
                }
                llvm::FunctionType *ft =
                    llvm::FunctionType::get(i64_ty, {i64_ty, i64_ty, i64_ty}, false);
                llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_cons_tagged", ft);
                llvm::Value *res = Builder->CreateCall(
                    ft, fn.getCallee(), {llvm::ConstantInt::get(i64_ty, tag), payload, tail_v},
                    "cons");
                if (pn->lvals.n > 0) llvm_emit_put_result(pn->lvals[0], res);
              },
          .is_functional = false,
          .is_visible = true,
      });
  reg_rt_typed("head", "_CG_wb_head", true, false);
  reg_rt_typed("head_tag", "_CG_wb_head_tag", true, false);
  reg_rt_typed("head_list", "_CG_wb_head_list", true, true);
  reg_rt_typed("tail", "_CG_wb_tail", true, true);
  reg_rt_typed("is_nil", "_CG_wb_is_nil", true, false);
  reg_rt_typed("list_len", "_CG_wb_list_len", true, false);
  reg_rt_typed("wb_match_fail", "_CG_wb_match_fail", false, false);

  prims_.register_prim(
      "head_str",
      PrimitiveHandler{
          .transfer =
              [](PNode *pn, EntrySet *es) {
                prim_emit_result(pn, es, make_abstract_type(sym_string));
              },
          .emit_llvm =
              [](PNode *pn, Fun * /*f*/) {
                llvm::Value *xs = llvm_emit_get_var(pn->rvals[2]);
                if (!xs) return;
                llvm::Type *i64_ty = llvm::Type::getInt64Ty(*TheContext);
                llvm::Type *ptr_ty = llvm::PointerType::getUnqual(*TheContext);
                llvm::FunctionType *ft = llvm::FunctionType::get(ptr_ty, {i64_ty}, false);
                llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_head_str", ft);
                llvm::Value *res = Builder->CreateCall(ft, fn.getCallee(), {xs}, "head_str");
                if (pn->lvals.n > 0) llvm_emit_put_result(pn->lvals[0], res);
              },
          .is_functional = true,
          .is_visible = true,
      });
  prims_.register_prim(
      "head_bool",
      PrimitiveHandler{
          .transfer =
              [](PNode *pn, EntrySet *es) {
                prim_emit_result(pn, es, make_abstract_type(sym_bool));
              },
          .emit_llvm =
              [](PNode *pn, Fun * /*f*/) {
                llvm::Value *xs = llvm_emit_get_var(pn->rvals[2]);
                if (!xs) return;
                llvm::Type *i64_ty = llvm::Type::getInt64Ty(*TheContext);
                llvm::Type *i8_ty = llvm::Type::getInt8Ty(*TheContext);
                llvm::FunctionType *ft = llvm::FunctionType::get(i8_ty, {i64_ty}, false);
                llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_head_bool", ft);
                llvm::Value *res = Builder->CreateCall(ft, fn.getCallee(), {xs}, "head_bool");
                llvm::Type *dst_ty = llvm_emit_type(sym_bool);
                if (dst_ty && dst_ty != i8_ty) {
                  res = Builder->CreateICmpNE(res, llvm::ConstantInt::get(i8_ty, 0), "hb_i1");
                }
                if (pn->lvals.n > 0) llvm_emit_put_result(pn->lvals[0], res);
              },
          .is_functional = true,
          .is_visible = true,
      });

  // I/O primitives: `print(x)` and `println(x)` (write to stdout in both JIT and AOT).
  auto reg_print = [&](const char *name, int64_t newline) {
    prims_.register_prim(
        name,
        PrimitiveHandler{
            .transfer =
                [](PNode *pn, EntrySet *es) {
                  prim_emit_result(pn, es, make_abstract_type(sym_int64));
                },
            .emit_llvm =
                [newline](PNode *pn, Fun * /*f*/) {
                  if (pn->rvals.n < 3) return;
                  Var *arg_var = pn->rvals[2];
                  llvm::Value *v = llvm_emit_get_var(arg_var);
                  if (!v) return;
                  llvm::Type *i64_ty = llvm::Type::getInt64Ty(*TheContext);
                  llvm::Type *ptr_ty = llvm::PointerType::getUnqual(*TheContext);
                  llvm::FunctionType *begin_ft = llvm::FunctionType::get(ptr_ty, {}, false);
                  llvm::FunctionCallee begin_fn =
                      TheModule->getOrInsertFunction("_CG_wb_print_begin", begin_ft);
                  llvm::Value *saved = Builder->CreateCall(begin_ft, begin_fn.getCallee(), {}, "pr_beg");
                  ifa_bridge::emit_llvm_wb_value(arg_var->type, v, /*quote_strings=*/false, 0);
                  llvm::FunctionType *end_ft =
                      llvm::FunctionType::get(i64_ty, {ptr_ty, i64_ty}, false);
                  llvm::FunctionCallee end_fn =
                      TheModule->getOrInsertFunction("_CG_wb_print_end", end_ft);
                  llvm::Value *res = Builder->CreateCall(
                      end_ft, end_fn.getCallee(),
                      {saved, llvm::ConstantInt::get(i64_ty, newline)}, "pr_end");
                  if (pn->lvals.n > 0 && res) llvm_emit_put_result(pn->lvals[0], res);
                },
            .is_functional = false,
            .is_visible = true,
            .reads_fields = true,
        });
  };
  reg_print("print", 0);
  reg_print("println", 1);

  // String primitives: str_concat, str_len, str_eq, str_slice, int_to_str, str_to_int.
  enum class StrRet { String, Int64, Bool };
  auto reg_str_prim = [&](const char *name, const char *c_sym, StrRet ret_kind) {
    prims_.register_prim(
        name,
        PrimitiveHandler{
            .transfer =
                [ret_kind](PNode *pn, EntrySet *es) {
                  Sym *rs = (ret_kind == StrRet::String) ? sym_string
                            : (ret_kind == StrRet::Bool) ? sym_bool
                                                         : sym_int64;
                  prim_emit_result(pn, es, make_abstract_type(rs));
                },
            .emit_llvm =
                [c_sym, ret_kind](PNode *pn, Fun * /*f*/) {
                  llvm::Type *i64_ty = llvm::Type::getInt64Ty(*TheContext);
                  llvm::Type *i8_ty = llvm::Type::getInt8Ty(*TheContext);
                  llvm::Type *ptr_ty = llvm::PointerType::getUnqual(*TheContext);
                  std::vector<llvm::Type *> ptys;
                  std::vector<llvm::Value *> args;
                  for (int i = 2; i < pn->rvals.n; ++i) {
                    llvm::Value *v = llvm_emit_get_var(pn->rvals[i]);
                    if (!v) return;
                    args.push_back(v);
                    ptys.push_back(v->getType());
                  }
                  llvm::Type *c_ret_ty = (ret_kind == StrRet::String) ? ptr_ty
                                         : (ret_kind == StrRet::Bool) ? i8_ty
                                                                      : i64_ty;
                  llvm::FunctionType *ft = llvm::FunctionType::get(c_ret_ty, ptys, false);
                  llvm::FunctionCallee fn = TheModule->getOrInsertFunction(c_sym, ft);
                  llvm::Value *res = Builder->CreateCall(ft, fn.getCallee(), args, "strcall");
                  if (ret_kind == StrRet::Bool) {
                    llvm::Type *dst_ty = llvm_emit_type(sym_bool);
                    if (dst_ty && dst_ty != i8_ty) {
                      res = Builder->CreateICmpNE(res, llvm::ConstantInt::get(i8_ty, 0), "streq");
                    }
                  }
                  if (pn->lvals.n > 0) llvm_emit_put_result(pn->lvals[0], res);
                },
            .is_functional = true,
            .is_visible = true,
        });
  };
  reg_str_prim("str_concat", "_CG_wb_str_concat", StrRet::String);
  reg_str_prim("str_len", "_CG_wb_str_len", StrRet::Int64);
  reg_str_prim("str_eq", "_CG_wb_str_eq", StrRet::Bool);
  reg_str_prim("str_slice", "_CG_wb_str_slice", StrRet::String);
  reg_str_prim("int_to_str", "_CG_wb_int_to_str", StrRet::String);
  reg_str_prim("str_to_int", "_CG_wb_str_to_int", StrRet::Int64);

  // Polymorphic `==` / `!=` supporting int64, bool, and string (via `_CG_wb_str_eq`).
  auto reg_eq = [&](const char *name, bool is_ne) {
    prims_.register_prim(
        name,
        PrimitiveHandler{
            .transfer =
                [](PNode *pn, EntrySet *es) {
                  prim_emit_result(pn, es, make_abstract_type(sym_bool));
                },
            .check =
                [](PNode *pn, EntrySet *es) {
                  if (pn->rvals.n < 4 || pn->lvals.n < 1) return;
                  AVar *a1 = make_AVar(pn->rvals[2], es);
                  AVar *a2 = make_AVar(pn->rvals[3], es);
                  if (!a1 || !a2 || !a1->out || !a2->out || !a1->out->n || !a2->out->n) return;
                  // Constructors of one ADT compare by variant when at least one
                  // side can only be a nullary constructor (`x == None`).
                  AdtEqShape e1 = adt_eq_shape(a1);
                  AdtEqShape e2 = adt_eq_shape(a2);
                  if (e1.any_adt || e2.any_adt) {
                    bool ok = e1.all_adt && e2.all_adt && e1.type_name == e2.type_name &&
                              (e1.all_nullary || e2.all_nullary);
                    if (!ok) {
                      AVar *bad = (!e1.all_adt || (e2.all_adt && !e1.all_nullary)) ? a1 : a2;
                      if (e1.all_adt && e2.all_adt && e1.type_name == e2.type_name) {
                        bad = e1.all_nullary ? a2 : a1;
                      }
                      type_violation(ATypeViolation_kind::PRIMITIVE_ARGUMENT, bad, bad->out,
                                     make_AVar(pn->lvals[0], es));
                    }
                    return;
                  }
                  auto classify = [](AVar *av, bool &has_str, bool &has_bool, bool &has_int,
                                     bool &has_other) {
                    for (CreationSet *cs : *av->out) {
                      if (!cs || !cs->sym) continue;
                      Sym *s = cs->sym->type ? cs->sym->type : cs->sym;
                      if (cs->sym == sym_string || s == sym_string) {
                        has_str = true;
                      } else if (s == sym_bool) {
                        has_bool = true;
                      } else if (s == sym_int64 || s == sym_int ||
                                 (s->num_kind == IF1_NUM_KIND_INT && s != wb_list_type_sym())) {
                        has_int = true;
                      } else {
                        has_other = true;
                      }
                    }
                  };
                  bool s1 = false, b1 = false, i1 = false, o1 = false;
                  bool s2 = false, b2 = false, i2 = false, o2 = false;
                  classify(a1, s1, b1, i1, o1);
                  classify(a2, s2, b2, i2, o2);
                  bool ok = !o1 && !o2 &&
                            ((s1 && s2 && !b1 && !b2 && !i1 && !i2) ||
                             (b1 && b2 && !s1 && !s2 && !i1 && !i2) ||
                             (i1 && i2 && !s1 && !s2 && !b1 && !b2));
                  if (!ok) {
                    AVar *bad = (o1 || (!s1 && !b1 && !i1)) ? a1 : a2;
                    type_violation(ATypeViolation_kind::PRIMITIVE_ARGUMENT, bad, bad->out,
                                   make_AVar(pn->lvals[0], es));
                  }
                },
            .emit_llvm =
                [is_ne](PNode *pn, Fun * /*f*/) {
                  if (pn->rvals.n < 4) return;
                  Var *v1 = pn->rvals[2];
                  Var *v2 = pn->rvals[3];
                  std::vector<Sym *> m1 = adt_variant_members(v1->type);
                  std::vector<Sym *> m2 = adt_variant_members(v2->type);
                  if (!m1.empty() && !m2.empty()) {
                    if (pn->lvals.n > 0) {
                      llvm::Value *eq = emit_adt_variant_eq(v1, m1, v2, m2);
                      if (eq) {
                        llvm_emit_put_result(pn->lvals[0], is_ne ? Builder->CreateNot(eq) : eq);
                      }
                    }
                    return;
                  }
                  llvm::Value *lv = llvm_emit_get_var(v1);
                  llvm::Value *rv = llvm_emit_get_var(v2);
                  if (!lv || !rv) return;
                  llvm::Value *res = nullptr;
                  if (v1->type == sym_string && v2->type == sym_string) {
                    llvm::Type *i8_ty = llvm::Type::getInt8Ty(*TheContext);
                    llvm::Type *ptr_ty = llvm::PointerType::getUnqual(*TheContext);
                    llvm::FunctionType *ft = llvm::FunctionType::get(i8_ty, {ptr_ty, ptr_ty}, false);
                    llvm::FunctionCallee fn = TheModule->getOrInsertFunction("_CG_wb_str_eq", ft);
                    llvm::Value *eq8 = Builder->CreateCall(ft, fn.getCallee(), {lv, rv}, "streq");
                    llvm::Value *eq1 =
                        Builder->CreateICmpNE(eq8, llvm::ConstantInt::get(i8_ty, 0), "streq1");
                    res = is_ne ? Builder->CreateNot(eq1, "strne1") : eq1;
                  } else {
                    if (lv->getType() != rv->getType()) {
                      llvm::Type *i64_ty = llvm::Type::getInt64Ty(*TheContext);
                      if (lv->getType()->isIntegerTy() && !lv->getType()->isIntegerTy(64)) {
                        lv = lv->getType()->isIntegerTy(1) ? Builder->CreateZExt(lv, i64_ty)
                                                           : Builder->CreateSExt(lv, i64_ty);
                      }
                      if (rv->getType()->isIntegerTy() && !rv->getType()->isIntegerTy(64)) {
                        rv = rv->getType()->isIntegerTy(1) ? Builder->CreateZExt(rv, i64_ty)
                                                           : Builder->CreateSExt(rv, i64_ty);
                      }
                    }
                    res = is_ne ? Builder->CreateICmpNE(lv, rv, "ne")
                                : Builder->CreateICmpEQ(lv, rv, "eq");
                  }
                  if (pn->lvals.n > 0 && res) llvm_emit_put_result(pn->lvals[0], res);
                },
            .is_functional = true,
            .is_visible = false,
        });
  };
  reg_eq("wb_eq", false);
  reg_eq("wb_ne", true);

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
      diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: comptime evaluation failed\n");
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
      diag::emitf(diag::Severity::Error, "workbench", syntax::range_of(call), "langwb: stage1 macro 'pow_opt' expects 2 arguments at %d:%d\n",
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
      diag::emitf(diag::Severity::Error, "workbench", syntax::range_of(call), "langwb: stage1 macro 'if_const' expects 3 arguments at %d:%d\n",
                   call->line, call->col);
      return nullptr;
    }
    return ctx.attrs()->is_const_i64(args[0]) ? args[1]->clone() : args[2]->clone();
  });

  stage1_.register_call_macro(
      "sum_fields", [this](SyntaxObj *call, MacroContext &ctx) -> SyntaxObj * {
        auto args = MacroExpander::collect_call_args(call->children[2]);
        if (args.size() != 1 || !ctx.attrs()) {
          diag::emitf(diag::Severity::Error, "workbench", syntax::range_of(call), "langwb: stage1 macro 'sum_fields' expects 1 argument at %d:%d\n",
                       call->line, call->col);
          return nullptr;
        }
        const auto *attr = ctx.attrs()->get(args[0]);
        if (!attr || attr->field_names.empty()) {
          diag::emitf(diag::Severity::Error, "workbench", syntax::range_of(call),
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

syntax::SyntaxObj *Workbench::wrap_for_syntax_stx(syntax::SyntaxObj *body_stx) const {
  if (!body_stx || for_syntax_defs_.empty()) return body_stx;
  auto is_lambda_stx = [](const syntax::SyntaxObj *n) {
    while (n) {
      if (n->core.kind == parse::CoreFormKind::Lambda) return true;
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
    return false;
  };
  syntax::SyntaxObj *cur = body_stx;
  for (size_t i = for_syntax_defs_.size(); i-- > 0;) {
    const auto &[name, rhs_stx] = for_syntax_defs_[i];
    if (!rhs_stx) return nullptr;
    auto *binder = new syntax::SyntaxObj();
    binder->node_id = syntax::SyntaxObj::next_node_id();
    binder->symbol = "ident";
    binder->text = name;
    binder->is_terminal = true;
    binder->file = rhs_stx->file;
    binder->line = rhs_stx->line;
    binder->col = rhs_stx->col;
    binder->core.kind = parse::CoreFormKind::Ident;

    auto *let_stx = new syntax::SyntaxObj();
    let_stx->node_id = syntax::SyntaxObj::next_node_id();
    let_stx->symbol = "for_syntax_let";
    let_stx->file = rhs_stx->file;
    let_stx->line = rhs_stx->line;
    let_stx->col = rhs_stx->col;
    let_stx->core = parse::CoreRuleMeta{
        .kind = is_lambda_stx(rhs_stx) ? parse::CoreFormKind::LetRec : parse::CoreFormKind::Let,
        .bind = 0,
        .rhs = 1,
        .body = 2,
    };
    let_stx->children = {binder, rhs_stx->clone(), cur};
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
        diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: syntax_rule arity mismatch (got %zu, expected >= %zu)\n",
                     items.size(), fixed_count);
        return nullptr;
      }
    } else if (items.size() != slots.size()) {
      diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: syntax_rule arity mismatch (got %zu, expected %zu)\n",
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
          diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: syntax_rule class mismatch for $%s:%s...\n", vname.c_str(),
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
        diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: syntax_rule class mismatch for $%s:%s (got %s '%s')\n",
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

  macro::MacroFn scoped_fn = macro::MacroExpander::with_def_scope(std::move(fn), decl_scope_);
  if (is_form) {
    stage0_.register_form_macro(std::move(name), std::move(scoped_fn));
  } else {
    if (decl_scope_ != 0) declared_call_macros_.push_back(name);
    stage0_.register_call_macro(std::move(name), std::move(scoped_fn));
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
  syntax::SyntaxObj *wrapped_stx = wrap_for_syntax_stx(body_stx);
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
  // Wrap with the defining file's scope so template identifiers resolve to that
  // file's top-level `def`s. This holds for Stage-1 too: the Stage-1 pass walks
  // the Stage-0 output with `expand_head` and adds no scopes of its own, so
  // top-level binders still carry exactly the file scope there.
  macro::MacroFn scoped_fn = macro::MacroExpander::with_def_scope(std::move(fn), decl_scope_);
  if (is_form) {
    target.register_form_macro(std::move(name), std::move(scoped_fn));
  } else {
    if (decl_scope_ != 0 && &target == &stage0_) declared_call_macros_.push_back(name);
    target.register_call_macro(std::move(name), std::move(scoped_fn));
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

bool Workbench::register_static_check_stx(std::string target_name, syntax::SyntaxObj *body_stx,
                                          CheckKind kind) {
  if (!body_stx) return false;
  syntax::SyntaxObj *wrapped_stx = wrap_for_syntax_stx(body_stx);
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
  static_checks_[std::move(target_name)].push_back(
      StaticCheckEntry{kind, std::move(fn), /*from_lang=*/loading_lang_depth_ > 0});
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
  syntax::SyntaxObj *wrapped_stx = wrap_for_syntax_stx(body_stx);
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
    syntax::SyntaxObj *wrapped = wrap_for_syntax_stx(transfer_stx);
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

// Reports a syntax error in the top-level form at the start of `rest`.
// Parsing the whole remainder as `stmts` (`stmt (';' stmt)*`) puts the error
// where the form actually breaks, instead of where a shorter `stmt` / `expr`
// prefix happened to end (which, with unreserved keywords, can be just the
// leading keyword parsed as an identifier).
//
// `fallback_msg` is reported when `rest` does parse as `stmts`, i.e. the form
// is well-formed but not acceptable where it appears.
void report_top_level_form_error(const parse::DynamicGrammar &gram, std::string_view rest,
                                 int line, int col, const char *file_prefix,
                                 const char *fallback_msg = "expected ';' between top-level forms") {
  std::string_view start;
  if (gram.has_start_symbol("stmts")) {
    start = "stmts";
  } else if (gram.has_start_symbol("stmt")) {
    start = "stmt";
  }
  if (gram.parse(rest, start, /*quiet=*/true) == nullptr) {
    (void)gram.parse(rest, start, /*quiet=*/false, line - 1, col);
    return;
  }
  const char *file =
      (file_prefix && *file_prefix) ? syntax::SyntaxObj::intern_file(file_prefix) : nullptr;
  diag::emitf(diag::Severity::Error, "syntax", diag::SourceRange{file, line, col},
              "langwb: %s%d:%d: %s\n", file_prefix, line, col, fallback_msg);
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
  return eval_impl(src, out_result, nullptr, opts, out_info, nullptr);
}

bool Workbench::eval_value(std::string_view src, ifa_bridge::EvalValue *out_value,
                           const EvalOptions &opts, ifa_bridge::CompileInfo *out_info) {
  return eval_impl(src, nullptr, out_value, opts, out_info, nullptr);
}

bool Workbench::build_aot(std::string_view src, const std::string &output_path,
                          bool emit_llvm_only, const EvalOptions &opts) {
  std::string ll_path;
  if (emit_llvm_only) {
    ll_path = output_path;
  } else {
    const char *tmpdir = std::getenv("TMPDIR");
    std::string dir = (tmpdir && *tmpdir) ? tmpdir : "/tmp";
    if (!dir.empty() && dir.back() == '/') dir.pop_back();
    std::string tmpl = dir + "/langwb_XXXXXX.ll";
    int fd = ::mkstemps(tmpl.data(), 3);
    if (fd < 0) {
      diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: cannot create temporary LLVM IR file in '%s'\n", dir.c_str());
      return false;
    }
    ::close(fd);
    ll_path = std::move(tmpl);
  }
  if (!eval_impl(src, nullptr, nullptr, opts, nullptr, &ll_path)) {
    if (!emit_llvm_only) std::filesystem::remove(ll_path);
    return false;
  }
  if (emit_llvm_only) return true;

  // Locate liblangwb_rt.a: prefer next to the running `langwb` executable, then
  // fall back to compile-time LANGWB_RT_LIB and the current working directory.
  std::string rt_lib;
  {
    std::filesystem::path exe_dir = executable_dir();
    if (!exe_dir.empty() && std::filesystem::exists(exe_dir / "liblangwb_rt.a")) {
      rt_lib = (exe_dir / "liblangwb_rt.a").string();
    }
  }
#ifdef LANGWB_RT_LIB
  if (rt_lib.empty() && std::filesystem::exists(LANGWB_RT_LIB)) {
    rt_lib = LANGWB_RT_LIB;
  }
#endif
  if (rt_lib.empty() && std::filesystem::exists("liblangwb_rt.a")) {
    rt_lib = "liblangwb_rt.a";
  }
  if (rt_lib.empty()) {
    diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: AOT runtime archive 'liblangwb_rt.a' not found\n");
    std::filesystem::remove(ll_path);
    return false;
  }

  std::vector<std::string> args = {
      "clang",
      "-O2",
      "-Wno-override-module",
      ll_path,
      rt_lib,
      "-o",
      output_path,
      "-L/opt/homebrew/lib",
      "-L/usr/local/lib",
      "-lgc",
  };
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (auto &a : args) argv.push_back(a.data());
  argv.push_back(nullptr);

  pid_t pid = ::fork();
  if (pid < 0) {
    diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: fork() failed invoking clang\n");
    std::filesystem::remove(ll_path);
    return false;
  }
  if (pid == 0) {
    ::execvp("clang", argv.data());
    diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: failed to exec clang\n");
    _exit(127);
  }
  int status = 0;
  bool ok = (::waitpid(pid, &status, 0) == pid) && WIFEXITED(status) && (WEXITSTATUS(status) == 0);
  std::filesystem::remove(ll_path);
  return ok;
}

namespace {

syntax::SyntaxObj *make_import_alias_def(const std::string &name, syntax::ScopeId importer_scope,
                                         syntax::ScopeId mod_scope) {
  auto *lhs = new syntax::SyntaxObj();
  lhs->node_id = syntax::SyntaxObj::next_node_id();
  lhs->symbol = "ident";
  lhs->text = name;
  lhs->is_terminal = true;
  lhs->core.kind = parse::CoreFormKind::Ident;
  lhs->scopes.add(importer_scope);

  auto *rhs = new syntax::SyntaxObj();
  rhs->node_id = syntax::SyntaxObj::next_node_id();
  rhs->symbol = "ident";
  rhs->text = name;
  rhs->is_terminal = true;
  rhs->core.kind = parse::CoreFormKind::Ident;
  rhs->scopes.add(mod_scope);

  auto *def = new syntax::SyntaxObj();
  def->node_id = syntax::SyntaxObj::next_node_id();
  def->symbol = "import_alias";
  def->core = parse::CoreRuleMeta{.kind = parse::CoreFormKind::Define, .bind = 0, .rhs = 1};
  def->children.push_back(lhs);
  def->children.push_back(rhs);
  return def;
}

}  // namespace

namespace {
// How diagnostics name an imported module: relative to the working directory
// when it is below it, otherwise the absolute path.
std::string display_module_path(const std::string &path) {
  std::error_code ec;
  std::filesystem::path rel = std::filesystem::proximate(path, ec);
  if (ec || rel.empty() || *rel.begin() == "..") return path;
  return rel.string();
}
}  // namespace

bool Workbench::import_module_file(
    std::string_view spec, const std::string &importer_dir, syntax::ScopeAllocator &scopes,
    syntax::ScopeId importer_scope, std::vector<syntax::SyntaxObj *> &out_stmts,
    std::unordered_map<std::string, ImportedModule> &imported_cache,
    std::vector<std::string> &import_stack,
    std::unordered_map<std::string, std::string> &imported_val_sources) {
  namespace fs = std::filesystem;
  std::string found_path;
  auto try_path = [&](const fs::path &p) -> bool {
    std::error_code ec;
    if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
      found_path = fs::weakly_canonical(p, ec).string();
      return !found_path.empty();
    }
    return false;
  };

  std::string spec_str(spec);
  fs::path spec_path(spec_str);
  bool found = false;
  if (!importer_dir.empty() && spec_path.is_relative()) {
    fs::path base(importer_dir);
    found = try_path(base / spec_str) || try_path(base / (spec_str + ".ml")) ||
            try_path(base / (spec_str + ".lang"));
  }
  if (!found) {
    found = try_path(spec_str) || try_path(spec_str + ".ml") || try_path(spec_str + ".lang");
  }
  if (!found) {
    for (const auto &dir : lang_search_paths_) {
      fs::path base(dir);
      if (try_path(base / spec_str) || try_path(base / (spec_str + ".ml")) ||
          try_path(base / (spec_str + ".lang"))) {
        found = true;
        break;
      }
    }
  }
  if (found_path.empty()) {
    diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: cannot find imported module '%.*s'\n",
                 static_cast<int>(spec.size()), spec.data());
    return false;
  }

  auto apply_exports = [&](const ImportedModule &mod) -> bool {
    if (mod.export_grammar) {
      for (const auto &op : mod.gram_ops) {
        if (op.clear) {
          grammar_def_.clear_production(op.prod);
        } else {
          grammar_def_.add_rule(op.prod, op.rhs, "", op.core);
        }
      }
    }
    stage0_.merge_from(mod.stage0_exports);
    if (decl_scope_ != 0) {
      for (const auto &[k, _] : mod.stage0_exports.call_macros()) {
        declared_call_macros_.push_back(k);
      }
    }
    for (const auto &[k, delta] : mod.syntax_ext_exports) {
      syntax_exts_[k] = delta;
    }
    for (const auto &raw_adt : mod.adt_defs) {
      bool same_module_type = false;
      bool name_collision = false;
      for (const auto &existing : prims_.adts()) {
        if (existing.name == raw_adt.name && existing.origin_module == raw_adt.origin_module) {
          same_module_type = true;
          break;
        }
        if (existing.name == raw_adt.name) {
          name_collision = true;
        }
      }
      if (same_module_type) continue;
      for (const auto &v : raw_adt.variants) {
        if (const auto *prev = prims_.find_variant(v.name)) {
          diag::emitf(diag::Severity::Error, "adt", diag::SourceRange{},
                       "langwb: variant '%s' imported from '%s' conflicts with existing variant "
                       "of type '%s'\n",
                       v.name.c_str(), fs::path(mod.path).filename().c_str(),
                       prev->type_name.c_str());
          return false;
        }
      }
      ifa_bridge::AdtDef adt = raw_adt;
      if (name_collision) {
        std::string stem = fs::path(raw_adt.origin_module).stem().string();
        if (stem.empty()) stem = "mod";
        std::string qual = stem + "::" + raw_adt.name;
        adt.name = qual;
        for (auto &v : adt.variants) v.type_name = qual;
      }
      prims_.register_adt(std::move(adt));
    }
    for (const auto &name : mod.exported_def_names) {
      auto prev = imported_val_sources.find(name);
      if (prev != imported_val_sources.end() && prev->second != mod.path) {
        diag::emitf(diag::Severity::Warning, "workbench", diag::SourceRange{},
                     "langwb: warning: ambiguous import of '%s' (already imported from '%s', "
                     "shadowed by '%s')\n",
                     name.c_str(), fs::path(prev->second).filename().c_str(),
                     fs::path(mod.path).filename().c_str());
      }
      imported_val_sources[name] = mod.path;
      out_stmts.push_back(make_import_alias_def(name, importer_scope, mod.mod_scope));
    }
    return true;
  };

  if (auto it = imported_cache.find(found_path); it != imported_cache.end()) {
    if (it->second.in_progress) {
      diag::Builder b(diag::Severity::Error, "import", diag::SourceRange{});
      b.printf("langwb: import cycle: ");
      bool started = false;
      for (const auto &p : import_stack) {
        if (p == found_path) started = true;
        if (started) b.printf("%s -> ", fs::path(p).filename().c_str());
      }
      b.printf("%s\n", fs::path(found_path).filename().c_str());
      return false;
    }
    return apply_exports(it->second);
  }

  std::ifstream ifs(found_path);
  if (!ifs) {
    diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: cannot open imported module '%s'\n", found_path.c_str());
    return false;
  }
  std::ostringstream oss;
  oss << ifs.rdbuf();
  std::string mod_src = oss.str();

  // Insert in-progress sentinel before recursing to detect circular imports.
  syntax::ScopeId mod_scope = scopes.fresh();
  imported_cache[found_path] =
      ImportedModule{.mod_scope = mod_scope, .in_progress = true, .path = found_path};
  import_stack.push_back(found_path);
  struct ImportStackGuard {
    std::vector<std::string> &stack;
    std::unordered_map<std::string, ImportedModule> &cache;
    const std::string &key;
    bool committed = false;
    ~ImportStackGuard() {
      if (!stack.empty() && stack.back() == key) stack.pop_back();
      if (!committed) cache.erase(key);
    }
  } stack_guard{import_stack, imported_cache, found_path};

  // A module with its own `#lang` header is in that language regardless of
  // the importer's: start it from the pre-`#lang` base rather than loading
  // the language on top of the importer's (possibly the same, already loaded)
  // one. A header-less module inherits the importer's language.
  std::string_view mod_after_lang = mod_src;
  std::string mod_lang_name;
  bool mod_has_lang = extract_lang_header(mod_after_lang, &mod_lang_name);
  const Workbench &mod_parent = (mod_has_lang && lang_base_) ? *lang_base_ : *this;
  std::shared_ptr<Workbench> mod_wb_owner(new Workbench(mod_parent, /*share_cache=*/true));
  Workbench &mod_wb = *mod_wb_owner;
  mod_wb.lang_search_paths_ =
      with_source_dir(lang_search_paths_, fs::path(found_path).parent_path().string());
  mod_wb.module_exports_.clear();
  mod_wb.module_export_grammar_ = false;
  mod_wb.decl_scope_ = mod_scope;
  mod_wb.cur_file_ = syntax::SyntaxObj::intern_file(display_module_path(found_path));
  mod_wb.prims_.clear_adts();
  ImportedModule mod;
  mod.mod_scope = mod_scope;
  mod.in_progress = false;
  mod.path = found_path;
  mod_wb.recording_gram_ops_ = &mod.gram_ops;

  std::string mod_dir = fs::path(found_path).parent_path().string();
  std::unordered_map<std::string, std::string> mod_imported_vals;

  std::string_view after_lang = mod_after_lang;
  if (mod_has_lang && !mod_wb.load_lang(mod_lang_name)) return false;

  std::string cleaned = strip_comments(after_lang);
  int cur_line = 1, cur_col = 0;
  std::string_view rest = skip_ws_tracking(cleaned, cur_line, cur_col);
  while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back()))) {
    rest.remove_suffix(1);
  }

  auto decl_gram = mod_wb.compiled_decl_grammar();
  if (!decl_gram) return false;

  std::vector<syntax::SyntaxObj *> mod_stmts;
  while (!rest.empty()) {
    size_t imp_consumed = 0;
    if (parse::CstNode *imp_cst =
            decl_gram->parse_prefix(rest, &imp_consumed, "decl_import", /*quiet=*/true);
        imp_cst && imp_consumed > 0) {
      std::string child_spec;
      if (const parse::CstNode *s = find_cst_sym(imp_cst, "str")) {
        if (!unquote_cst_str(s->text, child_spec)) return false;
      } else if (const parse::CstNode *id = find_cst_sym(imp_cst, "ident")) {
        child_spec = id->text;
      }
      if (child_spec.empty() ||
          !mod_wb.import_module_file(child_spec, mod_dir, scopes, mod_scope, out_stmts,
                                     imported_cache, import_stack, mod_imported_vals)) {
        return false;
      }
      advance_line_col(rest.substr(0, imp_consumed), cur_line, cur_col);
      rest = skip_ws_tracking(rest.substr(imp_consumed), cur_line, cur_col);
      continue;
    }

    size_t head_probe = 0;
    if (decl_gram->parse_prefix(rest, &head_probe, "decl", /*quiet=*/true) && head_probe > 0) {
      size_t consumed = 0;
      if (!mod_wb.parse_and_apply_decl_prefix(rest, &consumed, cur_line, cur_col) ||
          consumed == 0) {
        std::string snip = single_line_snippet(rest, 60);
        diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{mod_wb.cur_file_, cur_line, cur_col}, "langwb: %s:%d:%d: invalid declaration near '%s'\n",
                     mod_wb.cur_file_, cur_line, cur_col, snip.c_str());
        return false;
      }
      advance_line_col(rest.substr(0, consumed), cur_line, cur_col);
      rest = skip_ws_tracking(rest.substr(consumed), cur_line, cur_col);
      continue;
    }

    auto gram = mod_wb.compiled_grammar();
    if (!gram) return false;
    const std::string mod_file_prefix = std::string(mod_wb.cur_file_) + ":";
    size_t consumed = 0;
    parse::CstNode *cst = gram->parse_prefix(rest, &consumed, "stmt", /*quiet=*/true);
    if (!cst || consumed == 0) {
      cst = gram->parse_prefix(rest, &consumed, "expr", /*quiet=*/true);
    }
    if (!cst || consumed == 0) {
      report_top_level_form_error(*gram, rest, cur_line, cur_col, mod_file_prefix.c_str());
      return false;
    }
    int form_line = cur_line, form_col = cur_col;
    std::string_view form_rest = rest;
    advance_line_col(rest.substr(0, consumed), cur_line, cur_col);
    rest = skip_ws_tracking(rest.substr(consumed), cur_line, cur_col);
    if (!rest.empty() && rest.front() == ';') {
      advance_line_col(rest.substr(0, 1), cur_line, cur_col);
      rest = skip_ws_tracking(rest.substr(1), cur_line, cur_col);
    } else if (!rest.empty()) {
      report_top_level_form_error(*gram, form_rest, form_line, form_col, mod_file_prefix.c_str());
      return false;
    }

    syntax::SyntaxObj *stx = syntax::SyntaxObj::from_cst(cst);
    offset_syntax_loc(stx, form_line - 1, form_col);
    stx->set_file(mod_wb.cur_file_);
    stx = stx->add_scope(mod_scope);
    syntax::SyntaxObj *stx0 = mod_wb.stage0_.expand(
        stx, *gram, scopes, nullptr, 64, /*run_hooks=*/true, /*run_module_begin=*/false,
        &mod_wb.grammar_def_);
    if (!stx0) return false;
    mod_stmts.push_back(stx0);
  }

  mod_wb.recording_gram_ops_ = nullptr;
  mod.exported_names = mod_wb.module_exports_;
  mod.export_grammar = mod_wb.module_export_grammar_;

  std::unordered_set<std::string> mod_adt_names;
  for (ifa_bridge::AdtDef adt : mod_wb.prims_.adts()) {
    // ADTs this module imported are passed through unchanged: their
    // visibility was decided by the module that defined them, whose own code
    // (already emitted with that module's scope) must keep seeing them.
    bool defined_here = adt.origin_module.empty() || adt.origin_module == found_path;
    if (adt.origin_module.empty()) adt.origin_module = found_path;
    bool type_exported = mod.exported_names.count(adt.name) > 0;
    mod_adt_names.insert(adt.name);
    for (auto &v : adt.variants) {
      mod_adt_names.insert(v.name);
      if (defined_here && !type_exported && !mod.exported_names.count(v.name)) {
        if (v.private_scope == 0) v.private_scope = mod_scope;
      }
    }
    mod.adt_defs.push_back(std::move(adt));
  }

  std::unordered_set<std::string> exported_macro_names;
  for (const auto &exp_name : mod.exported_names) {
    // The module's macros were wrapped with `mod_scope` when registered
    // (`decl_scope_`), so they are hygienic in the importer without rewrapping.
    if (mod.stage0_exports.copy_macro_from(mod_wb.stage0_, exp_name)) {
      exported_macro_names.insert(exp_name);
    }
    if (auto sit = mod_wb.syntax_exts_.find(exp_name); sit != mod_wb.syntax_exts_.end()) {
      mod.syntax_ext_exports[exp_name] = sit->second;
    }
  }

  // The module's own (and imported) unexported call macros stay usable by its
  // exported macros: their templates' identifiers carry `mod_scope`, and only
  // those see these copies (importer code cannot call them). Also forward any
  // transitively imported scoped helper macros.
  for (const auto &name : mod_wb.declared_call_macros_) {
    if (!exported_macro_names.count(name)) {
      mod.stage0_exports.copy_scoped_call_macro_from(mod_wb.stage0_, name, mod_scope);
    }
  }
  mod.stage0_exports.merge_scoped_call_macros_from(mod_wb.stage0_);
  mod.owner = mod_wb_owner;

  // `export grammar` also exports the form macros that expand the module's
  // productions; without them the importer could parse the new syntax but not
  // expand it.
  if (mod.export_grammar) {
    for (const auto &op : mod.gram_ops) {
      if (!op.clear && mod.stage0_exports.copy_form_macro_from(mod_wb.stage0_, op.prod)) {
        exported_macro_names.insert(op.prod);
      }
    }
  }

  // Emit the module's own statements (tagged with `mod_scope`) once, and record
  // which `def` binders are explicitly exported.
  for (syntax::SyntaxObj *st : mod_stmts) {
    out_stmts.push_back(st);
    const syntax::SyntaxObj *u = st;
    while (u && u->core.kind == parse::CoreFormKind::PassThrough && u->children.size() == 1) {
      u = u->children[0];
    }
    if (u && u->core.kind == parse::CoreFormKind::Define && u->core.bind >= 0 &&
        static_cast<size_t>(u->core.bind) < u->children.size()) {
      const syntax::SyntaxObj *b = u->children[static_cast<size_t>(u->core.bind)];
      while (b && !b->is_terminal && b->children.size() == 1) b = b->children[0];
      if (b && b->is_terminal && mod.exported_names.count(b->text)) {
        mod.exported_def_names.insert(b->text);
      }
    }
  }

  for (const auto &exp_name : mod.exported_names) {
    if (!mod.exported_def_names.count(exp_name) && !exported_macro_names.count(exp_name) &&
        !mod.syntax_ext_exports.count(exp_name) && !mod_adt_names.count(exp_name)) {
      diag::emitf(diag::Severity::Warning, "workbench", diag::SourceRange{}, "langwb: warning: '%s' exported from '%s' is not defined\n",
                   exp_name.c_str(), fs::path(found_path).filename().c_str());
    }
  }

  imported_cache[found_path] = mod;
  stack_guard.committed = true;
  return apply_exports(mod);
}

bool Workbench::eval_impl(std::string_view src, int64_t *out_i64, ifa_bridge::EvalValue *out_value,
                          const EvalOptions &opts, ifa_bridge::CompileInfo *out_info,
                          const std::string *aot_ll_path, InspectResult *out_inspect) {
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
    if (!opts.source_path.empty()) {
      session.lang_search_paths_ = with_source_dir(
          session.lang_search_paths_,
          std::filesystem::path(opts.source_path).parent_path().string());
    }
    if (has_lang && !session.load_lang(lang_name)) {
      return false;
    }
    auto decl_gram = session.compiled_decl_grammar();
    if (!decl_gram) return false;

    syntax::ScopeAllocator scopes;
    syntax::ScopeId mod_scope = scopes.fresh();
    std::vector<syntax::SyntaxObj *> top_stmts;
    session.decl_scope_ = mod_scope;
    std::unordered_map<std::string, ImportedModule> imported_cache;
    std::vector<std::string> import_stack;
    std::unordered_map<std::string, std::string> imported_val_sources;
    std::string importer_dir;
    if (!opts.source_path.empty()) {
      importer_dir = std::filesystem::path(opts.source_path).parent_path().string();
    }

    int cur_line = base_line;
    int cur_col = base_col;

    while (!rest.empty()) {
      size_t imp_consumed = 0;
      if (parse::CstNode *imp_cst =
              decl_gram->parse_prefix(rest, &imp_consumed, "decl_import", /*quiet=*/true);
          imp_cst && imp_consumed > 0) {
        std::string mod_spec;
        if (const parse::CstNode *s = find_cst_sym(imp_cst, "str")) {
          if (!unquote_cst_str(s->text, mod_spec)) return false;
        } else if (const parse::CstNode *id = find_cst_sym(imp_cst, "ident")) {
          mod_spec = id->text;
        }
        if (mod_spec.empty() ||
            !session.import_module_file(mod_spec, importer_dir, scopes, mod_scope, top_stmts,
                                        imported_cache, import_stack, imported_val_sources)) {
          return false;
        }
        advance_line_col(rest.substr(0, imp_consumed), cur_line, cur_col);
        rest = skip_ws_tracking(rest.substr(imp_consumed), cur_line, cur_col);
        continue;
      }

      // Grammar-driven declaration detection: only treat the next form as a
      // declaration when `decl_grammar_` matches a valid declaration head, so
      // keywords like `check`, `hook`, or `syntax_ext` remain usable as ordinary
      // identifiers in guest expressions (e.g. `def check = 2; check * 21`).
      size_t head_probe = 0;
      if (decl_gram->parse_prefix(rest, &head_probe, "decl", /*quiet=*/true) && head_probe > 0) {
        size_t consumed = 0;
        if (!session.parse_and_apply_decl_prefix(rest, &consumed, cur_line, cur_col) ||
            consumed == 0) {
          std::string snip = single_line_snippet(rest, 60);
          diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{nullptr, cur_line, cur_col}, "langwb: %d:%d: invalid declaration near '%s'\n", cur_line, cur_col,
                       snip.c_str());
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
          return session.eval_single_expr(rest, out_i64, out_value, opts, out_info, aot_ll_path,
                                          out_inspect, cur_line - 1, cur_col, &scopes, mod_scope);
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
        report_top_level_form_error(*gram, rest, cur_line, cur_col, "");
        return false;
      }

      int form_line = cur_line;
      int form_col = cur_col;
      std::string_view form_rest = rest;
      advance_line_col(rest.substr(0, consumed), cur_line, cur_col);
      rest = skip_ws_tracking(rest.substr(consumed), cur_line, cur_col);
      if (!rest.empty() && rest.front() == ';') {
        advance_line_col(rest.substr(0, 1), cur_line, cur_col);
        rest = skip_ws_tracking(rest.substr(1), cur_line, cur_col);
      } else if (!rest.empty()) {
        report_top_level_form_error(*gram, form_rest, form_line, form_col, "");
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
      diag::emitf(diag::Severity::Error, "workbench", diag::SourceRange{}, "langwb: expected expression after declarations\n");
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

    return session.run_staged_and_jit(mod_stx, *gram, scopes, out_i64, out_value, opts, out_info,
                                      aot_ll_path, out_inspect);
  }

  return eval_single_expr(rest, out_i64, out_value, opts, out_info, aot_ll_path, out_inspect,
                          base_line - 1, base_col);
}

bool Workbench::eval_single_expr(std::string_view rest, int64_t *out_i64,
                                 ifa_bridge::EvalValue *out_value, const EvalOptions &opts,
                                 ifa_bridge::CompileInfo *out_info, const std::string *aot_ll_path,
                                 InspectResult *out_inspect, int line_offset, int col_offset,
                                 syntax::ScopeAllocator *scopes_in, syntax::ScopeId file_scope) {
  auto gram = compiled_grammar();
  if (!gram) return false;

  parse::CstNode *cst = gram->parse(rest, {}, /*quiet=*/true);
  if (!cst) {
    report_top_level_form_error(*gram, rest, line_offset + 1, col_offset, "",
                                "expected an expression (a program must end with one)");
    return false;
  }
  if (opts.dump_cst) {
    std::fprintf(opts.diag_fp, "=== CST ===\n");
    parse::dump_cst(cst, opts.diag_fp);
  }

  // Inside a multi-form evaluation the caller passes its allocator and file
  // scope: scopes it already handed out (the file's, imported modules', the
  // ones its macros were wrapped with) must not be reissued here, or an
  // expansion's intro scope can collide with one and flip it away.
  syntax::ScopeAllocator own_scopes;
  syntax::ScopeAllocator &scopes = scopes_in ? *scopes_in : own_scopes;
  syntax::SyntaxObj *stx = syntax::SyntaxObj::from_cst(cst);
  if (line_offset != 0 || col_offset != 0) {
    offset_syntax_loc(stx, line_offset, col_offset);
  }
  if (file_scope != 0) stx = stx->add_scope(file_scope);
  syntax::SyntaxObj *stx0 = stage0_.expand(stx, *gram, scopes, nullptr, 64, /*run_hooks=*/true,
                                           /*run_module_begin=*/true, &grammar_def_);
  if (!stx0) return false;

  return run_staged_and_jit(stx0, *gram, scopes, out_i64, out_value, opts, out_info, aot_ll_path,
                            out_inspect);
}

}  // namespace langwb
