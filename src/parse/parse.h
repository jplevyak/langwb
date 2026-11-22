#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gc/gc.h"
#include "gc/gc_cpp.h"

struct BinaryTables;
struct D_ParseNode;
struct D_Parser;

namespace langwb::parse {

// Core form identity & child-role metadata attached to grammar rules via `core(...)`.
// Expander, interposition hooks, name resolution, and IF1 lowering dispatch on
// this metadata rather than literal minilang surface token text.
enum class CoreFormKind : uint8_t {
  None = 0,
  Let,          // bind, rhs, body
  LetRec,       // bind, rhs, body (binder visible in both rhs and body)
  Lambda,       // bind (params), body
  If,           // test, then_b, else_b
  App,          // fn, args
  BinOp,        // lhs, op, rhs
  Paren,        // inner
  Record,       // fields
  Field,        // obj, field
  Datum,        // integer literal
  String,       // string literal
  Ident,        // identifier
  PassThrough,  // single-child transparent wrapper (e.g. expr -> custom form)
  Seq,          // block / sequence of statements (inner = stmts list)
  Define,       // definition inside a Seq (bind, rhs)
  UsingSyntax,  // lexically scoped syntax region (bind = ext name, body = raw_block)
  Amb,          // GLR ambiguity node (children = parse alternatives)
};

struct CoreRuleMeta {
  CoreFormKind kind = CoreFormKind::None;
  int8_t bind = -1;
  int8_t rhs = -1;
  int8_t body = -1;
  int8_t test = -1;
  int8_t then_b = -1;
  int8_t else_b = -1;
  int8_t fn = -1;
  int8_t args = -1;
  int8_t lhs = -1;
  int8_t op = -1;
  int8_t inner = -1;
  int8_t fields = -1;
  int8_t obj = -1;
  int8_t field = -1;

  bool operator==(const CoreRuleMeta &) const = default;
};

const char *core_form_name(CoreFormKind kind);
bool parse_core_annotation(std::string_view spec, CoreRuleMeta *out);

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
  int rule_index = -1;
  CoreRuleMeta core;
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
  void set_rule_meta(std::string prod_name, int rule_index, CoreRuleMeta meta);
  CoreRuleMeta lookup_rule_meta(const std::string &prod_name, int rule_index) const;

  CstNode *parse(std::string_view input, std::string_view start_symbol = {},
                 bool quiet = false) const;
  CstNode *parse_prefix(std::string_view input, size_t *out_consumed,
                        std::string_view start_symbol = {}, bool quiet = false) const;
  bool parse_check(std::string_view input) const;

 private:
  BinaryTables *tables_ = nullptr;
  std::unordered_map<std::string, std::vector<CoreRuleMeta>> rule_meta_;
};

}  // namespace langwb::parse
