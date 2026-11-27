#include "parse/grammar_def.h"

#include <algorithm>
#include <cctype>

#include "diag/diagnostic.h"

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
          << static_cast<int>(c.field) << (c.is_const ? ",const" : "") << ';';
    }
  }
  return oss.str();
}

uint64_t GrammarDef::content_hash() const { return fnv1a_64(cache_key()); }

void GrammarDef::set_last_rule_refs(std::string_view prod_name, std::vector<NontermRef> refs,
                                    NontermRef origin) {
  for (auto &p : productions_) {
    if (p.name == prod_name && !p.rules.empty()) {
      p.rules.back().refs = std::move(refs);
      p.rules.back().origin = std::move(origin);
      return;
    }
  }
}

namespace {

// A rule's elements and priority modifiers, from its DParser text.
struct RuleShape {
  std::vector<std::string> elems;  // non-terminal names; "" for terminals
  std::string rule_assoc;          // "$left N" / "$right N", or ""
  std::string op_assoc;            // "$binary_op_left N" / ..., or ""
  bool simple = true;              // no grouping or EBNF operators
};

RuleShape rule_shape(const std::string &text) {
  RuleShape out;
  size_t i = 0, n = text.size();
  bool in_mods = false;
  std::string *pending = nullptr;
  while (i < n) {
    char c = text[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '\'' || c == '"' || c == '[') {
      char close = c == '[' ? ']' : c;
      size_t j = i + 1;
      while (j < n && text[j] != close) j += (text[j] == '\\') ? 2 : 1;
      i = j + 1;
      if (!in_mods) out.elems.push_back("");
      continue;
    }
    size_t j = i;
    while (j < n && !std::isspace(static_cast<unsigned char>(text[j]))) ++j;
    std::string tok = text.substr(i, j - i);
    i = j;
    if (tok[0] == '$') {
      in_mods = true;
      pending = nullptr;
      if (tok == "$left" || tok == "$right") pending = &out.rule_assoc;
      else if (tok.rfind("$binary_op_", 0) == 0) pending = &out.op_assoc;
      if (pending) *pending = tok;
      continue;
    }
    if (pending && std::isdigit(static_cast<unsigned char>(tok[0]))) {
      *pending += " " + tok;
      pending = nullptr;
      continue;
    }
    if (in_mods) continue;
    if (tok.find_first_of("()*+?|{}") != std::string::npos) out.simple = false;
    out.elems.push_back(tok);
  }
  return out;
}

}  // namespace

int GrammarDef::report_priority_warnings() const {
  int count = 0;
  auto shape_of = [](const RuleDef &r) {
    return rule_shape(r.modifier.empty() ? r.rhs : r.rhs + " " + r.modifier);
  };
  auto has_rule_level_binary = [&](const std::string &prod, std::string *assoc) {
    for (const auto &p : productions_) {
      if (p.name != prod) continue;
      for (const auto &r : p.rules) {
        RuleShape sh = shape_of(r);
        if (!sh.rule_assoc.empty() && sh.elems.size() > 1 && sh.elems.front() == prod &&
            sh.elems.back() == prod) {
          *assoc = sh.rule_assoc;
          return true;
        }
      }
    }
    return false;
  };
  auto op_priority = [&](const std::string &prod, const RuleDef **guest_rule) -> std::string {
    for (const auto &p : productions_) {
      if (p.name != prod) continue;
      for (const auto &r : p.rules) {
        RuleShape sh = shape_of(r);
        if (!sh.op_assoc.empty()) {
          *guest_rule = &r;
          return sh.op_assoc;
        }
      }
    }
    return {};
  };
  std::vector<std::string> warned_ops;
  for (const auto &p : productions_) {
    for (const auto &r : p.rules) {
      RuleShape sh = shape_of(r);
      if (!sh.simple) continue;
      diag::SourceRange at{r.origin.file, r.origin.line, r.origin.col};
      std::string shown = r.rhs.substr(0, r.rhs.find(" $"));
      // A binary-looking rule (`X op X`) whose ends are not its own
      // non-terminal: DParser leaves it n-ary, so `$left` / `$right` do not
      // set its associativity.
      if (r.origin.line > 0 && !sh.rule_assoc.empty() && sh.elems.size() >= 3 &&
          !sh.elems.front().empty() && sh.elems.front() == sh.elems.back() &&
          sh.elems.front() != p.name) {
        diag::emitf(diag::Severity::Warning, "grammar", at,
                    "rule-level '%s' on '%s : %s' has no effect: neither end of the rule is "
                    "'%s'; give the operator its own production with an operator priority "
                    "(e.g. '%s : %s %s_op %s; %s_op : ... $binary_op_%s')",
                    sh.rule_assoc.c_str(), p.name.c_str(), shown.c_str(), p.name.c_str(),
                    p.name.c_str(), sh.elems.front().c_str(), p.name.c_str(), sh.elems.back().c_str(),
                    p.name.c_str(), sh.rule_assoc.rfind("$left", 0) == 0 ? "left N" : "right N");
        ++count;
      }
      // `Q : P X P` with an operator-priority X, where P also has rule-level
      // binary rules.
      if (sh.elems.size() == 3 && !sh.elems[0].empty() && sh.elems[0] == sh.elems[2] &&
          !sh.elems[1].empty()) {
        const RuleDef *op_rule = nullptr;
        std::string op_assoc = op_priority(sh.elems[1], &op_rule);
        std::string rule_assoc;
        if (op_assoc.empty() || !has_rule_level_binary(sh.elems[0], &rule_assoc)) continue;
        const NontermRef &o = op_rule->origin.line > 0 ? op_rule->origin : r.origin;
        if (o.line <= 0) continue;  // both built in
        if (std::find(warned_ops.begin(), warned_ops.end(), sh.elems[1]) != warned_ops.end()) continue;
        warned_ops.push_back(sh.elems[1]);
        diag::emitf(diag::Severity::Warning, "grammar", diag::SourceRange{o.file, o.line, o.col},
                    "operator production '%s' ('%s') is used between '%s' operands, but '%s' "
                    "also has rule-level '%s' rules, and the two forms do not group correctly "
                    "together: write every binary operator of '%s' as '%s <op> %s' with an "
                    "operator priority ('$binary_op_left N') on '<op>'",
                    sh.elems[1].c_str(), op_assoc.c_str(), sh.elems[0].c_str(), sh.elems[0].c_str(),
                    rule_assoc.c_str(), sh.elems[0].c_str(), sh.elems[0].c_str(), sh.elems[0].c_str());
        ++count;
      }
    }
  }
  return count;
}

int GrammarDef::report_undefined_nonterminals() const {
  int n = 0;
  for (const auto &p : productions_) {
    for (const auto &r : p.rules) {
      for (const auto &ref : r.refs) {
        if (has_production(ref.name)) continue;
        diag::emitf(diag::Severity::Error, "grammar",
                    diag::SourceRange{ref.file, ref.line, ref.col},
                    "grammar rule for '%s' refers to undefined non-terminal '%s'",
                    p.name.c_str(), ref.name.c_str());
        ++n;
      }
    }
  }
  return n;
}

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
    // Locate the cause in the language author's grammar declarations when it
    // is an undefined non-terminal; otherwise report DParser's own message.
    if (def.report_undefined_nonterminals() == 0) {
      diag::emitf(diag::Severity::Error, "grammar", diag::SourceRange{},
                  "grammar error: %s\n",
                  gram->last_error().empty() ? "grammar compilation failed"
                                             : gram->last_error().c_str());
    }
    return nullptr;
  }
  def.report_priority_warnings();
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
