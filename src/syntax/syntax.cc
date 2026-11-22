#include "syntax/syntax.h"

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

SyntaxObj *SyntaxObj::from_cst(const parse::CstNode *cst) {
  if (!cst) return nullptr;
  auto *stx = new SyntaxObj();
  stx->node_id = next_node_id();
  stx->symbol = cst->symbol;
  stx->text = cst->text;
  stx->line = cst->line;
  stx->col = cst->col;
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
  auto *stx = new SyntaxObj();
  stx->node_id = node_id;
  stx->symbol = symbol;
  stx->text = text;
  stx->line = line;
  stx->col = col;
  stx->is_terminal = is_terminal;
  stx->scopes = scopes;
  stx->props = props;
  stx->children.reserve(children.size());
  for (const SyntaxObj *c : children) {
    stx->children.push_back(c ? c->clone() : nullptr);
  }
  return stx;
}

SyntaxObj *SyntaxObj::add_scope(ScopeId s) const {
  auto *stx = new SyntaxObj();
  stx->node_id = node_id;
  stx->symbol = symbol;
  stx->text = text;
  stx->line = line;
  stx->col = col;
  stx->is_terminal = is_terminal;
  stx->scopes = scopes;
  stx->scopes.add(s);
  stx->props = props;
  stx->children.reserve(children.size());
  for (const SyntaxObj *c : children) {
    stx->children.push_back(c ? c->add_scope(s) : nullptr);
  }
  return stx;
}

SyntaxObj *SyntaxObj::flip_scope(ScopeId s) const {
  auto *stx = new SyntaxObj();
  stx->node_id = node_id;
  stx->symbol = symbol;
  stx->text = text;
  stx->line = line;
  stx->col = col;
  stx->is_terminal = is_terminal;
  stx->scopes = scopes;
  stx->scopes.flip(s);
  stx->props = props;
  stx->children.reserve(children.size());
  for (const SyntaxObj *c : children) {
    stx->children.push_back(c ? c->flip_scope(s) : nullptr);
  }
  return stx;
}

SyntaxObj *SyntaxObj::with_scopes(const ScopeSet &s) const {
  auto *stx = new SyntaxObj();
  stx->node_id = node_id;
  stx->symbol = symbol;
  stx->text = text;
  stx->line = line;
  stx->col = col;
  stx->is_terminal = is_terminal;
  stx->scopes = s;
  stx->props = props;
  stx->children.reserve(children.size());
  for (const SyntaxObj *c : children) {
    stx->children.push_back(c ? c->with_scopes(s) : nullptr);
  }
  return stx;
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
