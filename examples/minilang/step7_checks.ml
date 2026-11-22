# Step 7 Showcase: Declarative Static Semantics (`check`) & Higher-Order Contour Cloning
#
# 1. `check binop` enforces a compile-time division-by-zero lint over IFA's
#    constant-propagation lattice (`attr_is_const_i64` / `attr_const_i64`).
# 2. `check norm2` enforces a domain-specific record contract (`attr_has_field`).
# 3. Record-stored functions (`ops.total`) and `def`-block functions (`total`)
#    are both contour-specialized per concrete record shape!

check binop =>
  let n = stx_call() in
  if stx_text_eq(stx_child(n, 1), "/") != 0
  then
    if attr_is_const_i64(stx_child(n, 2)) != 0
    then
      if attr_const_i64(stx_child(n, 2)) == 0
      then stx_error(n, "division by zero detected at compile time")
      else 0
    else 0
  else 0;

check norm2 =>
  let arg = stx_arg(stx_call(), 0) in
  if attr_has_field(arg, "x") == 0
  then stx_error(arg, "norm2 requires a record with field 'x'")
  else if attr_has_field(arg, "y") == 0
  then stx_error(arg, "norm2 requires a record with field 'y'")
  else 0;

{
  def norm2 = fn(v) => v.x * v.x + v.y * v.y;
  def total_def = fn(p) => sum_fields(p);
  let d = 10 / 2 in
  let n = norm2({ x: 3, y: 4 }) in
  let ops = { total: fn(p) => sum_fields(p) } in
  let s1 = ops.total({ a: 2, b: 3 }) + ops.total({ z: 4 }) in
  let s2 = total_def({ u: 1, v: 2 }) in
  d + n + s1 + s2
}
