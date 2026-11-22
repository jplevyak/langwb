#include "workbench/workbench.h"

#include <cstdio>
#include <fcntl.h>
#include <set>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace langwb {

namespace {

void dump_attrs_tree_local(const syntax::SyntaxObj *node, const ifa_bridge::AnalysisAttrs &attrs,
                           FILE *fp, int indent = 0) {
  if (!node) return;
  const auto *a = attrs.get(node);
  if (a && a->analyzed) {
    for (int i = 0; i < indent; ++i) std::fputc(' ', fp);
    std::fprintf(fp, "%s \"%s\" [id=%llu", node->symbol.c_str(), node->text.c_str(),
                 static_cast<unsigned long long>(node->node_id));
    if (!a->type_names.empty()) {
      std::fprintf(fp, " type=");
      for (size_t i = 0; i < a->type_names.size(); ++i) {
        if (i > 0) std::fputc('|', fp);
        std::fprintf(fp, "%s", a->type_names[i].c_str());
      }
    }
    if (a->has_const_i64) {
      std::fprintf(fp, " const=%lld", static_cast<long long>(a->const_i64));
    }
    if (!a->field_names.empty()) {
      std::fprintf(fp, " fields={");
      for (size_t i = 0; i < a->field_names.size(); ++i) {
        if (i > 0) std::fputc(',', fp);
        std::fprintf(fp, "%s", a->field_names[i].c_str());
      }
      std::fputc('}', fp);
    }
    if (a->contour_count > 1) {
      std::fprintf(fp, " ctr=%d", a->contour_count);
    }
    std::fprintf(fp, "]\n");
  }
  for (const syntax::SyntaxObj *c : node->children) {
    dump_attrs_tree_local(c, attrs, fp, indent + 2);
  }
}

// Clones a SyntaxObj subtree assigning fresh `node_id`s so a subsequent IFA
// analysis pass maps each clone's nodes independently in AnalysisAttrs.
syntax::SyntaxObj *clone_fresh_ids(const syntax::SyntaxObj *node) {
  if (!node) return nullptr;
  syntax::SyntaxObj *out = node->clone();
  auto assign_ids = [](auto &self, syntax::SyntaxObj *n) -> void {
    if (!n) return;
    n->node_id = syntax::SyntaxObj::next_node_id();
    for (syntax::SyntaxObj *c : n->children) self(self, c);
  };
  assign_ids(assign_ids, out);
  return out;
}

// Canonical alpha-equivalent signature of an expanded SyntaxObj (normalizing
// ScopeIds by order of first appearance) for decision-keyed contour grouping.
void append_syntax_sig(const syntax::SyntaxObj *n,
                       std::unordered_map<syntax::ScopeId, uint32_t> &scope_norm,
                       std::string &out) {
  if (!n) {
    out += "null;";
    return;
  }
  out += n->symbol;
  out.push_back(':');
  if (n->is_terminal) {
    out += n->text;
    out.push_back('{');
    const auto &raw = n->scopes.raw();
    for (size_t i = 0; i < raw.size(); ++i) {
      if (i > 0) out.push_back(',');
      auto [it, ins] = scope_norm.emplace(raw[i], static_cast<uint32_t>(scope_norm.size()));
      out += std::to_string(it->second);
    }
    out.push_back('}');
  } else {
    out.push_back('(');
    for (const syntax::SyntaxObj *c : n->children) {
      append_syntax_sig(c, scope_norm, out);
      out.push_back(',');
    }
    out.push_back(')');
  }
}

std::string syntax_sig(const syntax::SyntaxObj *n) {
  std::string out;
  std::unordered_map<syntax::ScopeId, uint32_t> scope_norm;
  append_syntax_sig(n, scope_norm, out);
  return out;
}

syntax::SyntaxObj *unwrap_lambda_stx(syntax::SyntaxObj *n) {
  while (n) {
    if (n->core.kind == parse::CoreFormKind::Lambda) return n;
    if (n->core.kind == parse::CoreFormKind::Paren && n->core.inner >= 0 &&
        static_cast<size_t>(n->core.inner) < n->children.size()) {
      n = n->children[static_cast<size_t>(n->core.inner)];
      continue;
    }
    if (n->core.kind == parse::CoreFormKind::PassThrough && n->children.size() == 1) {
      n = n->children[0];
      continue;
    }
    break;
  }
  return nullptr;
}

struct CloneTarget {
  std::string name;
  syntax::ScopeSet scopes;  // fresh hygienic ScopeSet containing ONLY the clone's ScopeId
};

// Rewrites App nodes whose node_id is in `redirects` to call the specialized clone
// identifier using the clone's fresh ScopeSet (cannot be captured by user bindings).
syntax::SyntaxObj *rewrite_redirected_calls(
    syntax::SyntaxObj *node, const std::unordered_map<uint64_t, CloneTarget> &redirects) {
  if (!node || redirects.empty()) return node;
  if (node->core.kind == parse::CoreFormKind::App && node->core.fn >= 0 &&
      static_cast<size_t>(node->core.fn) < node->children.size()) {
    if (auto it = redirects.find(node->node_id); it != redirects.end()) {
      syntax::SyntaxObj *out = node->clone();
      size_t fn_idx = static_cast<size_t>(out->core.fn);
      syntax::SyntaxObj *fn_node = out->children[fn_idx]->with_scopes(it->second.scopes);
      fn_node->text = it->second.name;
      syntax::SyntaxObj *leaf = fn_node;
      while (leaf && !leaf->is_terminal && leaf->children.size() == 1) {
        leaf = leaf->children[0];
        leaf->text = it->second.name;
      }
      out->children[fn_idx] = fn_node;
      for (size_t i = 0; i < out->children.size(); ++i) {
        if (i == fn_idx) continue;
        out->children[i] = rewrite_redirected_calls(out->children[i], redirects);
      }
      return out;
    }
  }
  syntax::SyntaxObj *out = node->clone();
  for (size_t i = 0; i < out->children.size(); ++i) {
    out->children[i] = rewrite_redirected_calls(out->children[i], redirects);
  }
  return out;
}

// Checks whether `node` still contains any identifier referencing `name` whose
// scopes are a superset of `binder_scopes`.
bool has_residual_ref(const syntax::SyntaxObj *node, const std::string &name,
                      const syntax::ScopeSet &binder_scopes) {
  if (!node) return false;
  if (node->is_terminal) {
    return node->text == name && binder_scopes.is_subset_of(node->scopes);
  }
  for (const syntax::SyntaxObj *c : node->children) {
    if (has_residual_ref(c, name, binder_scopes)) return true;
  }
  return false;
}

// Builds a synthetic `let <clone_name> = <rhs> in <body>` node using `clone_scopes`
// on the clone binder (so it cannot collide with or be captured by user bindings).
syntax::SyntaxObj *make_clone_let(const syntax::SyntaxObj *let_tmpl, const std::string &clone_name,
                                  const syntax::ScopeSet &clone_scopes, syntax::SyntaxObj *rhs,
                                  syntax::SyntaxObj *body) {
  syntax::SyntaxObj *out = let_tmpl->clone();
  out->node_id = syntax::SyntaxObj::next_node_id();
  size_t bi = static_cast<size_t>(out->core.bind);
  size_t ri = static_cast<size_t>(out->core.rhs);
  size_t bodyi = static_cast<size_t>(out->core.body);
  out->children[bi] = out->children[bi]->with_scopes(clone_scopes);
  out->children[bi]->text = clone_name;
  syntax::SyntaxObj *leaf = out->children[bi];
  while (leaf && !leaf->is_terminal && leaf->children.size() == 1) {
    leaf = leaf->children[0];
    leaf->text = clone_name;
  }
  out->children[ri] = rhs;
  out->children[bodyi] = body;
  return out;
}

}  // namespace

