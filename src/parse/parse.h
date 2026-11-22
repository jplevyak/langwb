#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "gc/gc.h"
#include "gc/gc_cpp.h"

struct BinaryTables;
struct D_ParseNode;
struct D_Parser;

namespace langwb::parse {

// Exception-free Boehm GC allocator for STL containers holding GC pointers.
template <typename T>
struct GcAlloc {
  using value_type = T;
  GcAlloc() noexcept = default;
  template <typename U>
  GcAlloc(const GcAlloc<U> &) noexcept {}
  T *allocate(std::size_t n) {
    void *p = GC_MALLOC(n * sizeof(T));
    if (!p) std::abort();
    return static_cast<T *>(p);
  }
  void deallocate(T *, std::size_t) noexcept {}
  template <typename U>
  bool operator==(const GcAlloc<U> &) const noexcept {
    return true;
  }
};

// Concrete syntax tree node produced by an in-memory DParser grammar.
// GC-allocated so it can live seamlessly alongside IFA and DParser objects.
struct CstNode : public gc_cleanup {
  std::string symbol;
  std::string text;
  int line = 0;
  int col = 0;
  bool is_terminal = false;
  std::vector<CstNode *, GcAlloc<CstNode *>> children;
};

void dump_cst(const CstNode *node, FILE *fp = stdout, int indent = 0);

// Compiles a DParser grammar from an in-memory string into relocatable binary
// tables and parses input strings into a CstNode tree.
class DynamicGrammar {
 public:
  DynamicGrammar() = default;
  ~DynamicGrammar();

  DynamicGrammar(const DynamicGrammar &) = delete;
  DynamicGrammar &operator=(const DynamicGrammar &) = delete;

  bool compile(std::string_view grammar_src);
  CstNode *parse(std::string_view input, std::string_view start_symbol = {},
                 bool quiet = false) const;
  bool parse_check(std::string_view input) const;

 private:
  BinaryTables *tables_ = nullptr;
};

}  // namespace langwb::parse
