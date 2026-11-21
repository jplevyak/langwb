#pragma once

#include <cstddef>
#include <string>
#include <string_view>

struct BinaryTables;
struct D_ParseNode;
struct D_Parser;

namespace langwb::parse {

// Compiles a DParser grammar from an in-memory string into relocatable binary
// tables and parses an input string. Minimal Phase-0 scaffolding.
class DynamicGrammar {
 public:
  DynamicGrammar() = default;
  ~DynamicGrammar();

  DynamicGrammar(const DynamicGrammar &) = delete;
  DynamicGrammar &operator=(const DynamicGrammar &) = delete;

  bool compile(std::string_view grammar_src);
  bool parse_check(std::string_view input) const;

 private:
  BinaryTables *tables_ = nullptr;
};

}  // namespace langwb::parse
