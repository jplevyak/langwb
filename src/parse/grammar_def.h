#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "parse/parse.h"

namespace langwb::parse {

// A non-terminal named in a rule's right-hand side, and where the grammar
// declaration names it (for diagnostics when it turns out to be undefined).
struct NontermRef {
  std::string name;
  const char *file = nullptr;  // interned module / `.lang` path; null for the program
  int line = 0;
  int col = 0;
};

struct RuleDef {
  std::string rhs;
  std::string modifier;  // e.g. "$left 1", "$right 0", or ""
  CoreRuleMeta core;     // optional core form & child-role metadata
  std::vector<NontermRef> refs;  // set for rules from guest `grammar` declarations
  // Where a guest `grammar` declaration wrote the rule (line 0 for built-ins).
  NontermRef origin;
};

struct ProductionDef {
  std::string name;
  std::vector<RuleDef> rules;
};

// Structured Grammar IR supporting composable syntax extensions and rendering
// to DParser grammar syntax for in-memory table compilation.
class GrammarDef {
 public:
  void add_rule(std::string prod_name, std::string rhs, std::string modifier = {},
                CoreRuleMeta core = {});
  void prepend_rule(std::string prod_name, std::string rhs, std::string modifier = {},
                    CoreRuleMeta core = {});
  // Records the non-terminals the most recently added rule of `prod_name` names.
  void set_last_rule_refs(std::string_view prod_name, std::vector<NontermRef> refs,
                          NontermRef origin = {});
  // FL-2: warns about binary-looking guest rules (`X op X`) whose rule-level
  // `$left N` / `$right N` DParser cannot apply (`X` is not the rule's own
  // non-terminal), and
  // about operator-priority productions (`$binary_op_left N`) used between
  // operands of a non-terminal that also has rule-level binary rules (the two
  // forms do not group correctly together). Returns the number of warnings.
  int report_priority_warnings() const;
  // Reports (as diagnostics) every recorded non-terminal reference that names
  // no production; returns how many were reported.
  int report_undefined_nonterminals() const;
  void clear_production(std::string_view prod_name);
  void extend(const GrammarDef &extension);

  std::string render() const;
  std::string cache_key() const;
  uint64_t content_hash() const;

  const std::vector<ProductionDef> &productions() const { return productions_; }
  bool has_production(std::string_view name) const {
    for (const auto &p : productions_) {
      if (p.name == name) return true;
    }
    return false;
  }

 private:
  ProductionDef &find_or_add_production(const std::string &name);
  std::vector<ProductionDef> productions_;
};

// Content-addressed in-memory cache of compiled DParser tables.
class GrammarCache {
 public:
  std::shared_ptr<DynamicGrammar> compile_or_get(const GrammarDef &def);
  size_t size() const { return cache_.size(); }

 private:
  struct Entry {
    std::string rendered;
    std::shared_ptr<DynamicGrammar> grammar;
  };
  std::unordered_map<uint64_t, std::vector<Entry>> cache_;
};

}  // namespace langwb::parse
