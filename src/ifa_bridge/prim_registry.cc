#include "ifa_bridge/prim_registry.h"

#include <cassert>
#include <cstdio>
#include <vector>

namespace langwb::ifa_bridge {

namespace {

PrimRegistry *g_active_registry = nullptr;

cchar *prim_name_of(PNode *pn) {
  if (!pn || pn->rvals.n < 2 || !pn->rvals.v[1] || !pn->rvals.v[1]->sym) return nullptr;
  cchar *name = pn->rvals.v[1]->sym->name;
  if (!name) name = pn->rvals.v[1]->sym->constant;
  return name;
}

const PrimitiveHandler *lookup_handler(PNode *pn) {
  if (!g_active_registry) return nullptr;
  cchar *name = prim_name_of(pn);
  if (!name) return nullptr;
  return g_active_registry->find(name);
}

#ifdef DEBUG
// Verify that every CreationSet previously in `before` is still present in `after`.
bool is_monotone_extension(AType *before, AType *after) {
  if (!before || before == fa->type_world.bottom_type) return true;
  if (!after) return false;
  for (int i = 0; i < before->n; ++i) {
    if (!after->set_in(before->v[i])) return false;
  }
  return true;
}
#endif

void trampoline_tfn(PNode *pn, EntrySet *es) {
  const PrimitiveHandler *h = lookup_handler(pn);
  if (!h || !h->transfer) return;

#ifdef DEBUG
  std::vector<AType *> before;
  before.reserve(static_cast<size_t>(pn->lvals.n));
  for (int i = 0; i < pn->lvals.n; ++i) {
    AVar *av = make_AVar(pn->lvals[i], es);
    before.push_back(av ? av->out : nullptr);
  }
#endif

  h->transfer(pn, es);

#ifdef DEBUG
  for (int i = 0; i < pn->lvals.n; ++i) {
    AVar *av = make_AVar(pn->lvals[i], es);
    AType *after = av ? av->out : nullptr;
    if (!is_monotone_extension(before[static_cast<size_t>(i)], after)) {
      std::fprintf(stderr, "langwb: non-monotonic transfer function for primitive '%s'\n",
                   prim_name_of(pn) ? prim_name_of(pn) : "<unknown>");
      assert(!"non-monotonic primitive transfer function");
    }
  }
#endif
}

void trampoline_check(PNode *pn, EntrySet *es) {
  const PrimitiveHandler *h = lookup_handler(pn);
  if (h && h->check) {
    h->check(pn, es);
  }
}

void trampoline_cg(FILE *fp, PNode *pn, Fun *f) {
  const PrimitiveHandler *h = lookup_handler(pn);
  if (h && h->emit_c) {
    h->emit_c(fp, pn, f);
  }
}

void trampoline_llvm_cg(PNode *pn, Fun *f) {
  const PrimitiveHandler *h = lookup_handler(pn);
  if (h && h->emit_llvm) {
    h->emit_llvm(pn, f);
  }
}

}  // namespace

void PrimRegistry::register_prim(std::string name, PrimitiveHandler handler) {
  handlers_[std::move(name)] = std::move(handler);
}

bool PrimRegistry::has(std::string_view name) const {
  return handlers_.find(std::string(name)) != handlers_.end();
}

const PrimitiveHandler *PrimRegistry::find(std::string_view name) const {
  auto it = handlers_.find(std::string(name));
  return it != handlers_.end() ? &it->second : nullptr;
}

void PrimRegistry::install_into_if1() {
  set_active(this);
  for (const auto &[name, handler] : handlers_) {
    cchar *interned = if1_cannonicalize_string(if1, name.c_str());
    PrimitiveTransferFunctionPtr tfn = handler.transfer ? &trampoline_tfn : nullptr;
    PrimitiveCGPtr cgfn = handler.emit_c ? &trampoline_cg : nullptr;
    PrimitiveLLVMCGPtr llvm_cgfn = handler.emit_llvm ? &trampoline_llvm_cg : nullptr;
    RegisteredPrim *rp = prim_reg(interned, tfn, cgfn, llvm_cgfn);
    if (handler.check) {
      rp->check_fn = &trampoline_check;
    }
    rp->is_functional = handler.is_functional ? 1 : 0;
    rp->is_visible = handler.is_visible ? 1 : 0;
    rp->reads_fields = handler.reads_fields ? 1 : 0;
  }
}

PrimRegistry *PrimRegistry::active() { return g_active_registry; }

void PrimRegistry::set_active(PrimRegistry *reg) { g_active_registry = reg; }

AVar *prim_arg_avar(PNode *pn, EntrySet *es, int arg_index) {
  int idx = 2 + arg_index;
  if (!pn || idx < 0 || idx >= pn->rvals.n || !pn->rvals[idx]) return nullptr;
  return make_AVar(pn->rvals[idx], es);
}

AVar *prim_result_avar(PNode *pn, EntrySet *es, int res_index) {
  if (!pn || res_index < 0 || res_index >= pn->lvals.n || !pn->lvals[res_index]) return nullptr;
  return make_AVar(pn->lvals[res_index], es);
}

void prim_emit_result(PNode *pn, EntrySet *es, AType *t, int res_index) {
  if (!t) return;
  if (AVar *res = prim_result_avar(pn, es, res_index)) {
    update_gen(res, t);
  }
}

bool try_get_const_i64(AVar *av, int64_t *out) {
  if (!av || !av->out || av->out->n != 1 || !out) return false;
  CreationSet *cs = av->out->v[0];
  if (!cs || !cs->sym || !cs->sym->is_constant) return false;
  if (cs->sym->imm.const_kind == IF1_NUM_KIND_INT) {
    *out = cs->sym->imm.v_int64;
    return true;
  }
  return false;
}

AType *make_const_i64_type(int64_t value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
  Sym *c = if1_const(if1, sym_int64, buf);
  return make_abstract_type(c);
}

}  // namespace langwb::ifa_bridge