syntax::SyntaxObj *Workbench::expand_stage1_contour_aware(syntax::SyntaxObj *node,
                                                          const parse::DynamicGrammar &gram,
                                                          syntax::ScopeAllocator &scopes,
                                                          const ifa_bridge::AnalysisAttrs &attrs,
                                                          bool allow_demand_reanalyze,
                                                          bool *out_did_reanalyze_split) {
  if (!node) return nullptr;
  using parse::CoreFormKind;
  static constexpr size_t kMaxValueSplits = 8;

  // When `allow_demand_reanalyze` is true, we are in the pre-reanalysis split pass:
  // look for `let f = fn(params) => body` with >= 2 exclusive call sites passing
  // distinct constant argument tuples, clone `lam` (with fresh node_ids) per constant
  // tuple so the next IFA pass propagates constants through `n + 1`, `g(n)`, etc.!
  if (allow_demand_reanalyze) {
    if (node->core.kind == CoreFormKind::Let && node->core.bind >= 0 && node->core.rhs >= 0 &&
        node->core.body >= 0) {
      size_t bi = static_cast<size_t>(node->core.bind);
      size_t ri = static_cast<size_t>(node->core.rhs);
      size_t bodyi = static_cast<size_t>(node->core.body);
      if (bi < node->children.size() && ri < node->children.size() &&
          bodyi < node->children.size()) {
        syntax::SyntaxObj *lam = unwrap_lambda_stx(node->children[ri]);
        const ifa_bridge::FnContourInfo *finfo = lam ? attrs.get_fn_info(lam->node_id) : nullptr;
        if (lam && finfo && finfo->call_sites.size() >= 2 && !finfo->param_node_ids.empty()) {
          std::string orig_name = node->children[bi]->text;
          const syntax::ScopeSet orig_binder_scopes = node->children[bi]->scopes;

          // Group exclusive call sites by their tuple of compile-time constant arguments.
          std::vector<std::string> tuple_keys;
          std::vector<CloneTarget> clones;
          std::unordered_map<uint64_t, CloneTarget> redirects;

          for (const auto &cs : finfo->call_sites) {
            if (attrs.call_target_count(cs.call_node_id) != 1) continue;
            std::string key;
            bool any_const = false;
            for (size_t pi = 0; pi < finfo->param_node_ids.size() && pi < cs.arg_node_ids.size();
                 ++pi) {
              const auto *aa = attrs.get_raw(cs.arg_node_ids[pi]);
              if (aa && aa->has_const_i64) {
                any_const = true;
                key += "C:" + std::to_string(aa->const_i64) + ";";
              } else {
                key += "D;";
              }
            }
            if (!any_const) continue;

            size_t gid = tuple_keys.size();
            for (size_t g = 0; g < tuple_keys.size(); ++g) {
              if (tuple_keys[g] == key) {
                gid = g;
                break;
              }
            }
            if (gid == tuple_keys.size()) {
              if (tuple_keys.size() >= kMaxValueSplits) continue;
              tuple_keys.push_back(std::move(key));
              syntax::ScopeSet cscope;
              cscope.add(scopes.fresh());
              clones.push_back(CloneTarget{
                  .name = "__c_" + orig_name + "_" + std::to_string(gid), .scopes = cscope});
            }
            redirects[cs.call_node_id] = clones[gid];
          }

          if (clones.size() >= 2 || (!clones.empty() && redirects.size() < finfo->call_sites.size())) {
            syntax::SyntaxObj *body_rewritten =
                rewrite_redirected_calls(node->children[bodyi], redirects);
            bool need_default = has_residual_ref(body_rewritten, orig_name, orig_binder_scopes);
            syntax::SyntaxObj *exp_body = expand_stage1_contour_aware(
                body_rewritten, gram, scopes, attrs, true, out_did_reanalyze_split);
            if (!exp_body) return nullptr;

            syntax::SyntaxObj *cur =
                need_default
                    ? make_clone_let(node, orig_name, orig_binder_scopes,
                                     node->children[ri]->clone(), exp_body)
                    : exp_body;
            for (size_t g = clones.size(); g-- > 0;) {
              cur = make_clone_let(node, clones[g].name, clones[g].scopes,
                                   clone_fresh_ids(node->children[ri]), cur);
            }
            stats_->contour_clones += static_cast<int>(clones.size());
            if (out_did_reanalyze_split) *out_did_reanalyze_split = true;
            return cur;
          }
        }
      }
    }

    syntax::SyntaxObj *out = node->clone();
    for (size_t i = 0; i < out->children.size(); ++i) {
      if (out->children[i] && !out->children[i]->is_terminal) {
        out->children[i] = expand_stage1_contour_aware(
            out->children[i], gram, scopes, attrs, true, out_did_reanalyze_split);
        if (!out->children[i]) return nullptr;
      }
    }
    return out;
  }

  // Normal Stage-1 pass: head-first macro expansion (so discarded branches like
  // `if_const(1, 42, sum_fields(0))` are never expanded).
  syntax::SyntaxObj *head =
      stage1_.expand_head(node, gram, scopes, &attrs, 0, 64, &grammar_def_);
  if (!head) return nullptr;
  if (head != node) {
    return expand_stage1_contour_aware(head, gram, scopes, attrs, false, nullptr);
  }

  // Check for `let f = fn(params) => body in rest` where IFA split `f` into
  // multiple type/record contours (Case A) or a direct formal constant demand
  // can be specialized in-place (Case B).
  if (node->core.kind == CoreFormKind::Let && node->core.bind >= 0 && node->core.rhs >= 0 &&
      node->core.body >= 0) {
    size_t bi = static_cast<size_t>(node->core.bind);
    size_t ri = static_cast<size_t>(node->core.rhs);
    size_t bodyi = static_cast<size_t>(node->core.body);
    if (bi < node->children.size() && ri < node->children.size() && bodyi < node->children.size()) {
      syntax::SyntaxObj *lam = unwrap_lambda_stx(node->children[ri]);
      const ifa_bridge::FnContourInfo *finfo = lam ? attrs.get_fn_info(lam->node_id) : nullptr;
      if (lam && finfo && !finfo->call_sites.empty()) {
        std::string orig_name = node->children[bi]->text;
        const syntax::ScopeSet orig_binder_scopes = node->children[bi]->scopes;

        // Case A: IFA already split `f` into K > 1 contours.
        if (finfo->contour_count > 1) {
          std::vector<std::string> group_sigs;
          std::vector<syntax::SyntaxObj *> group_lams;
          std::unordered_map<int, size_t> contour_to_group;
          bool all_ok = true;

          for (int ci = 0; ci < finfo->contour_count; ++ci) {
            ifa_bridge::AnalysisAttrs::ScopedView view(&attrs, ci);
            syntax::SyntaxObj *exp_lam =
                expand_stage1_contour_aware(lam->clone(), gram, scopes, attrs, false, nullptr);
            if (!exp_lam) {
              all_ok = false;
              break;
            }
            std::string sig = syntax_sig(exp_lam);
            size_t gid = group_sigs.size();
            for (size_t g = 0; g < group_sigs.size(); ++g) {
              if (group_sigs[g] == sig) {
                gid = g;
                break;
              }
            }
            if (gid == group_sigs.size()) {
              group_sigs.push_back(std::move(sig));
              group_lams.push_back(exp_lam);
            }
            contour_to_group[ci] = gid;
          }

          if (all_ok && group_lams.size() > 1) {
            std::unordered_map<uint64_t, std::set<size_t>> call_to_groups;
            for (const auto &cs : finfo->call_sites) {
              if (attrs.call_target_count(cs.call_node_id) != 1) continue;
              if (auto it = contour_to_group.find(cs.contour_idx); it != contour_to_group.end()) {
                call_to_groups[cs.call_node_id].insert(it->second);
              }
            }

            std::vector<CloneTarget> clones;
            clones.reserve(group_lams.size());
            for (size_t g = 0; g < group_lams.size(); ++g) {
              syntax::ScopeSet cs;
              cs.add(scopes.fresh());
              clones.push_back(
                  CloneTarget{.name = "__c_" + orig_name + "_" + std::to_string(g), .scopes = cs});
            }

            std::unordered_map<uint64_t, CloneTarget> redirects;
            for (const auto &[cid, groups] : call_to_groups) {
              if (groups.size() == 1) {
                redirects[cid] = clones[*groups.begin()];
              }
            }

            if (!redirects.empty()) {
              syntax::SyntaxObj *body_rewritten =
                  rewrite_redirected_calls(node->children[bodyi], redirects);
              bool need_default = has_residual_ref(body_rewritten, orig_name, orig_binder_scopes);
              syntax::SyntaxObj *default_lam = nullptr;
              if (need_default) {
                default_lam =
                    expand_stage1_contour_aware(lam->clone(), gram, scopes, attrs, false, nullptr);
              }
              if (!need_default || default_lam) {
                syntax::SyntaxObj *exp_body = expand_stage1_contour_aware(
                    body_rewritten, gram, scopes, attrs, false, nullptr);
                if (!exp_body) return nullptr;

                syntax::SyntaxObj *cur =
                    need_default
                        ? make_clone_let(node, orig_name, orig_binder_scopes, default_lam, exp_body)
                        : exp_body;
                for (size_t g = group_lams.size(); g-- > 0;) {
                  cur = make_clone_let(node, clones[g].name, clones[g].scopes, group_lams[g], cur);
                }
                stats_->contour_clones += static_cast<int>(group_lams.size());
                return cur;
              }
            }
          }
        }
      }
    }
  }

  syntax::SyntaxObj *out = node->clone();
  for (size_t i = 0; i < out->children.size(); ++i) {
    if (out->children[i] && !out->children[i]->is_terminal) {
      out->children[i] =
          expand_stage1_contour_aware(out->children[i], gram, scopes, attrs, false, nullptr);
      if (!out->children[i]) return nullptr;
    }
  }
  return out;
}

