# dsl.ml — Zero-C++ language extension + hygienic & IFA-driven macros in minilang!
#
# 1. Extend the grammar in-memory with a brand-new `when ... do ... otherwise ...` form.
grammar expr += when_expr;
grammar when_expr : 'when' expr 'do' expr 'otherwise' expr $right 0;

# 2. Define a hygienic form macro for `when_expr` purely in minilang.
form_macro when_expr =>
  let f = stx_call() in
  let t = stx_parse("expr", "if __c__ then __a__ else __b__") in
  let t1 = stx_splice(t, "__c__", stx_child(f, 1)) in
  let t2 = stx_splice(t1, "__a__", stx_child(f, 3)) in
  stx_splice(t2, "__b__", stx_child(f, 5));

# 3. Define a hygienic call macro `triple` purely in minilang (introduces `tmp`).
macro triple =>
  let a = stx_arg(stx_call(), 0) in
  let t = stx_parse("expr", "let tmp = __x__ in tmp + tmp + tmp") in
  stx_splice(t, "__x__", a);

# 4. Define an IFA-attribute-driven Stage-1 macro purely in minilang.
stage1_macro pick_const =>
  let c = stx_call() in
  if attr_is_const_i64(stx_arg(c, 0)) != 0
  then stx_arg(c, 1)
  else stx_arg(c, 2);

# 5. Use all three together: outer `tmp = 10` is NOT captured by `triple`'s `tmp`,
#    IFA proves `tmp + 1 == 11` (constant), so `pick_const` chooses `triple(tmp + 4)` = 42.
let tmp = 10 in
when 5 > 2 do pick_const(tmp + 1, triple(tmp + 4), 0) otherwise 999
