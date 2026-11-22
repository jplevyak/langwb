#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "syntax/syntax.h"

namespace langwb::ifa_bridge {

// Per-contour IFA analysis facts for a single SyntaxObj node.
struct ContourAttr {
  bool is_monomorphic = false;
  bool has_const_i64 = false;
  int64_t const_i64 = 0;
  std::vector<std::string> type_names;
  std::vector<std::string> field_names;
};

// Pure C++ snapshot of IFA analysis facts for a single SyntaxObj node.
// Survives ifa_reset() so Stage-1 attribute-driven macros can inspect IFA results.
struct NodeAttr {
  bool analyzed = false;
  bool is_monomorphic = false;
  bool has_const_i64 = false;
  int64_t const_i64 = 0;
  std::vector<std::string> type_names;
  std::vector<std::string> field_names;  // populated when inferred type is a Type_RECORD
  int contour_count = 0;
  std::vector<ContourAttr> contours;
};

// Per-function contour & call-site metadata captured from IFA's EntrySet call graph.
struct FnContourInfo {
  uint64_t fn_node_id = 0;
  int contour_count = 0;
  std::vector<uint64_t> param_node_ids;
  struct CallSite {
    uint64_t call_node_id = 0;
    int contour_idx = 0;
    std::vector<uint64_t> arg_node_ids;
  };
  std::vector<CallSite> call_sites;
};

class AnalysisAttrs {
 public:
  void set(uint64_t node_id, NodeAttr attr) { attrs_[node_id] = std::move(attr); }
  void set_fn_info(uint64_t fn_node_id, FnContourInfo info) {
    fn_infos_[fn_node_id] = std::move(info);
  }
  const FnContourInfo *get_fn_info(uint64_t fn_node_id) const {
    auto it = fn_infos_.find(fn_node_id);
    return it != fn_infos_.end() ? &it->second : nullptr;
  }

  void set_ref_binder(uint64_t ref_node_id, uint64_t binder_node_id) {
    ref_to_binder_[ref_node_id] = binder_node_id;
  }
  uint64_t binder_of(uint64_t ref_node_id) const {
    auto it = ref_to_binder_.find(ref_node_id);
    return it != ref_to_binder_.end() ? it->second : 0;
  }
  void set_call_target_count(uint64_t call_node_id, int count) {
    call_target_counts_[call_node_id] = count;
  }
  int call_target_count(uint64_t call_node_id) const {
    auto it = call_target_counts_.find(call_node_id);
    return it != call_target_counts_.end() ? it->second : 0;
  }

  // Raw (unscoped) attribute lookup.
  const NodeAttr *get_raw(uint64_t node_id) const {
    auto it = attrs_.find(node_id);
    return it != attrs_.end() ? &it->second : nullptr;
  }

  // Context-aware attribute lookup: when a contour view or demand-split override
  // is active (during per-contour Stage-1 specialization), returns the active
  // contour's exact facts; otherwise returns the merged NodeAttr.
  const NodeAttr *get(uint64_t node_id) const {
    if (auto ov = active_overrides_.find(node_id); ov != active_overrides_.end()) {
      return &ov->second;
    }
    if (uint64_t b = binder_of(node_id); b != 0) {
      if (auto ov = active_overrides_.find(b); ov != active_overrides_.end()) {
        return &ov->second;
      }
    }
    const NodeAttr *raw = get_raw(node_id);
    if (!raw) return nullptr;
    if (active_contour_ >= 0 && static_cast<size_t>(active_contour_) < raw->contours.size()) {
      const ContourAttr &c = raw->contours[static_cast<size_t>(active_contour_)];
      auto &cached = contour_views_[node_id];
      cached.analyzed = raw->analyzed;
      cached.is_monomorphic = c.is_monomorphic;
      cached.has_const_i64 = c.has_const_i64;
      cached.const_i64 = c.const_i64;
      cached.type_names = c.type_names;
      cached.field_names = c.field_names;
      cached.contour_count = 1;
      cached.contours.clear();
      return &cached;
    }
    if (record_const_demands_ && !raw->is_monomorphic && !raw->field_names.empty()) {
      const_demands_.insert(node_id);
    }
    return raw;
  }

  const NodeAttr *get(const syntax::SyntaxObj *stx) const {
    return stx ? get(stx->node_id) : nullptr;
  }

  bool is_const_i64(const syntax::SyntaxObj *stx, int64_t *out = nullptr) const {
    const NodeAttr *a = get(stx);
    if (!a || !a->has_const_i64) {
      if (stx && record_const_demands_) {
        const_demands_.insert(stx->node_id);
      }
      return false;
    }
    if (out) *out = a->const_i64;
    return true;
  }

  bool is_monomorphic(const syntax::SyntaxObj *stx) const {
    const NodeAttr *a = get(stx);
    return a && a->is_monomorphic;
  }

  size_t size() const { return attrs_.size(); }

  // RAII guard activating a specific contour index and/or formal-parameter
  // constant overrides for per-contour Stage-1 specialization.
  class ScopedView {
   public:
    ScopedView(const AnalysisAttrs *attrs, int contour_idx,
               std::unordered_map<uint64_t, NodeAttr> overrides = {})
        : attrs_(attrs),
          prev_contour_(attrs->active_contour_),
          prev_overrides_(std::move(attrs->active_overrides_)),
          prev_views_(std::move(attrs->contour_views_)) {
      attrs_->active_contour_ = contour_idx;
      attrs_->active_overrides_ = std::move(overrides);
      attrs_->contour_views_.clear();
    }
    ~ScopedView() {
      attrs_->active_contour_ = prev_contour_;
      attrs_->active_overrides_ = std::move(prev_overrides_);
      attrs_->contour_views_ = std::move(prev_views_);
    }

   private:
    const AnalysisAttrs *attrs_;
    int prev_contour_;
    std::unordered_map<uint64_t, NodeAttr> prev_overrides_;
    std::unordered_map<uint64_t, NodeAttr> prev_views_;
  };

  void begin_demand_recording() const {
    record_const_demands_ = true;
    const_demands_.clear();
  }
  std::unordered_set<uint64_t> end_demand_recording() const {
    record_const_demands_ = false;
    auto out = std::move(const_demands_);
    const_demands_.clear();
    return out;
  }

 private:
  std::unordered_map<uint64_t, NodeAttr> attrs_;
  std::unordered_map<uint64_t, FnContourInfo> fn_infos_;
  std::unordered_map<uint64_t, uint64_t> ref_to_binder_;
  std::unordered_map<uint64_t, int> call_target_counts_;
  mutable int active_contour_ = -1;
  mutable std::unordered_map<uint64_t, NodeAttr> active_overrides_;
  mutable std::unordered_map<uint64_t, NodeAttr> contour_views_;
  mutable bool record_const_demands_ = false;
  mutable std::unordered_set<uint64_t> const_demands_;
};

}  // namespace langwb::ifa_bridge
