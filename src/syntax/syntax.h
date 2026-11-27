#pragma once

#include "diag/diagnostic.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gc/gc_cpp.h"
#include "parse/parse.h"

namespace langwb::syntax {

using ScopeId = uint32_t;

class ScopeAllocator {
 public:
  ScopeId fresh() { return ++next_id_; }

 private:
  ScopeId next_id_ = 0;
};

// Sorted set of lexical / macro-introduction scopes (Racket sets-of-scopes model).
class ScopeSet {
 public:
  void add(ScopeId s);
  void remove(ScopeId s);
  void flip(ScopeId s);
  bool contains(ScopeId s) const;
  bool is_subset_of(const ScopeSet &other) const;

  size_t size() const { return scopes_.size(); }
  bool empty() const { return scopes_.empty(); }
  const std::vector<ScopeId> &raw() const { return scopes_; }

  bool operator==(const ScopeSet &other) const { return scopes_ == other.scopes_; }

 private:
  std::vector<ScopeId> scopes_;
};

// Hygienic syntax tree node carrying source span, grammar symbol, ScopeSet,
// and Racket-style key-value syntax properties.
struct SyntaxObj : public gc_cleanup {
  uint64_t node_id = 0;
  std::string symbol;
  std::string text;
  int line = 0;
  int col = 0;
  // Position just past the node's source text (0 when unknown, e.g. for nodes
  // built by macros rather than parsed).
  int end_line = 0;
  int end_col = 0;
  int rule_index = -1;
  parse::CoreRuleMeta core;
  // Macro provenance: the innermost expansion that produced (or contained)
  // this node and that macro's call site, then the expansions enclosing that
  // call (outermost last), so each step can be reported as a note.
  std::string origin_macro;
  int origin_line = 0;
  int origin_col = 0;
  struct Origin {
    std::string macro;
    const char *file = nullptr;
    int line = 0;
    int col = 0;
  };
  std::vector<Origin> outer_origins;
  // Introduced by a macro template (`parse_template`): its own position is
  // relative to the template text, so the expansion that introduces it gives
  // it the macro call's range (FL-11). Cleared when a macro locates the node
  // explicitly (`stx_copy_loc`). LSP lookups prefer the user's own nodes.
  bool synthetic = false;
  // Source file of an imported module or `.lang` (interned; see `intern_file`).
  // Null for the program being evaluated (`diag::main_file_name()`).
  const char *file = nullptr;
  bool is_terminal = false;
  ScopeSet scopes;
  std::unordered_map<std::string, int64_t> props;
  std::vector<SyntaxObj *, parse::GcAlloc<SyntaxObj *>> children;

  static uint64_t next_node_id();
  static SyntaxObj *from_cst(const parse::CstNode *cst);

  SyntaxObj *clone() const;
  SyntaxObj *add_scope(ScopeId s) const;
  SyntaxObj *flip_scope(ScopeId s) const;
  // Explicit hygiene escape (datum->syntax): replace scopes on this subtree.
  SyntaxObj *with_scopes(const ScopeSet &s) const;
  // Records the expansion of macro `macro_name` at `call` on the subtree:
  // nodes lacking an origin get this one (with `call`'s own provenance as the
  // enclosing steps), nodes without a `file` get `call`'s, and template nodes
  // (`synthetic`, or without a position) get `call`'s range.
  SyntaxObj *with_origin(std::string macro_name, const SyntaxObj *call) const;
  // Marks this subtree as template-introduced (in place).
  void mark_synthetic();
  // Sets `file` on this subtree (in place).
  void set_file(const char *f);
  // Returns a stable, process-lifetime copy of `path` for `SyntaxObj::file`.
  static const char *intern_file(const std::string &path);
};

void dump_syntax(const SyntaxObj *stx, FILE *fp = stdout, int indent = 0);

// Where a diagnostic about `node` is reported (its own position and file).
inline diag::SourceRange range_of(const SyntaxObj *node) {
  if (!node) return {};
  return diag::SourceRange{node->file, node->line, node->col, node->end_line, node->end_col};
}

// "in expansion of 'M'" notes for `node`'s macro provenance, innermost first
// (interposition hooks are not named).
std::vector<diag::Diagnostic> expansion_notes(const SyntaxObj *node);

// The name a form macro is reported under: its first keyword (the first
// terminal among the form's children, e.g. `case`, `not`, `**`), else its
// production name.
std::string form_display_name(const SyntaxObj *form);

// Hygienic lexical binding environment using Flatt's maximal-subset rule:
// a reference with `use_scopes` resolves to the unique binding with the same
// name whose `binding.scopes ⊆ use_scopes` and whose `binding.scopes.size()`
// is maximal.
template <typename T>
class BindingEnv {
 public:
  struct Entry {
    ScopeSet scopes;
    T value;
  };

  void bind(std::string name, ScopeSet scopes, T value) {
    auto &entries = table_[std::move(name)];
    for (auto &e : entries) {
      if (e.scopes == scopes) {
        e.value = std::move(value);
        return;
      }
    }
    entries.push_back(Entry{.scopes = std::move(scopes), .value = std::move(value)});
  }

  std::optional<T> resolve(const std::string &name, const ScopeSet &use_scopes) const {
    auto it = table_.find(name);
    if (it == table_.end()) return std::nullopt;

    const Entry *best = nullptr;
    bool ambiguous = false;
    for (const auto &entry : it->second) {
      if (!entry.scopes.is_subset_of(use_scopes)) continue;
      if (!best || entry.scopes.size() > best->scopes.size()) {
        best = &entry;
        ambiguous = false;
      } else if (entry.scopes.size() == best->scopes.size() && !(entry.scopes == best->scopes)) {
        ambiguous = true;
      }
    }
    if (!best || ambiguous) return std::nullopt;
    return best->value;
  }

 private:
  std::unordered_map<std::string, std::vector<Entry>> table_;
};

}  // namespace langwb::syntax
