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
  bool reads_fields = false;
};

// Algebraic Data Type (Sum Type) variant and type declaration metadata.
struct AdtVariant {
  std::string type_name;    // e.g. "Option"
  std::string name;         // e.g. "Some"
  std::vector<std::string> fields;  // e.g. {"val"} (empty for nullary variants like None)
  uint32_t private_scope = 0;  // 0 = public; nonzero = visible only in this module's ScopeId
  // Where the `type` declaration names the constructor (for LSP definition).
  const char *file = nullptr;
  int line = 0;
  int col = 0;
  std::string qualified_name() const { return type_name + "::" + name; }
};

struct AdtDef {
  std::string name;
  std::string origin_module;
  // The name in the `type` declaration when an import qualified `name` to
  // `stem::Name` (a type of the same name was already visible); else empty.
  std::string declared_name;
  const std::string &unqualified_name() const {
    return declared_name.empty() ? name : declared_name;
  }
  std::vector<AdtVariant> variants;
};

// Manages frontend-injected primitives across ifa_reset() boundaries and
// bridges IFA's bare C function pointers (prim_reg) to C++23 handlers,
// with a debug-mode monotonicity check on custom transfer functions.
class PrimRegistry {
 public:
  void register_prim(std::string name, PrimitiveHandler handler);
  bool has(std::string_view name) const;
  const PrimitiveHandler *find(std::string_view name) const;

  void register_adt(AdtDef adt) {
    for (auto &existing : adts_) {
      if (existing.name == adt.name) {
        for (const auto &old_v : existing.variants) {
          variant_map_.erase(old_v.name);
        }
        for (const auto &v : adt.variants) {
          variant_map_[v.name] = v;
        }
        existing = std::move(adt);
        return;
      }
    }
    for (const auto &v : adt.variants) {
      variant_map_[v.name] = v;
    }
    adts_.push_back(std::move(adt));
  }
  void clear_adts() {
    adts_.clear();
    variant_map_.clear();
  }
  const std::vector<AdtDef> &adts() const { return adts_; }
  const AdtVariant *find_variant(std::string_view ctor_name) const {
    auto it = variant_map_.find(std::string(ctor_name));
    return (it != variant_map_.end()) ? &it->second : nullptr;
  }

  // Installs all registered primitives into the currently active IF1 instance
  // (call after ifa_init() and before ifa_analyze()).
  void install_into_if1();

  static PrimRegistry *active();
  static void set_active(PrimRegistry *reg);

 private:
  std::unordered_map<std::string, PrimitiveHandler> handlers_;
  std::vector<AdtDef> adts_;
  std::unordered_map<std::string, AdtVariant> variant_map_;
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
