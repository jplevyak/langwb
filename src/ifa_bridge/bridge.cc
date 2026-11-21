#include "ifa_bridge/bridge.h"

namespace langwb::ifa_bridge {

IFASymbol *WbSymbol::copy() {
  auto *s = new WbSymbol();
  s->filename = filename;
  s->line_no = line_no;
  s->col_no = col_no;
  s->sym = new Sym();
  s->sym->asymbol = s;
  return s;
}

IFAAST *WbAST::copy_tree(ASTCopyContext *context) { return copy_node(context); }

IFAAST *WbAST::copy_node(ASTCopyContext * /*context*/) {
  auto *a = new WbAST();
  a->filename = filename;
  a->line_no = line_no;
  a->col_no = col_no;
  a->sym_ptr = sym_ptr;
  return a;
}

Sym *WbCallbacks::new_Sym(cchar *name) {
  auto *ws = new WbSymbol();
  Sym *s = new Sym();
  s->asymbol = ws;
  ws->sym = s;
  if (name) {
    s->name = if1_cannonicalize_string(if1, name);
  }
  return s;
}

bool smoke_test_ifa() {
  auto *cb = new WbCallbacks();
  ifa_init(cb);
  bool ok = (if1 != nullptr && pdb != nullptr);
  ifa_reset();
  return ok;
}

}  // namespace langwb::ifa_bridge
