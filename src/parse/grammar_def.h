#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "parse/parse.h"

namespace langwb::parse {

struct RuleDef {
  std::string rhs;
  std::string modifier;  // e.g. "$binary_op_left 1", "$right 0", or ""
  CoreRuleMeta core;     // optional core form & child-role metadata
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
