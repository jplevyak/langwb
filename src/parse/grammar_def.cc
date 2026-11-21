#include "parse/grammar_def.h"

#include <sstream>

namespace langwb::parse {

namespace {

uint64_t fnv1a_64(std::string_view data) {
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : data) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

}  // namespace

ProductionDef &GrammarDef::find_or_add_production(const std::string &name) {
  for (auto &p : productions_) {
    if (p.name == name) return p;
  }
  productions_.push_back(ProductionDef{.name = name, .rules = {}});
  return productions_.back();
}

void GrammarDef::add_rule(std::string prod_name, std::string rhs, std::string modifier) {
  auto &p = find_or_add_production(prod_name);
  p.rules.push_back(RuleDef{.rhs = std::move(rhs), .modifier = std::move(modifier)});
}

void GrammarDef::prepend_rule(std::string prod_name, std::string rhs, std::string modifier) {
  auto &p = find_or_add_production(prod_name);
  p.rules.insert(p.rules.begin(), RuleDef{.rhs = std::move(rhs), .modifier = std::move(modifier)});
}

void GrammarDef::extend(const GrammarDef &extension) {
  for (const auto &p : extension.productions_) {
    auto &dst = find_or_add_production(p.name);
    for (const auto &r : p.rules) {
      dst.rules.push_back(r);
    }
  }
}

std::string GrammarDef::render() const {
  std::ostringstream oss;
  for (const auto &p : productions_) {
    if (p.rules.empty()) continue;
    oss << p.name << ": ";
    for (size_t i = 0; i < p.rules.size(); ++i) {
      if (i > 0) oss << "\n    | ";
      oss << p.rules[i].rhs;
      if (!p.rules[i].modifier.empty()) {
        oss << ' ' << p.rules[i].modifier;
      }
    }
    oss << ";\n";
  }
  return oss.str();
}

uint64_t GrammarDef::content_hash() const { return fnv1a_64(render()); }

std::shared_ptr<DynamicGrammar> GrammarCache::compile_or_get(const GrammarDef &def) {
  std::string rendered = def.render();
  uint64_t h = fnv1a_64(rendered);

  auto &bucket = cache_[h];
  for (const auto &entry : bucket) {
    if (entry.rendered == rendered) {
      return entry.grammar;
    }
  }

  auto gram = std::make_shared<DynamicGrammar>();
  if (!gram->compile(rendered)) {
    return nullptr;
  }
  bucket.push_back(Entry{.rendered = std::move(rendered), .grammar = gram});
  return gram;
}

}  // namespace langwb::parse
