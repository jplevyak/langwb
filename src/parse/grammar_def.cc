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

void GrammarDef::add_rule(std::string prod_name, std::string rhs, std::string modifier,
                          CoreRuleMeta core) {
  auto &p = find_or_add_production(prod_name);
  p.rules.push_back(RuleDef{.rhs = std::move(rhs), .modifier = std::move(modifier), .core = core});
}

void GrammarDef::prepend_rule(std::string prod_name, std::string rhs, std::string modifier,
                              CoreRuleMeta core) {
  auto &p = find_or_add_production(prod_name);
  p.rules.insert(p.rules.begin(),
                 RuleDef{.rhs = std::move(rhs), .modifier = std::move(modifier), .core = core});
}

void GrammarDef::clear_production(std::string_view prod_name) {
  for (auto &p : productions_) {
    if (p.name == prod_name) {
      p.rules.clear();
      return;
    }
  }
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

std::string GrammarDef::cache_key() const {
  std::ostringstream oss;
  oss << render() << "\n#core\n";
  for (const auto &p : productions_) {
    for (size_t i = 0; i < p.rules.size(); ++i) {
      const auto &c = p.rules[i].core;
      if (c.kind == CoreFormKind::None) continue;
      oss << p.name << ':' << i << '=' << static_cast<int>(c.kind) << ',' << static_cast<int>(c.bind)
          << ',' << static_cast<int>(c.rhs) << ',' << static_cast<int>(c.body) << ','
          << static_cast<int>(c.test) << ',' << static_cast<int>(c.then_b) << ','
          << static_cast<int>(c.else_b) << ',' << static_cast<int>(c.fn) << ','
          << static_cast<int>(c.args) << ',' << static_cast<int>(c.lhs) << ','
          << static_cast<int>(c.op) << ',' << static_cast<int>(c.inner) << ','
          << static_cast<int>(c.fields) << ',' << static_cast<int>(c.obj) << ','
          << static_cast<int>(c.field) << ';';
    }
  }
  return oss.str();
}

uint64_t GrammarDef::content_hash() const { return fnv1a_64(cache_key()); }

std::shared_ptr<DynamicGrammar> GrammarCache::compile_or_get(const GrammarDef &def) {
  std::string key = def.cache_key();
  uint64_t h = fnv1a_64(key);

  auto &bucket = cache_[h];
  for (const auto &entry : bucket) {
    if (entry.rendered == key) {
      return entry.grammar;
    }
  }

  auto gram = std::make_shared<DynamicGrammar>();
  if (!gram->compile(def.render())) {
    return nullptr;
  }
  for (const auto &p : def.productions()) {
    for (size_t i = 0; i < p.rules.size(); ++i) {
      if (p.rules[i].core.kind != CoreFormKind::None) {
        gram->set_rule_meta(p.name, static_cast<int>(i), p.rules[i].core);
      }
    }
  }
  bucket.push_back(Entry{.rendered = std::move(key), .grammar = gram});
  return gram;
}

}  // namespace langwb::parse
