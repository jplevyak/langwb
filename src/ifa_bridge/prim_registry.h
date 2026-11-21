#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "ifa.h"

namespace langwb::ifa_bridge {

// Frontend-injected primitive specification backed by C++23 callables.
struct PrimitiveHandler {
  std::function<void(PNode *, EntrySet *)> transfer;
  std::function<void(PNode *, EntrySet *)> check;
  std::function<void(FILE *, PNode *, Fun *)> emit_c;
  std::function<void(PNode *, Fun *)> emit_llvm;
  bool is_functional = true;
  bool is_visible = false;
};

// Manages frontend-injected primitives across ifa_reset() boundaries and
// bridges IFA's bare C function pointers (prim_reg) to C++23 handlers,
// with a debug-mode monotonicity check on custom transfer functions.
class PrimRegistry {
 public:
  void register_prim(std::string name, PrimitiveHandler handler);
  bool has(std::string_view name) const;
  const PrimitiveHandler *find(std::string_view name) const;

  // Installs all registered primitives into the currently active IF1 instance
  // (call after ifa_init() and before ifa_analyze()).
  void install_into_if1();

  static PrimRegistry *active();
  static void set_active(PrimRegistry *reg);

 private:
  std::unordered_map<std::string, PrimitiveHandler> handlers_;
};

// Convenience helpers for writing custom IFA transfer functions.
// Note: for sym_primitive SENDs, rvals[0] = sym_primitive, rvals[1] = name,
// and user arguments start at rvals[2].
AVar *prim_arg_avar(PNode *pn, EntrySet *es, int arg_index);
AVar *prim_result_avar(PNode *pn, EntrySet *es, int res_index = 0);
void prim_emit_result(PNode *pn, EntrySet *es, AType *t, int res_index = 0);

// Extracts a compile-time constant int64 if `av->out` is a singleton constant
// integer CreationSet.
bool try_get_const_i64(AVar *av, int64_t *out);

// Constructs an AType representing a singleton constant int64 value.
AType *make_const_i64_type(int64_t value);

}  // namespace langwb::ifa_bridge