namespace {

struct QuietStderr {
  int saved_fd = -1;
  QuietStderr() {
    std::fflush(stderr);
    int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      saved_fd = ::dup(STDERR_FILENO);
      if (saved_fd >= 0) ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
  }
  ~QuietStderr() {
    if (saved_fd >= 0) {
      std::fflush(stderr);
      ::dup2(saved_fd, STDERR_FILENO);
      ::close(saved_fd);
    }
  }
};

const syntax::SyntaxObj *find_first_amb(const syntax::SyntaxObj *node) {
  if (!node) return nullptr;
  if (node->core.kind == parse::CoreFormKind::Amb) return node;
  for (const syntax::SyntaxObj *c : node->children) {
    if (const syntax::SyntaxObj *found = find_first_amb(c)) return found;
  }
  return nullptr;
}

syntax::SyntaxObj *replace_node_by_id(syntax::SyntaxObj *node, uint64_t target_id,
                                      syntax::SyntaxObj *replacement) {
  if (!node) return nullptr;
  if (node->node_id == target_id) return replacement;
  syntax::SyntaxObj *out = node->clone();
  for (size_t i = 0; i < out->children.size(); ++i) {
    out->children[i] = replace_node_by_id(out->children[i], target_id, replacement);
  }
  return out;
}

std::string summarize_alt(const syntax::SyntaxObj *alt) {
  if (!alt) return "<null>";
  const syntax::SyntaxObj *u = alt;
  while (u && u->core.kind == parse::CoreFormKind::PassThrough && u->children.size() == 1) {
    u = u->children[0];
  }
  if (!u) return alt->symbol;
  if (u->core.kind != parse::CoreFormKind::None) {
    return std::string(parse::core_form_name(u->core.kind)) + " `" + u->text + "`";
  }
  return u->symbol + " `" + u->text + "`";
}

// Tier-3 (Analysis-tier) GLR ambiguity disambiguation:
// For each remaining CoreFormKind::Amb node, test each alternative under IFA.
// If exactly one alternative type-checks cleanly, select it; otherwise report a
// clear diagnostic naming the conflicting alternatives and fail.
syntax::SyntaxObj *resolve_ambs_tier3(syntax::SyntaxObj *root, ifa_bridge::PrimRegistry *prims,
                                      int depth = 0) {
  if (!root || depth > 8) return root;
  const syntax::SyntaxObj *amb = find_first_amb(root);
  if (!amb) return root;

  std::vector<syntax::SyntaxObj *> valid_trees;
  std::vector<const syntax::SyntaxObj *> valid_alts;
  for (syntax::SyntaxObj *alt : amb->children) {
    syntax::SyntaxObj *cand = replace_node_by_id(root, amb->node_id, alt);
    // Recursively resolve any remaining Amb nodes in this candidate branch.
    if (find_first_amb(cand)) {
      QuietStderr quiet;
      cand = resolve_ambs_tier3(cand, prims, depth + 1);
      if (!cand) continue;
    }
    ifa_bridge::AnalysisAttrs probe_attrs;
    bool ok = false;
    {
      QuietStderr quiet;
      ok = ifa_bridge::analyze_syntax(cand, prims, &probe_attrs);
    }
    if (ok) {
      valid_trees.push_back(cand);
      valid_alts.push_back(alt);
    }
  }

  if (valid_trees.size() == 1) {
    return valid_trees[0];
  }

  std::fprintf(stderr,
               "<input>:%d:%d: error: unresolved syntax ambiguity for '%s' (%zu of %zu "
               "alternatives valid):\n",
               amb->line, amb->col, amb->text.c_str(), valid_alts.size(), amb->children.size());
  if (!valid_alts.empty()) {
    for (size_t i = 0; i < valid_alts.size(); ++i) {
      std::fprintf(stderr, "  alternative %zu: %s\n", i + 1, summarize_alt(valid_alts[i]).c_str());
    }
  } else {
    for (size_t i = 0; i < amb->children.size(); ++i) {
      std::fprintf(stderr, "  alternative %zu: %s\n", i + 1,
                   summarize_alt(amb->children[i]).c_str());
    }
  }
  return nullptr;
}

}  // namespace

