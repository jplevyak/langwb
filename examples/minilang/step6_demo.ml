# Step 6 Showcase: Lists, `match`, Grammar-Checked Quasiquote, & Guest Record Reflection
#
# Demonstrates a 100% guest-written `qq_sum_fields` macro using quasiquote
# `` `[expr| ... ] `` + `attr_field_name`, combined with immutable lists,
# structural `match`, and top-level record value evaluation.

stage1_macro qq_sum_fields =>
  let arg = stx_arg(stx_call(), 0) in
  let n = attr_num_fields(arg) in
  if n == 0 then stx_call()
  else
    rec build = fn(i) =>
      let term = `[expr| r.,(attr_field_name(arg, i))] in
      if i == n - 1 then term
      else `[expr| (,(term) + ,(build(i + 1)))]
    in
    `[expr| let r = ,(arg) in ,(build(0))];

rec list_sum = fn(xs) =>
  match xs {
    [] => 0,
    [h | t] => h + list_sum(t)
  } in
let rec_total = qq_sum_fields({ a: 10, b: 12 }) in
let lst_total = list_sum([5, 7, 8]) in
{ rec_total: rec_total, lst_total: lst_total, grand: rec_total + lst_total }
