#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
  // The module scopes whose code can name the constructor (FL-15): its
  // defining module's, plus each importer's along import edges that export
  // or re-export it. Empty = public (declared in the entry module or a
  // `.lang`).
  std::vector<uint32_t> visible_in;
  // What the defining module exports to make it visible ('Name' for the
  // type, or the constructor's own name); empty when it is private.
  std::string export_name;
  template <class Scopes>
  bool visible_to(const Scopes &scopes) const {
    if (visible_in.empty()) return true;
    for (uint32_t s : visible_in) {
      if (scopes.contains(s)) return true;
    }
    return false;
  }
  void add_visible_in(uint32_t s) {
    for (uint32_t have : visible_in) {
      if (have == s) return;
    }
    visible_in.push_back(s);
  }
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

  // Registers `adt`, replacing a type of the same name from the same module
  // (a redeclaration). Two same-named types from different modules are both
  // kept: the imported one is qualified to `stem::Name` (the incoming one when
  // both are imported), so declaring a type after importing a module with a
  // type of that name is the same as declaring it before.
  void register_adt(AdtDef adt) {
    AdtDef *clash = nullptr;
    for (auto &existing : adts_) {
      if (existing.name != adt.name) continue;
      if (existing.origin_module == adt.origin_module) {
        for (const auto &old_v : existing.variants) {
          variant_map_.erase(old_v.name);
        }
        for (const auto &v : adt.variants) {
          variant_map_[v.name] = v;
        }
        existing = std::move(adt);
        return;
      }
      clash = &existing;
    }
    if (clash) qualify(adt.origin_module.empty() ? *clash : adt);
    for (const auto &v : adt.variants) {
      variant_map_[v.name] = v;
    }
    adts_.push_back(std::move(adt));
  }
  // The type named `name` (as qualified, if it was), or null.
  const AdtDef *find_adt(std::string_view name) const {
    for (const auto &adt : adts_) {
      if (adt.name == name) return &adt;
    }
    return nullptr;
  }
  // `stem::Name` for a type `Name` of module `origin_module` (`mod::Name`
  // when the module has no path).
  static std::string qualified_type_name(const std::string &origin_module,
                                         const std::string &name) {
    std::string stem = origin_module;
    if (size_t slash = stem.find_last_of('/'); slash != std::string::npos) stem.erase(0, slash + 1);
    if (size_t dot = stem.find_last_of('.'); dot != std::string::npos && dot > 0) stem.erase(dot);
    if (stem.empty()) stem = "mod";
    return stem + "::" + name;
  }
  void clear_adts() {
    adts_.clear();
    variant_map_.clear();
  }
  const std::vector<AdtDef> &adts() const { return adts_; }
  // Adds `scopes` to the visibility of constructor `ctor_name` (both copies:
  // the one in its `AdtDef` and the by-name lookup).
  void add_variant_visibility(std::string_view ctor_name, const std::vector<uint32_t> &scopes) {
    auto it = variant_map_.find(std::string(ctor_name));
    if (it == variant_map_.end() || it->second.visible_in.empty()) return;  // public
    for (uint32_t s : scopes) it->second.add_visible_in(s);
    for (auto &adt : adts_) {
      for (auto &v : adt.variants) {
        if (v.name == ctor_name) {
          for (uint32_t s : scopes) v.add_visible_in(s);
        }
      }
    }
  }
  // Qualifies `adt` (and its registered constructors) to `stem::Name`.
  void qualify(AdtDef &adt) {
    if (!adt.declared_name.empty()) return;
    adt.declared_name = adt.name;
    adt.name = qualified_type_name(adt.origin_module, adt.declared_name);
    for (auto &v : adt.variants) {
      v.type_name = adt.name;
      if (auto it = variant_map_.find(v.name);
          it != variant_map_.end() && it->second.type_name == adt.declared_name) {
        it->second.type_name = adt.name;
      }
    }
  }
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
