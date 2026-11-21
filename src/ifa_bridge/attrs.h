#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "syntax/syntax.h"

namespace langwb::ifa_bridge {

// Pure C++ snapshot of IFA analysis facts for a single SyntaxObj node.
// Survives ifa_reset() so Stage-1 attribute-driven macros can inspect IFA results.
struct NodeAttr {
  bool analyzed = false;
  bool is_monomorphic = false;
  bool has_const_i64 = false;
  int64_t const_i64 = 0;
  std::vector<std::string> type_names;
  int contour_count = 0;
};

class AnalysisAttrs {
 public:
  void set(uint64_t node_id, NodeAttr attr) { attrs_[node_id] = std::move(attr); }

  const NodeAttr *get(uint64_t node_id) const {
    auto it = attrs_.find(node_id);
    return it != attrs_.end() ? &it->second : nullptr;
  }

  const NodeAttr *get(const syntax::SyntaxObj *stx) const {
    return stx ? get(stx->node_id) : nullptr;
  }

  bool is_const_i64(const syntax::SyntaxObj *stx, int64_t *out = nullptr) const {
    const NodeAttr *a = get(stx);
    if (!a || !a->has_const_i64) return false;
    if (out) *out = a->const_i64;
    return true;
  }

  bool is_monomorphic(const syntax::SyntaxObj *stx) const {
    const NodeAttr *a = get(stx);
    return a && a->is_monomorphic;
  }

  size_t size() const { return attrs_.size(); }

 private:
  std::unordered_map<uint64_t, NodeAttr> attrs_;
};

}  // namespace langwb::ifa_bridge
