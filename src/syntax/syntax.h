#pragma once

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
  int rule_index = -1;
  parse::CoreRuleMeta core;
  std::string origin_macro;
  int origin_line = 0;
  int origin_col = 0;
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
  // Record macro expansion provenance on nodes lacking an origin.
  SyntaxObj *with_origin(std::string macro_name, int call_line, int call_col) const;
};

void dump_syntax(const SyntaxObj *stx, FILE *fp = stdout, int indent = 0);

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
    table_[std::move(name)].push_back(Entry{.scopes = std::move(scopes), .value = std::move(value)});
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