bool Workbench::prepare_staged_syntax(syntax::SyntaxObj *stx0, const parse::DynamicGrammar &gram,
                                      syntax::ScopeAllocator &scopes, const EvalOptions &opts,
                                      syntax::SyntaxObj **out_final,
                                      std::vector<std::string> *out_top_fields) {
  if (find_first_amb(stx0)) {
    stx0 = resolve_ambs_tier3(stx0, &prims_);
    if (!stx0) return false;
  }

  syntax::SyntaxObj *final_stx = stx0;
  if (opts.staged) {
    static constexpr int kMaxFixpointRounds = 4;
    syntax::SyntaxObj *cur = stx0;
    for (int round = 0; round < kMaxFixpointRounds; ++round) {
      ifa_bridge::AnalysisAttrs attrs;
      if (!ifa_bridge::analyze_syntax(cur, &prims_, &attrs)) return false;
      if (opts.dump_attrs && round == 0) {
        std::fprintf(opts.diag_fp, "=== IFA AnalysisAttrs ===\n");
        dump_attrs_tree_local(cur, attrs, opts.diag_fp);
      }

      attrs.begin_demand_recording();
      syntax::SyntaxObj *expanded =
          expand_stage1_contour_aware(cur, gram, scopes, attrs, /*allow_demand_reanalyze=*/false);
      auto demands = attrs.end_demand_recording();
      if (!expanded) return false;

      if (demands.empty() || round + 1 >= kMaxFixpointRounds) {
        final_stx = expanded;
        if (out_top_fields) {
          if (const auto *a = attrs.get(cur); a && !a->field_names.empty()) {
            *out_top_fields = a->field_names;
          }
        }
        break;
      }

      // A Stage-1 macro demanded a compile-time constant that was not yet known.
      // Clone functions called with distinct constant argument tuples and re-run
      // IFA so constants propagate through expressions (`n + 1`) and nested calls (`g(n)`).
      bool did_split = false;
      syntax::SyntaxObj *split_tree = expand_stage1_contour_aware(
          cur, gram, scopes, attrs, /*allow_demand_reanalyze=*/true, &did_split);
      if (!split_tree || !did_split) {
        final_stx = expanded;
        if (out_top_fields) {
          if (const auto *a = attrs.get(cur); a && !a->field_names.empty()) {
            *out_top_fields = a->field_names;
          }
        }
        break;
      }
      cur = split_tree;
    }
  } else if (out_top_fields) {
    ifa_bridge::AnalysisAttrs attrs;
    if (ifa_bridge::analyze_syntax(final_stx, &prims_, &attrs)) {
      if (const auto *a = attrs.get(final_stx); a && !a->field_names.empty()) {
        *out_top_fields = a->field_names;
      }
    }
  }

  if (opts.dump_expanded) {
    std::fprintf(opts.diag_fp, "=== Expanded SyntaxObj ===\n");
    syntax::dump_syntax(final_stx, opts.diag_fp);
  }
  *out_final = final_stx;
  return true;
}

bool Workbench::run_staged_and_jit(syntax::SyntaxObj *stx0, const parse::DynamicGrammar &gram,
                                   syntax::ScopeAllocator &scopes, int64_t *out_i64,
                                   ifa_bridge::EvalValue *out_value, const EvalOptions &opts,
                                   ifa_bridge::CompileInfo *out_info) {
  syntax::SyntaxObj *final_stx = nullptr;
  std::vector<std::string> top_fields;
  if (!prepare_staged_syntax(stx0, gram, scopes, opts, &final_stx,
                             out_value ? &top_fields : nullptr)) {
    return false;
  }
  if (out_value) {
    return ifa_bridge::compile_and_jit_value(final_stx, out_value, &prims_, out_info,
                                             top_fields.empty() ? nullptr : &top_fields);
  }
  return ifa_bridge::compile_and_jit_i64(final_stx, out_i64, &prims_, out_info);
}

}  // namespace langwb
