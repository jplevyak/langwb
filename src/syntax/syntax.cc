#include "syntax/syntax.h"

#include <unordered_set>

namespace langwb::syntax {

void ScopeSet::add(ScopeId s) {
  auto it = std::lower_bound(scopes_.begin(), scopes_.end(), s);
  if (it == scopes_.end() || *it != s) {
    scopes_.insert(it, s);
  }
}

void ScopeSet::remove(ScopeId s) {
  auto it = std::lower_bound(scopes_.begin(), scopes_.end(), s);
  if (it != scopes_.end() && *it == s) {
    scopes_.erase(it);
  }
}

void ScopeSet::flip(ScopeId s) {
  auto it = std::lower_bound(scopes_.begin(), scopes_.end(), s);
  if (it != scopes_.end() && *it == s) {
    scopes_.erase(it);
  } else {
    scopes_.insert(it, s);
  }
}

bool ScopeSet::contains(ScopeId s) const {
  return std::binary_search(scopes_.begin(), scopes_.end(), s);
}

bool ScopeSet::is_subset_of(const ScopeSet &other) const {
  return std::includes(other.scopes_.begin(), other.scopes_.end(), scopes_.begin(), scopes_.end());
}

uint64_t SyntaxObj::next_node_id() {
  static uint64_t counter = 0;
  return ++counter;
}

namespace {

SyntaxObj *copy_shallow(const SyntaxObj *src) {
  auto *stx = new SyntaxObj();
  stx->node_id = src->node_id;
  stx->symbol = src->symbol;
  stx->text = src->text;
  stx->line = src->line;
  stx->col = src->col;
  stx->end_line = src->end_line;
  stx->end_col = src->end_col;
  stx->rule_index = src->rule_index;
  stx->core = src->core;
  stx->origin_macro = src->origin_macro;
  stx->origin_line = src->origin_line;
  stx->origin_col = src->origin_col;
  stx->outer_origins = src->outer_origins;
  stx->synthetic = src->synthetic;
  stx->file = src->file;
  stx->is_terminal = src->is_terminal;
  stx->scopes = src->scopes;
  stx->props = src->props;
  return stx;
}

}  // namespace

SyntaxObj *SyntaxObj::from_cst(const parse::CstNode *cst) {
  if (!cst) return nullptr;
  auto *stx = new SyntaxObj();
  stx->node_id = next_node_id();
  stx->symbol = cst->symbol;
  stx->text = cst->text;
  stx->line = cst->line;
  stx->col = cst->col;
  stx->end_line = cst->end_line;
  stx->end_col = cst->end_col;
  stx->rule_index = cst->rule_index;
  stx->core = cst->core;
  stx->is_terminal = cst->is_terminal;
  stx->children.reserve(cst->children.size());
  for (const parse::CstNode *c : cst->children) {
    if (SyntaxObj *sc = from_cst(c)) {
      stx->children.push_back(sc);
    }
  }
  return stx;
}

SyntaxObj *SyntaxObj::clone() const {
  auto *stx = copy_shallow(this);
  stx->children.reserve(children.size());
  for (const SyntaxObj *c : children) {
    stx->children.push_back(c ? c->clone() : nullptr);
  }
  return stx;
}

SyntaxObj *SyntaxObj::add_scope(ScopeId s) const {
  auto *stx = copy_shallow(this);
  stx->scopes.add(s);
  stx->children.reserve(children.size());
  for (const SyntaxObj *c : children) {
    stx->children.push_back(c ? c->add_scope(s) : nullptr);
  }
  return stx;
}

SyntaxObj *SyntaxObj::flip_scope(ScopeId s) const {
  auto *stx = copy_shallow(this);
  stx->scopes.flip(s);
  stx->children.reserve(children.size());
  for (const SyntaxObj *c : children) {
    stx->children.push_back(c ? c->flip_scope(s) : nullptr);
  }
  return stx;
}

SyntaxObj *SyntaxObj::with_scopes(const ScopeSet &s) const {
  auto *stx = copy_shallow(this);
  stx->scopes = s;
  stx->children.reserve(children.size());
  for (const SyntaxObj *c : children) {
    stx->children.push_back(c ? c->with_scopes(s) : nullptr);
  }
  return stx;
}

SyntaxObj *SyntaxObj::with_origin(std::string macro_name, const SyntaxObj *call) const {
  auto *stx = copy_shallow(this);
  if (stx->origin_macro.empty()) {
    stx->origin_macro = macro_name;
    stx->origin_line = call ? call->line : 0;
    stx->origin_col = call ? call->col : 0;
    stx->outer_origins.clear();
    if (call && !call->origin_macro.empty()) {
      stx->outer_origins.push_back(
          Origin{call->origin_macro, call->file, call->origin_line, call->origin_col});
      stx->outer_origins.insert(stx->outer_origins.end(), call->outer_origins.begin(),
                                call->outer_origins.end());
    }
  }
  if (call && (this->synthetic || this->line <= 0) && this->origin_macro.empty() &&
      call->line > 0) {
    // Introduced by this expansion: located at the call.
    stx->line = call->line;
    stx->col = call->col;
    stx->end_line = call->end_line;
    stx->end_col = call->end_col;
    stx->file = call->file;
  }
  if (!stx->file && call) stx->file = call->file;
  stx->children.reserve(children.size());
  for (const SyntaxObj *c : children) {
    stx->children.push_back(c ? c->with_origin(macro_name, call) : nullptr);
  }
  return stx;
}

void SyntaxObj::mark_synthetic() {
  synthetic = true;
  for (SyntaxObj *c : children) {
    if (c) c->mark_synthetic();
  }
}

std::vector<diag::Diagnostic> expansion_notes(const SyntaxObj *node) {
  std::vector<diag::Diagnostic> out;
  if (!node || node->origin_macro.empty()) return out;
  auto add = [&](const std::string &macro, const char *file, int line, int col) {
    if (macro.rfind("hook:", 0) == 0 || line <= 0) return;
    out.push_back(diag::expansion_note(file, macro, line, col));
  };
  add(node->origin_macro, node->file, node->origin_line, node->origin_col);
  for (const SyntaxObj::Origin &o : node->outer_origins) add(o.macro, o.file, o.line, o.col);
  return out;
}

std::string form_display_name(const SyntaxObj *form) {
  if (!form) return {};
  for (const SyntaxObj *c : form->children) {
    // A child that is (a chain of single-child wrappers around) a literal
    // terminal, whose symbol is its own text: `'case'`, `not_op -> 'not'`.
    while (c && !c->is_terminal && c->children.size() == 1) c = c->children[0];
    if (c && c->is_terminal && !c->text.empty() && c->symbol == c->text) return c->text;
  }
  return form->symbol;
}

void SyntaxObj::set_file(const char *f) {
  file = f;
  for (SyntaxObj *c : children) {
    if (c) c->set_file(f);
  }
}

const char *SyntaxObj::intern_file(const std::string &path) {
  static std::unordered_set<std::string> *files = new std::unordered_set<std::string>();
  return files->insert(path).first->c_str();
}

void dump_syntax(const SyntaxObj *stx, FILE *fp, int indent) {
  if (!stx) return;
  for (int i = 0; i < indent; ++i) std::fputc(' ', fp);
  std::fprintf(fp, "%s \"%s\" {", stx->symbol.c_str(), stx->text.c_str());
  const auto &raw = stx->scopes.raw();
  for (size_t i = 0; i < raw.size(); ++i) {
    if (i > 0) std::fputc(',', fp);
    std::fprintf(fp, "%u", raw[i]);
  }
  std::fprintf(fp, "}\n");
  for (const SyntaxObj *c : stx->children) {
    dump_syntax(c, fp, indent + 2);
  }
}

}  // namespace langwb::syntax
