#!/usr/bin/env bash
set -euo pipefail

LANGWB="${1:-./langwb}"
PASS=0
FAIL=0

expect_eval() {
  local name="$1"
  local expr="$2"
  local expected="$3"
  local got
  if ! got="$("$LANGWB" -e "$expr" 2>&1)"; then
    echo "FAIL [$name]: exited non-zero: $got" >&2
    FAIL=$((FAIL + 1))
    return
  fi
  if [[ "$got" != "$expected" ]]; then
    echo "FAIL [$name]: got '$got', expected '$expected'" >&2
    FAIL=$((FAIL + 1))
    return
  fi
  PASS=$((PASS + 1))
}

expect_error() {
  local name="$1"
  local expr="$2"
  local pattern="$3"
  local out
  if out="$("$LANGWB" -e "$expr" 2>&1)"; then
    echo "FAIL [$name]: expected error matching '$pattern', but succeeded with '$out'" >&2
    FAIL=$((FAIL + 1))
    return
  fi
  if ! grep -Fq -- "$pattern" <<<"$out"; then
    echo "FAIL [$name]: stderr did not contain '$pattern'; got:" >&2
    echo "$out" >&2
    FAIL=$((FAIL + 1))
    return
  fi
  PASS=$((PASS + 1))
}

expect_flag_contains() {
  local name="$1"
  local flag="$2"
  local target="$3"
  local pattern="$4"
  local out
  if [[ "$target" == *.ml ]]; then
    out="$("$LANGWB" "$flag" "$target" 2>&1)"
  else
    out="$("$LANGWB" "$flag" -e "$target" 2>&1)"
  fi
  if ! grep -Fq -- "$pattern" <<<"$out"; then
    echo "FAIL [$name]: output did not contain '$pattern'; got:" >&2
    echo "$out" >&2
    FAIL=$((FAIL + 1))
    return
  fi
  PASS=$((PASS + 1))
}

# 1. Stage-1 fallbacks (§2.3)
expect_eval "pow_opt_runtime_fallback" \
  "pow_opt(2, runtime_val(5))" "32"

expect_eval "pow_opt_merged_contours_fallback" \
  "let f = fn(n) => pow_opt(2, n) in f(3) + f(4)" "24"

expect_eval "guest_stage1_decline_fallback" '
  prim my_abs c_call "labs";
  stage1_macro my_abs =>
    let c = stx_call() in
    if attr_is_const_i64(stx_arg(c, 0)) != 0
    then stx_parse("expr", "999")
    else c;
  my_abs(0 - 7) + my_abs(runtime_val(0 - 11))
' "1010"

# 2. Deeper hygiene tests (§2.9)
expect_eval "hygiene_nested_with_tmp" \
  "let tmp = 100 in with_tmp(10, with_tmp(20, tmp))" "130"

expect_eval "hygiene_nested_syntax_rule" '
  syntax_rule triple($x:expr) => "let tmp = $x in tmp + tmp + tmp";
  let tmp = 5 in triple(triple(tmp))
' "45"

expect_eval "hygiene_aif_inside_with_tmp" \
  "let tmp = 100 in with_tmp(5, aif tmp + 2 then it * 3 else 0)" "311"

expect_eval "hygiene_macro_defining_binder_for_another" '
  syntax_rule add_ten($x:expr) => "let tmp = 10 in $x + tmp";
  syntax_rule mul_two($x:expr) => "let tmp = 2 in $x * tmp";
  let tmp = 7 in add_ten(mul_two(tmp))
' "24"

# 3. Golden --dump-expanded & --dump-attrs checks (§2.9)
expect_flag_contains "golden_dump_expanded_unroll" \
  "--dump-expanded" "let x = 2 in pow_opt(x, 3)" "=== Expanded SyntaxObj ==="

expect_flag_contains "golden_dump_attrs_reflect" \
  "--dump-attrs" "examples/minilang/reflect.ml" "fields={x,y,z}"

# 4. Negative diagnostic tests (§2.9)
expect_error "neg_sum_fields_non_record" \
  "sum_fields(42)" "stage1 macro 'sum_fields' declined"

expect_error "neg_unbound_ident" \
  "let x = 1 in y + 1" "unbound identifier 'y'"

expect_error "neg_syntax_error" \
  "1 + * 3" "syntax error"

# 5. Core role annotations, resolution pass, letrec, general calls, & traces (§2.4, §2.7, §2.8)
expect_eval "letrec_sum_to_10" \
  "rec r = fn(n) => if n < 1 then 0 else n + r(n - 1) in r(10)" "55"

expect_error "neg_nonrec_let_callee_unbound" \
  "let r = fn(n) => if n < 1 then 0 else n + r(n - 1) in r(10)" "unbound identifier 'r'"

expect_eval "general_call_iife" \
  "(fn(x) => x * 2)(21)" "42"

expect_eval "general_call_record_field" \
  "let p = { f: fn(x) => x + 1 } in p.f(41)" "42"

expect_eval "custom_core_let_surface" \
  'grammar expr += "[a-zA-Z_][a-zA-Z0-9_]*" '\'':='\'' expr '\'';'\'' expr $right 0 core(let, bind=0, rhs=2, body=4); x := 10; y := x * 4; y + 2' \
  "42"

expect_error "neg_unexpanded_3child_form" \
  'grammar expr += '\''foo'\'' '\''bar'\'' expr; foo bar 1' \
  "unexpanded syntax form 'expr'"

expect_error "neg_macro_origin_trace" \
  'syntax_rule bad_ref($x:expr) => "$x + missing_var"; bad_ref(1)' \
  "(expanded from 'bad_ref'"

expect_eval "recursive_variadic_guest_macro" '
  macro sum_all =>
    let c = stx_call() in
    let n = stx_num_args(c) in
    rec loop = fn(c, n, i) =>
      if i == n - 1 then stx_arg(c, i)
      else stx_splice(stx_splice(stx_parse("expr", "(__a__ + __b__)"), "__a__", stx_arg(c, i)), "__b__", loop(c, n, i + 1))
    in loop(c, n, 0);
  sum_all(10, 20, 7, 5)
' "42"

expect_eval "stx_is_core_query" '
  macro check_datum =>
    let a = stx_arg(stx_call(), 0) in
    if stx_is_core(a, "datum") != 0 then a else stx_parse("expr", "0");
  check_datum(42) + check_datum(1 + 2)
' "42"

# 6. Seq/Define partial expansion, self-hosted decl grammar, mid-file decls,
#    macro-generated grammar, ellipsis syntax_rule, binding queries, & bare #lang prelude
expect_eval "seq_mutual_recursion" \
  "{ def is_even = fn(n) => if n == 0 then 1 else is_odd(n - 1); def is_odd = fn(n) => if n == 0 then 0 else is_even(n - 1); is_even(10) * 40 + is_odd(7) * 2 }" \
  "42"

expect_eval "seq_macro_generated_define" '
  macro def_double =>
    let c = stx_call() in
    stx_splice(stx_splice(stx_parse("stmt", "def __n__ = __v__ * 2"), "__n__", stx_arg(c, 0)), "__v__", stx_arg(c, 1));
  { def_double(x, 21); x }
' "42"

expect_eval "mid_file_declaration" \
  'def a = 10; syntax_rule add_ten($x:expr) => "$x + 10"; add_ten(a + 22)' "42"

expect_eval "macro_generated_grammar_mid_file" '
  macro install_bang =>
    let r1 = grammar_add_rule("expr", "bang_expr", "") in
    let r2 = grammar_add_rule("bang_expr", "'\''!'\'' expr", "$right 5") in
    stx_parse("expr", "0");
  syntax_rule form bang_expr(_ $x:expr) => "($x * 2)";
  install_bang(0);
  !21
' "42"

expect_eval "syntax_rule_variadic_ellipsis" \
  'syntax_rule sum_v($xs:expr...) => "($xs + ...)"; sum_v(10, 20, 7, 5)' "42"

expect_eval "stx_binding_eq_query" '
  macro same_id =>
    let c = stx_call() in
    if stx_binding_eq(stx_arg(c, 0), stx_arg(c, 1)) != 0
    then stx_parse("expr", "42")
    else stx_parse("expr", "0");
  let x = 1 in same_id(x, x) + same_id(x, y)
' "42"

# Bare mode: zero C++ grammar rules, booted completely from examples/langs/prelude.lang
bare_out="$("$LANGWB" --bare -e $'#lang prelude\nlet tmp = 10 in unless 1 > 2 then with_tmp(5, tmp + 27) else 0' 2>&1)"
if [[ "$bare_out" != "42" ]]; then
  echo "FAIL [bare_prelude_lang]: got '$bare_out', expected '42'" >&2
  FAIL=$((FAIL + 1))
else
  PASS=$((PASS + 1))
fi

# 7. Contour-aware Stage-1 & demand-driven constant splitting (§2.2)
expect_eval "contour_polymorphic_sum_fields" \
  "let f = fn(p) => sum_fields(p) in f({ x: 10, y: 20 }) + f({ a: 5, b: 7 })" "42"

expect_flag_contains "contour_demand_split_pow_opt_unrolls" \
  "--dump-expanded" "let f = fn(n) => pow_opt(2, n) in f(3) + f(4)" "b * b * b * b"

expect_eval "contour_mixed_const_and_runtime_fallback" \
  "let f = fn(n) => pow_opt(2, n) in f(3) + f(runtime_val(4))" "24"

expect_eval "contour_guest_stage1_macro_demand_split" '
  stage1_macro bonus_if_const =>
    let c = stx_call() in
    if attr_is_const_i64(stx_arg(c, 0)) != 0
    then stx_parse("expr", "20")
    else stx_parse("expr", "2");
  let f = fn(x) => bonus_if_const(x) in f(1) + f(2) + f(runtime_val(3))
' "42"

expect_eval "contour_attr_meta_abi_queries" '
  stage1_macro count_ctrs =>
    stx_int(attr_contour_count(stx_arg(stx_call(), 0)));
  let f = fn(x) => count_ctrs(x) in f(1) + f({ a: 2 })
' "4"

# 8. Copilot PR #3 regression tests
expect_error "copilot_invalid_binop_role" \
  'grammar expr += expr '\''@'\'' expr $left 3 core(binop, lhs=0, rhs=2); 1 @ 2' \
  "invalid core(binop)"

expect_error "copilot_invalid_field_role" \
  'grammar expr += expr '\''->'\'' "[a-zA-Z_][a-zA-Z0-9_]*" $left 5 core(field, obj=0); let p = { a: 1 } in p->a' \
  "invalid core(field)"

expect_eval "copilot_const_contour_attr" '
  stage1_macro check_const_ctr =>
    let a = stx_arg(stx_call(), 0) in
    if attr_contour_count(a) == 1 then stx_int(attr_contour_const_i64(a, 0)) else stx_parse("expr", "0");
  check_const_ctr(42)
' "42"

expect_eval "copilot_custom_core_app_args" '
  grammar expr += '\''invoke'\'' expr '\''with'\'' args $right 0 core(app, fn=1, args=3);
  syntax_rule first($a:expr, $b:expr) => "$a";
  invoke first with 42, 99
' "42"

expect_eval "copilot_free_id_eq_literal" '
  macro is_else =>
    let c = stx_call() in
    if stx_free_id_eq(stx_arg(c, 0), stx_parse("expr", "otherwise")) != 0
    then stx_parse("expr", "1")
    else stx_parse("expr", "0");
  is_else(otherwise) * 10 + is_else(foo) + (let otherwise = 99 in is_else(otherwise))
' "10"

expect_error "copilot_local_expand_depth_limit" \
  'macro loop => stx_local_expand(stx_call()); loop(1)' \
  "exceeded max depth"

expect_eval "copilot_pow_fallback_no_overflow" \
  "pow_opt(3037000500, runtime_val(1))" "3037000500"

expect_eval "copilot_syntax_rule_no_placeholder_collision" '
  syntax_rule m($x:expr) => "let __pat_x__ = 10 in $x + __pat_x__";
  let y = 1 in m(y)
' "11"

expect_eval "copilot_forward_ref_guest_macro" '
  macro outer => identity(stx_arg(stx_call(), 0));
  macro identity => stx_arg(stx_call(), 0);
  outer(42)
' "42"

expect_eval "copilot_grammar_prefix_custom_semi_after_def" \
  'grammar expr += "[a-zA-Z_][a-zA-Z0-9_]*" '\'':='\'' expr '\'';'\'' expr $right 0 core(let, bind=0, rhs=2, body=4); def a = 2; x := 10; x * a' \
  "20"

expect_eval "copilot_module_begin_once_on_seq" \
  $'#lang mydsl\ndef a = 32; a + magic' "42"

expect_error "copilot_multiline_top_level_loc" \
  $'def a = 1;\ndef b = 2;\ny' "3:0: unbound identifier 'y'"

expect_eval "copilot_scope_aware_syntax_sig" '
  syntax_rule choose($n:expr, $x:expr) => "let x = 2 in if_const($n, x, $x)";
  let f = fn(n, x) => choose(n, x) in f(1, 5) + f(runtime_val(2), 7)
' "9"

expect_eval "copilot_dynamic_dispatch_not_redirected" \
  "let f = fn(n) => pow_opt(2, n) in let h = if runtime_val(0) != 0 then f else fn(n) => 1000 in h(3) + h(4)" \
  "2000"

expect_eval "copilot_hygienic_clone_names" \
  "let f = fn(n) => pow_opt(2, n) in let __c_f_0 = fn(n) => 1000 in f(3) + f(4)" \
  "24"

expect_eval "copilot_head_first_stage1_discarded_branch" \
  "if_const(1, 42, sum_fields(0))" "42"

# 9. Hardening Step 4.5 — Real closure capture (§2.1)
expect_eval "closure_capture_outer_let" \
  "let a = 5 in let f = fn(x) => x + a in f(1)" "6"

expect_eval "closure_capture_returning_closure" \
  "let mk = fn(a) => fn(x) => x + a in mk(5)(1)" "6"

expect_eval "closure_capture_nested_curried" \
  "let mk = fn(a) => fn(b) => fn(c) => a + b * c in mk(2)(4)(10)" "42"

expect_eval "closure_capture_recursive" \
  "let base = 10 in rec f = fn(n) => if n == 0 then base else n + f(n - 1) in f(4)" "20"

expect_eval "closure_capture_runtime_value" \
  "let a = runtime_val(40) in let f = fn(x) => x + a in f(runtime_val(2))" "42"

expect_eval "closure_capture_guest_macro_recursive" '
  macro sum_all_cap =>
    let c = stx_call() in
    let n = stx_num_args(c) in
    rec loop = fn(i) =>
      if i == n - 1 then stx_arg(c, i)
      else stx_splice(stx_splice(stx_parse("expr", "(__a__ + __b__)"), "__a__", stx_arg(c, i)), "__b__", loop(i + 1))
    in loop(0);
  sum_all_cap(10, 20, 7, 5)
' "42"

# 10. Hardening Step 4.5 — Bounded analyze->Stage-1->analyze re-analysis loop (§2.2c)
expect_eval "reanalyze_expr_const_prop" \
  "let f = fn(n) => pow_opt(2, n + 1) in f(3) + f(4)" "48"

out_expr_prop=$("$LANGWB" --dump-expanded -e "let f = fn(n) => pow_opt(2, n + 1) in f(3) + f(4)" 2>&1)
if echo "$out_expr_prop" | grep -q 'b \* b \* b \* b \* b'; then
  PASS=$((PASS + 1))
else
  echo "FAIL [reanalyze_expr_const_prop_unrolled]: expected unrolled 5th power in --dump-expanded" >&2
  FAIL=$((FAIL + 1))
fi

expect_eval "reanalyze_nested_call_const_prop" \
  "let g = fn(n) => pow_opt(2, n) in let f = fn(n) => g(n) in f(3) + f(4)" "24"

out_nested_prop=$("$LANGWB" --dump-expanded -e "let g = fn(n) => pow_opt(2, n) in let f = fn(n) => g(n) in f(3) + f(4)" 2>&1)
if echo "$out_nested_prop" | grep -q 'b \* b \* b \* b'; then
  PASS=$((PASS + 1))
else
  echo "FAIL [reanalyze_nested_call_const_prop_unrolled]: expected unrolled 4th power in --dump-expanded" >&2
  FAIL=$((FAIL + 1))
fi

out_no_clone=$("$LANGWB" --dump-expanded -e "let id = fn(n) => n in id(3) + id(4)" 2>&1)
if echo "$out_no_clone" | grep -q '^7$' && ! echo "$out_no_clone" | grep -q '__c_id'; then
  PASS=$((PASS + 1))
else
  echo "FAIL [reanalyze_zero_clones_when_undemanded]: unexpected clone or wrong output" >&2
  FAIL=$((FAIL + 1))
fi

# 11. Hardening Step 4.5 — Source-mapped IFA diagnostics (§2.5)
expect_error "ifa_diag_source_loc" \
  "let p = {a: 1} in p.b" \
  "<input>:1:20: error: unresolved member 'b'"

expect_error "ifa_diag_macro_provenance" \
  'syntax_rule getb($x:expr) => "$x.b"; let p = {a: 1} in getb(p)' \
  "<macro:getb@"

# 12. Step 5 — Scoped Syntax (`using syntax`), GLR `Amb` Disambiguation & Grammar Hygiene
expect_eval "using_syntax_isolated_conflicting_exts" '
  syntax_ext ext_a {
    grammar expr += op_a_expr core(passthrough);
    grammar op_a_expr : '\''op'\'' expr;
    syntax_rule form op_a_expr(_, $x:expr) => "($x + 1)";
  };
  syntax_ext ext_b {
    grammar expr += op_b_expr core(passthrough);
    grammar op_b_expr : '\''op'\'' expr;
    syntax_rule form op_b_expr(_, $x:expr) => "($x * 10)";
  };
  let x = 4 in (using syntax ext_a in { op x }) + (using syntax ext_b in { op x })
' "45"

expect_error "using_syntax_no_leak_outside" '
  syntax_ext ext_a {
    grammar expr += op_a_expr core(passthrough);
    grammar op_a_expr : '\''op'\'' expr;
    syntax_rule form op_a_expr(_, $x:expr) => "($x + 1)";
  };
  op 5
' "syntax error"

expect_eval "using_syntax_nested_composition" '
  syntax_ext ext_a {
    grammar expr += inc_expr core(passthrough);
    grammar inc_expr : '\''inc'\'' expr;
    syntax_rule form inc_expr(_, $x:expr) => "($x + 1)";
  };
  syntax_ext ext_b {
    grammar expr += dbl_expr core(passthrough);
    grammar dbl_expr : '\''dbl'\'' expr;
    syntax_rule form dbl_expr(_, $x:expr) => "($x * 2)";
  };
  let v = 20 in using syntax ext_a in { inc (using syntax ext_b in { dbl v }) }
' "41"

expect_eval "using_syntax_lang_module" \
  "using syntax mydsl in { when 5 > 2 do 42 otherwise 0 }" "42"

expect_eval "glr_amb_tier2_expansion_resolution" '
  grammar {
    expr += bang_valid core(passthrough) | bang_unexpanded core(passthrough);
    bang_valid : '\''!'\'' expr;
    bang_unexpanded : '\''!'\'' expr;
  };
  syntax_rule form bang_valid(_, $e:expr) => "($e * 2)";
  !21
' "42"

expect_eval "glr_amb_tier3_ifa_type_disambiguation" '
  grammar {
    expr += at_rec core(passthrough) | at_int core(passthrough);
    at_rec : '\''@'\'' expr;
    at_int : '\''@'\'' expr;
  };
  syntax_rule form at_rec(_, $e:expr) => "$e.x";
  syntax_rule form at_int(_, $e:expr) => "($e + 100)";
  let p = { x: 2 } in let n = 40 in (@p) + (@n)
' "142"

expect_error "glr_amb_unresolved_error" '
  grammar {
    expr += amb_a core(passthrough) | amb_b core(passthrough);
    amb_a : '\''@'\'' expr;
    amb_b : '\''@'\'' expr;
  };
  syntax_rule form amb_a(_, $e:expr) => "($e + 1)";
  syntax_rule form amb_b(_, $e:expr) => "($e + 2)";
  @40
' "unresolved syntax ambiguity"

expect_eval "grammar_hygiene_macro_nterms_ok" '
  form_macro use_a_form => stx_int(10);
  form_macro use_b_form => stx_int(32);
  macro install_a => {
    def _1 = grammar_add_rule("item", "'"'"'alpha'"'"'", "");
    def _2 = grammar_add_rule("use_a_form", "'"'"'use_a'"'"' item", "");
    def _3 = grammar_add_rule("expr", "use_a_form", "");
    stx_int(0)
  };
  macro install_b => {
    def _1 = grammar_add_rule("item", "'"'"'beta'"'"'", "");
    def _2 = grammar_add_rule("use_b_form", "'"'"'use_b'"'"' item", "");
    def _3 = grammar_add_rule("expr", "use_b_form", "");
    stx_int(0)
  };
  install_a();
  install_b();
  (use_a alpha) + (use_b beta)
' "42"

expect_error "grammar_hygiene_macro_nterms_no_crosstalk" '
  form_macro use_a_form => stx_int(10);
  form_macro use_b_form => stx_int(32);
  macro install_a => {
    def _1 = grammar_add_rule("item", "'"'"'alpha'"'"'", "");
    def _2 = grammar_add_rule("use_a_form", "'"'"'use_a'"'"' item", "");
    def _3 = grammar_add_rule("expr", "use_a_form", "");
    stx_int(0)
  };
  macro install_b => {
    def _1 = grammar_add_rule("item", "'"'"'beta'"'"'", "");
    def _2 = grammar_add_rule("use_b_form", "'"'"'use_b'"'"' item", "");
    def _3 = grammar_add_rule("expr", "use_b_form", "");
    stx_int(0)
  };
  install_a();
  install_b();
  use_a beta
' "18:8: syntax error"

# 13. Step 6 — Lists, `match`, Grammar-Checked Quasiquote, & General EvalValue
expect_eval "list_and_match_sum" \
  "rec sum = fn(xs) => match xs { [] => 0, [h | t] => h + sum(t) } in sum([10, 20, 12])" \
  "42"

expect_eval "list_len_and_cons" \
  "list_len(cons(1, [2, 3, 4]))" "4"

expect_eval "match_int_and_wildcard" \
  "let f = fn(n) => match n { 0 => 100, 1 => 200, _ => n + 1 } in f(0) + f(1) + f(41)" \
  "342"

expect_eval "match_record_destructure" \
  "match { x: 15, y: 27 } { { x: a, y } => a + y }" "42"

expect_eval "quasiquote_hygienic_macro" '
  macro qq_twice =>
    let x = stx_arg(stx_call(), 0) in
    `[expr| let tmp = ,(x) in tmp + tmp];
  let tmp = 20 in qq_twice(tmp + 1)
' "42"

expect_eval "quasiquote_guest_sum_fields" '
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
  let id = fn(v) => v in
  let p = id({ x: 10, y: 20, z: 12 }) in
  qq_sum_fields(p)
' "42"

expect_error "quasiquote_compile_time_grammar_error" '
  macro bad_qq => `[expr| 1 + * 3];
  bad_qq()
' "quasiquote template does not parse as"

expect_eval "eval_value_string" '"hello world"' '"hello world"'

expect_eval "eval_value_record" \
  'let mk = fn(a, b) => { x: a, name: b } in mk(42, "ok")' \
  '{ x: 42, name: "ok" }'

expect_eval "eval_value_function" "fn(x) => x + 1" "<fn>"

# 14. Step 7 — Declarative Static Semantics (`check`) & Higher-Order Contour Cloning
expect_eval "static_check_div_zero_ok" '
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
  84 / 2
' "42"

expect_error "static_check_div_zero_caught" '
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
  let k = 3 - 3 in 10 / k
' "division by zero detected at compile time"

expect_error "static_check_record_contract_caught" '
  check norm2 =>
    let arg = stx_arg(stx_call(), 0) in
    if attr_has_field(arg, "y") == 0
    then stx_error(arg, "norm2 requires a record with field y")
    else 0;
  let norm2 = fn(v) => v.x in norm2({ x: 3 })
' "norm2 requires a record with field y"

expect_eval "contour_clone_mixed_record_arities" \
  "let sum_rec = fn(p) => sum_fields(p) in sum_rec({ a: 10, b: 12 }) + sum_rec({ x: 20 })" \
  "42"

expect_eval "contour_clone_def_block_records" \
  "{ def total = fn(p) => sum_fields(p); total({ a: 10, b: 12 }) + total({ x: 20 }) }" \
  "42"

expect_eval "contour_clone_def_block_const_split" \
  "{ def f = fn(n) => pow_opt(2, n + 1); f(3) + f(4) }" \
  "48"

expect_eval "contour_clone_record_stored_method" \
  "let ops = { total: fn(p) => sum_fields(p) } in ops.total({ a: 10, b: 12 }) + ops.total({ x: 20 })" \
  "42"

# 15. Operator precedence & eager declaration-body parsing
expect_eval "precedence_mul_add_left_assoc" "3 * 3 + 4 * 4" "25"
expect_eval "precedence_sub_left_assoc" "10 - 3 - 2" "5"
expect_eval "precedence_field_mul_add" "let v = { x: 3, y: 4 } in v.x * v.x + v.y * v.y" "25"

expect_error "eager_decl_parse_uncalled_macro_syntax_error" \
  "macro never_called => 1 + * 3; 42" \
  "invalid declaration"

expect_eval "eager_decl_parse_sees_earlier_grammar_ext" '
  grammar expr += twice_val core(passthrough);
  grammar twice_val : '\''twice'\'' expr;
  syntax_rule form twice_val(_, $x:expr) => "($x + $x)";
  macro emit_42 => stx_int(twice 21);
  emit_42()
' "42"

# 16. Review 2026-11-22 (1f88a19..ce2fa9b) regression suite
expect_eval "review_closure_captured_by_closure" \
  "let a = 1 in let f = fn(x) => x + a in let g = fn(y) => f(y) in g(3)" "4"

expect_eval "review_closure_stored_in_record" \
  "let k = 1 in let ops = { t: fn(p) => p + k } in ops.t(1)" "2"

expect_eval "review_closure_in_record_with_sum_fields" \
  "let k = 1 in let ops = { t: fn(p) => sum_fields(p) + k } in ops.t({ a: 10, b: 12 }) + ops.t({ x: 18 })" \
  "42"

expect_eval "review_mutual_rec_def_capture" \
  "let k = 5 in { def a = fn(x) => if x == 0 then k else b(x - 1); def b = fn(x) => a(x) + k; a(3) }" \
  "20"

expect_eval "review_rec_self_call_in_nested_lambda" \
  "let k = 7 in rec f = fn(n) => if n == 0 then k else (fn(y) => f(y))(n - 1) in f(2)" \
  "7"

expect_eval "review_eval_value_bool_true" "5 == 5" "true"
expect_eval "review_eval_value_bool_false" "5 == 6" "false"
expect_eval "review_eval_value_record_bool" "{ x: 5 == 5 }" "{ x: true }"
expect_eval "review_eval_value_list" "[1, 2, 3]" "[1, 2, 3]"
expect_eval "review_eval_value_empty_list" "[]" "[]"

expect_eval "review_tier3_linear_12_ambs" '
  grammar {
    expr += at_rec core(passthrough) | at_int core(passthrough);
    at_rec : '\''@'\'' expr;
    at_int : '\''@'\'' expr;
  };
  syntax_rule form at_rec(_, $e:expr) => "$e.x";
  syntax_rule form at_int(_, $e:expr) => "($e + 10)";
  let n = 1 in (@n)+(@n)+(@n)+(@n)+(@n)+(@n)+(@n)+(@n)+(@n)+(@n)+(@n)+(@n)
' "132"

expect_eval "review_tier2_duplicate_derivation_dedup" '
  form_macro use_a_form => stx_int(10);
  macro install_a => {
    def _1 = grammar_add_rule("item", "'\''alpha'\''", "");
    def _2 = grammar_add_rule("use_a_form", "'\''use_a'\'' item", "");
    def _3 = grammar_add_rule("expr", "use_a_form", "");
    stx_int(0)
  };
  install_a();
  install_a();
  use_a alpha
' "10"

expect_error "review_match_non_exhaustive_trap" \
  "match 5 { 1 => 10, 2 => 20 }" \
  "non-exhaustive match"

expect_eval "review_quasiquote_string_literal" '
  macro m => `[expr| "a" ];
  m()
' '"a"'

expect_eval "review_check_not_reserved_keyword" \
  "def check = 2; check * 21" "42"

expect_error "review_decl_body_error_pos" \
  "macro m => 1 + * 3; 5" \
  "1:15: syntax error"

# 15. Step 8 — AOT Compiler Driver (`langwb build`) & Language Server (`langwb --lsp`)
AOT_TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/langwb_aot.XXXXXX")
trap 'rm -rf "$AOT_TMP_DIR"' EXIT

if "$LANGWB" build examples/minilang/contour_spec.ml -o "$AOT_TMP_DIR/contour_spec" >/dev/null 2>&1 \
   && [[ "$("$AOT_TMP_DIR/contour_spec")" == "42" ]]; then
  PASS=$((PASS + 1))
else
  echo "FAIL [step8_aot_build_contour_spec]: expected standalone binary to print 42" >&2
  FAIL=$((FAIL + 1))
fi

if "$LANGWB" build examples/minilang/step6_demo.ml -o "$AOT_TMP_DIR/step6_demo" >/dev/null 2>&1 \
   && [[ "$("$AOT_TMP_DIR/step6_demo")" == "{ rec_total: 22, lst_total: 20, grand: 42 }" ]]; then
  PASS=$((PASS + 1))
else
  echo "FAIL [step8_aot_build_step6_demo]: expected standalone binary to print record value" >&2
  FAIL=$((FAIL + 1))
fi

if "$LANGWB" build examples/minilang/hello.ml -o "$AOT_TMP_DIR/hello.ll" --emit-llvm >/dev/null 2>&1 \
   && grep -q 'define .*@main' "$AOT_TMP_DIR/hello.ll"; then
  PASS=$((PASS + 1))
else
  echo "FAIL [step8_aot_emit_llvm]: expected LLVM IR with @main" >&2
  FAIL=$((FAIL + 1))
fi

if python3 - "$LANGWB" <<'PYEOF'
import json, subprocess, sys

langwb = sys.argv[1]
def send(p, msg):
    body = json.dumps(msg).encode("utf-8")
    p.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body)
    p.stdin.flush()

def recv(p):
    headers = {}
    while True:
        line = p.stdout.readline()
        if not line:
            return None
        line = line.decode("ascii")
        if line in ("\r\n", "\n"):
            break
        k, v = line.split(":", 1)
        headers[k.strip().lower()] = v.strip()
    return json.loads(p.stdout.read(int(headers["content-length"])).decode("utf-8"))

p = subprocess.Popen([langwb, "--lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
send(p, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
init = recv(p)
assert init["result"]["capabilities"]["hoverProvider"] is True
assert init["result"]["capabilities"]["definitionProvider"] is True

send(p, {"jsonrpc": "2.0", "method": "initialized", "params": {}})

with open("examples/minilang/step6_demo.ml") as f:
    doc = f.read()
send(p, {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": "file:///step6_demo.ml", "languageId": "minilang", "version": 1, "text": doc}
}})
diag1 = recv(p)
assert diag1["params"]["diagnostics"] == [], f"unexpected diagnostics: {diag1}"

send(p, {"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": "file:///step6_demo.ml"},
    "position": {"line": 25, "character": 15}
}})
hover = recv(p)
assert "int64 = 22" in hover["result"]["contents"]["value"], f"unexpected hover: {hover}"

send(p, {"jsonrpc": "2.0", "id": 3, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": "file:///step6_demo.ml"},
    "position": {"line": 25, "character": 15}
}})
defn = recv(p)
assert defn["result"]["range"]["start"] == {"line": 23, "character": 4}, f"unexpected def: {defn}"

send(p, {"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": "file:///step6_demo.ml", "version": 2},
    "contentChanges": [{"text": "let p = { a: 1 } in p.b"}]
}})
diag2 = recv(p)
msgs = [d["message"] for d in diag2["params"]["diagnostics"]]
assert any("unresolved member 'b'" in m for m in msgs), f"missing error diag: {diag2}"

send(p, {"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": None})
recv(p)
send(p, {"jsonrpc": "2.0", "method": "exit", "params": None})
p.wait(timeout=5)
assert p.returncode == 0
PYEOF
then
  PASS=$((PASS + 1))
else
  echo "FAIL [step8_lsp_hover_def_diagnostics]: LSP server check failed" >&2
  FAIL=$((FAIL + 1))
fi

expect_eval "native_grammar_replace_colon_vs_extend" '
  grammar myop : '\''foo'\'' expr;
  grammar myop := '\''bar'\'' expr;
  grammar expr += myop core(passthrough);
  syntax_rule form myop(_, $e:expr) => "($e + 1)";
  bar 41
' "42"

expect_error "native_grammar_replace_colon_drops_old" '
  grammar myop : '\''foo'\'' expr;
  grammar myop := '\''bar'\'' expr;
  grammar expr += myop core(passthrough);
  syntax_rule form myop(_, $e:expr) => "($e + 1)";
  foo 41
' "syntax error"

# 17. Shortcut cleanups (#5 strip_comments, #3 LSP JSON, #4 AOT rt/execvp, #1 syntax_rule, #2 syntax_ext)
expect_eval "cleanup_hash_token_in_grammar" '
  grammar tag_expr : '\''#'\'' expr | '\''tag'\'' expr;
  grammar expr += tag_expr core(passthrough);
  syntax_rule form tag_expr(_, $x:expr) => ($x * 2);
  tag 21
' "42"

expect_eval "cleanup_unquoted_syntax_rule" '
  syntax_rule dbl($x:expr) => let t = $x in t + t;
  dbl(21)
' "42"

expect_error "cleanup_unquoted_syntax_rule_eager_error" '
  syntax_rule never_called($x:expr) => $x + * 3;
  42
' "syntax error"

expect_error "cleanup_syntax_ext_eager_error" '
  syntax_ext broken_unused {
    macro bad => 1 + * 3;
  };
  42
' "invalid declaration in syntax_ext"

if "$LANGWB" build examples/minilang/hello.ml -o "$AOT_TMP_DIR/out with space" >/dev/null 2>&1 \
   && [[ "$("$AOT_TMP_DIR/out with space")" == "13" ]]; then
  PASS=$((PASS + 1))
else
  echo "FAIL [cleanup_aot_execvp_space_in_path]: expected 13" >&2
  FAIL=$((FAIL + 1))
fi

if python3 - "$LANGWB" <<'PYEOF'
import json, subprocess, sys
langwb = sys.argv[1]
def send(p, msg):
    body = json.dumps(msg).encode("utf-8")
    p.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body)
    p.stdin.flush()
def recv(p):
    headers = {}
    while True:
        line = p.stdout.readline()
        if not line: return None
        line = line.decode("ascii")
        if line in ("\r\n", "\n"): break
        k, v = line.split(":", 1)
        headers[k.strip().lower()] = v.strip()
    return json.loads(p.stdout.read(int(headers["content-length"])).decode("utf-8"))

p = subprocess.Popen([langwb, "--lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
send(p, {"jsonrpc": "2.0", "id": "init-1", "method": "initialize", "params": {}})
init = recv(p)
assert init["id"] == "init-1"
send(p, {"jsonrpc": "2.0", "method": "initialized", "params": {}})
# Document text deliberately contains JSON-like keys ("method", "line", "id")!
doc = 'let s = "{\\"method\\": \\"fake\\", \\"line\\": 999}" in let x = 42 in x'
send(p, {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": "file:///json_keys.ml", "languageId": "minilang", "version": 1, "text": doc}
}})
diag = recv(p)
assert diag["params"]["diagnostics"] == [], f"unexpected diag: {diag}"
send(p, {"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": "file:///json_keys.ml"},
    "position": {"line": 0, "character": len(doc) - 1}
}})
hover = recv(p)
assert "int64 = 42" in hover["result"]["contents"]["value"], f"unexpected hover: {hover}"
send(p, {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": None})
recv(p)
send(p, {"jsonrpc": "2.0", "method": "exit", "params": None})
p.wait(timeout=5)
assert p.returncode == 0
PYEOF
then
  PASS=$((PASS + 1))
else
  echo "FAIL [cleanup_lsp_json_parser_embedded_keys]: LSP JSON parser test failed" >&2
  FAIL=$((FAIL + 1))
fi

# 18. Review 2026-11-22 (ce2fa9b..HEAD) regression tests
expect_error "review2_syntax_rule_error_col" \
  'syntax_rule form foo(_ $x:expr) => ($x + ); 5' \
  "1:41: syntax error"

expect_error "review2_grammar_legacy_quoted_rejected" \
  'grammar expr += "when_x"; 5' \
  "legacy quoted syntax"

expect_error "review2_grammar_colon_existing_rejected" \
  'grammar expr : '\''zz'\''; 1' \
  "already exists"

expect_eval "review2_syntax_ext_replace_preserved" '
  syntax_ext e {
    grammar expr := zz_lit core(passthrough) | "[0-9]+" core(datum);
    grammar zz_lit : '\''zz'\'';
    syntax_rule form zz_lit(_) => 42;
  };
  using syntax e in { zz }
' "42"

expect_error "review2_syntax_ext_rejects_stage1" '
  syntax_ext e { stage1_macro m => stx_int(1); };
  1
' "not supported inside syntax_ext"

expect_eval "review2_nested_amb_resolves" '
  grammar {
    expr += at_rec core(passthrough) | at_int core(passthrough);
    at_rec : '\''@'\'' expr;
    at_int : '\''@'\'' expr;
  };
  syntax_rule form at_rec(_, $e:expr) => $e.x;
  syntax_rule form at_int(_, $e:expr) => ($e + 10);
  let p = { x: { x: 1 } } in (@(@p))
' "1"

expect_eval "review2_string_newline_escaped" '"a\nb"' '"a\nb"'

expect_eval "review2_bool_literals" \
  "if true then (if false then 0 else 42) else 1" "42"

expect_eval "review2_heterogeneous_str_list" \
  '["a", "b"]' \
  '["a", "b"]'

if python3 - "$LANGWB" <<'PYEOF'
import json, subprocess, sys
langwb = sys.argv[1]
def send(p, msg):
    body = json.dumps(msg).encode("utf-8")
    p.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body)
    p.stdin.flush()
def recv(p):
    headers = {}
    while True:
        line = p.stdout.readline()
        if not line: return None
        line = line.decode("ascii")
        if line in ("\r\n", "\n"): break
        k, v = line.split(":", 1)
        headers[k.strip().lower()] = v.strip()
    return json.loads(p.stdout.read(int(headers["content-length"])).decode("utf-8"))

p = subprocess.Popen([langwb, "--lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
send(p, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
init = recv(p)
assert init["result"]["capabilities"]["positionEncoding"] == "utf-8"
send(p, {"jsonrpc": "2.0", "method": "initialized", "params": {}})
# >64 KB of stderr diagnostics (previously deadlocked on the pipe buffer).
big_doc = "let x = { a: 1 } in " + " + ".join(f"x.b{i}" for i in range(500))
send(p, {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": "file:///big.ml", "languageId": "minilang", "version": 1, "text": big_doc}
}})
diag = recv(p)
assert len(diag["params"]["diagnostics"]) >= 500, f"expected >=500 diags, got {len(diag['params']['diagnostics'])}"
send(p, {"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": None})
recv(p)
send(p, {"jsonrpc": "2.0", "method": "exit", "params": None})
p.wait(timeout=15)
assert p.returncode == 0
PYEOF
then
  PASS=$((PASS + 1))
else
  echo "FAIL [review2_lsp_large_diag_no_deadlock]: LSP >64KB diagnostic test failed" >&2
  FAIL=$((FAIL + 1))
fi

# 19. Known Issues & Step 9 (Runtime Hygiene) regression tests
out_chk_warn=$("$LANGWB" -e "check nosuch => 0; 5" 2>&1)
if echo "$out_chk_warn" | grep -q "warning: check target 'nosuch'" && echo "$out_chk_warn" | grep -q '^5$'; then
  PASS=$((PASS + 1))
else
  echo "FAIL [known_issue_check_nosuch_warns]: got '$out_chk_warn'" >&2
  FAIL=$((FAIL + 1))
fi

expect_error "known_issue_check_core_invalid_errors" \
  "check core nosuch => 0; 5" \
  "unknown core form 'nosuch'"

expect_error "known_issue_record_error_human_name" \
  "let p = { a: 1 } in p.b" \
  "unresolved member 'b' of class 'record { a }'"

expect_error "known_issue_wrong_arity_error" \
  "let f = fn(x) => x + 1 in f(1, 2)" \
  "wrong number of arguments to 'f': expected 1, got 2"

expect_eval "known_issue_def_sibling_capture_fixpoint" \
  "let k = 10 in { def pure = fn(x) => x * 2; def cap = fn(x) => x + k; pure(11) + cap(10) }" \
  "42"

# 20. Step 10 — Core Guest-Language Gaps (Mutation/While, I/O, Strings, Tagged Lists, Import/Export)
expect_eval "step10_while_and_local_assign" \
  "let i = 0 in let s = 0 in { while i < 10 do { s = s + i; i = i + 1 }; s }" \
  "45"

expect_eval "step10_mutable_record_field" \
  "let p = { x: 1, y: 2 } in { p.x = 40; p.x + p.y }" \
  "42"

expect_eval "step10_str_concat" 'str_concat("hel", "lo")' '"hello"'
expect_eval "step10_str_len" 'str_len("hello")' "5"
expect_eval "step10_str_eq_true" 'str_eq("ab", str_concat("a", "b"))' "true"
expect_eval "step10_str_eq_false" 'str_eq("ab", "cd")' "false"
expect_eval "step10_str_slice" 'str_slice("abcdef", 1, 4)' '"bcd"'
expect_eval "step10_int_to_str" 'int_to_str(42)' '"42"'
expect_eval "step10_str_to_int" 'str_to_int("42") + 8' "50"

expect_eval "step10_list_bools" '[true, false]' '[true, false]'
expect_eval "step10_list_nested" '[[1, 2], [3]]' '[[1, 2], [3]]'
expect_eval "step10_list_heterogeneous" '[1, "two", true, [3, 4]]' '[1, "two", true, [3, 4]]'
expect_eval "step10_head_str" 'head_str(["hi", "yo"])' '"hi"'
expect_eval "step10_head_bool" 'head_bool([true, false])' "true"
expect_eval "step10_head_list" 'head_list([[10, 20], [30]])' '[10, 20]'
expect_eval "step10_head_tag_str" 'head_tag(["hi"])' "2"

out_io=$("$LANGWB" -e '{ println("hi"); println(42); println([1, "a", true]); 0 }' 2>&1)
if [[ "$out_io" == $'hi\n42\n[1, "a", true]\n0' ]]; then
  PASS=$((PASS + 1))
else
  echo "FAIL [step10_io_println]: got '$out_io'" >&2
  FAIL=$((FAIL + 1))
fi

# AOT heterogeneous list + println test
cat > "$AOT_TMP_DIR/step10_aot.ml" <<'EOF'
{ println(str_concat("val=", int_to_str(42))); [1, "two", true, [3, 4]] }
EOF
if "$LANGWB" build "$AOT_TMP_DIR/step10_aot.ml" -o "$AOT_TMP_DIR/step10_aot" >/dev/null 2>&1 \
   && [[ "$("$AOT_TMP_DIR/step10_aot")" == $'val=42\n[1, "two", true, [3, 4]]' ]]; then
  PASS=$((PASS + 1))
else
  echo "FAIL [step10_aot_io_str_list]: unexpected AOT output" >&2
  FAIL=$((FAIL + 1))
fi

# Multi-file import/export tests (hygiene, private hiding, macro export, diamond import)
cat > "$AOT_TMP_DIR/base_mod.ml" <<'EOF'
def secret = 1;
def bump = fn(x) => x + secret;
export bump;
EOF
cat > "$AOT_TMP_DIR/left_mod.ml" <<EOF
import "$AOT_TMP_DIR/base_mod.ml";
def helper = fn(x) => bump(x);
def inc2 = fn(x) => helper(helper(x));
syntax_rule add10(\$x:expr) => (\$x + 10);
export inc2, add10;
EOF
cat > "$AOT_TMP_DIR/right_mod.ml" <<EOF
import "$AOT_TMP_DIR/base_mod.ml";
def plus_one = fn(x) => bump(x);
export plus_one;
EOF

expect_eval "step10_import_export_hygiene_and_diamond" "
  import \"$AOT_TMP_DIR/left_mod.ml\";
  import \"$AOT_TMP_DIR/right_mod.ml\";
  def helper = fn(x) => x * 100;
  add10(inc2(plus_one(29))) + helper(0)
" "42"

expect_error "step10_import_private_not_visible" "
  import \"$AOT_TMP_DIR/left_mod.ml\";
  helper(1)
" "unbound identifier 'helper'"

# 21. Step 10.5 — Sum Types / ADTs in IFA (per-variant CreationSets & narrowing)
expect_eval "step10_5_adt_option_monomorphic" \
  "type Option = None | Some(val); match Some(42) { None => 0, Some(v) => v }" \
  "42"

expect_eval "step10_5_adt_option_runtime_some" \
  "type Option = None | Some(val); let x = if runtime_val(1) == 1 then Some(42) else None in match x { None => 0, Some(v) => v }" \
  "42"

expect_eval "step10_5_adt_option_runtime_none" \
  "type Option = None | Some(val); let x = if runtime_val(0) == 1 then Some(42) else None in match x { Some(v) => v, None => 99 }" \
  "99"

expect_eval "step10_5_adt_result_heterogeneous_narrowing" \
  'type Result = Ok(val) | Err(msg); let r = if runtime_val(0) == 1 then Ok(42) else Err("bad") in match r { Ok(n) => int_to_str(n), Err(s) => str_concat("err: ", s) }' \
  '"err: bad"'

expect_eval "step10_5_adt_recursive_expr_eval" \
  "type Expr = Lit(n) | Add(lhs, rhs); rec ev = fn(e) => match e { Lit(n) => n, Add(l, r) => ev(l) + ev(r) } in ev(Add(Lit(20), Add(Lit(10), Lit(12))))" \
  "42"

expect_eval "step10_5_adt_polymorphic_contour_specialization" \
  "type Option = None | Some(val); let f = fn(o) => match o { None => 10, Some(v) => v } in f(Some(32)) + f(None)" \
  "42"

expect_error "step10_5_adt_ctor_wrong_arity" \
  "type Option = None | Some(val); Some(1, 2)" \
  "wrong number of arguments to variant 'Some': expected 1, got 2"

expect_error "step10_5_adt_unknown_variant_in_match" \
  "type Option = None | Some(val); match Some(1) { Bogus(x) => x }" \
  "unknown variant 'Bogus'"

# AOT binary for recursive Expr ADT
cat > "$AOT_TMP_DIR/step10_5_expr.ml" <<'EOF'
type Expr = Lit(n) | Add(lhs, rhs);
rec ev = fn(e) => match e {
  Lit(n) => n,
  Add(l, r) => ev(l) + ev(r)
} in
ev(Add(Lit(20), Add(Lit(10), Lit(12))))
EOF
if "$LANGWB" build "$AOT_TMP_DIR/step10_5_expr.ml" -o "$AOT_TMP_DIR/step10_5_expr" >/dev/null 2>&1 \
   && [[ "$("$AOT_TMP_DIR/step10_5_expr")" == "42" ]]; then
  PASS=$((PASS + 1))
else
  echo "FAIL [step10_5_aot_recursive_expr]: expected 42" >&2
  FAIL=$((FAIL + 1))
fi

# 21. Review 2026-11-23 (bb6f4eb..9d9fc47) regression tests
expect_eval "review3_captured_mutable_closure_assign" \
  'let x = 1 in { def f = fn(u) => { x = x + 1 }; f(0); f(0); x }' \
  "3"

expect_eval "review3_captured_mutable_post_assign" \
  'let x = 1 in let g = fn(u) => x in { x = 5; g(0) }' \
  "5"

expect_eval "review3_captured_mutable_def_and_outer" \
  'let x = 1 in { def f = fn(u) => x; x = 5; f(0) + x }' \
  "10"

expect_eval "review3_setfield_lhs_precedence_binop" \
  'let p = {x:1} in { 1 + p.x = 5; p.x }' \
  "5"

expect_eval "review3_setfield_lhs_precedence_if" \
  'let p = {x:1} in { if true then p.x = 5 else p.x = 6; p.x }' \
  "5"

expect_eval "review3_setfield_lhs_precedence_fn" \
  'let c = {n:0} in { def inc = fn(u) => c.n = c.n + 1; inc(0); inc(0); c.n }' \
  "2"

expect_error "review3_def_fn_reassign_rejected" \
  '{ def f = fn(x) => x; f = fn(x) => x + 1; f(1) }' \
  "cannot assign to function definition 'f'"

expect_eval "review3_let_fn_reassign_narrowed" \
  'let f = fn(u) => u in { f = 3; f }' \
  "3"

expect_eval "review3_adt_print_constructor_and_nullary" \
  'type Option = None | Some(val); Some(3)' \
  "Some(3)"

expect_eval "review3_adt_print_nullary" \
  'type Option = None | Some(val); None' \
  "None"

expect_eval "review3_println_record_adt_fn_safe" \
  'type O = N | S(v); { println({ x: 1 }); println(S(65)); println(N); println(fn(x) => x); 0 }' \
  "$(printf '{ x: 1 }\nS(65)\nN\n<fn>\n0')"

expect_eval "review3_string_eq_and_ne" \
  'if "ab" == str_concat("a", "b") then (if "ab" != "cd" then 42 else 0) else 0' \
  "42"

expect_error "review3_adt_duplicate_variant_across_types" \
  'type A = X(v); type B = X(w); 1' \
  "already defined by type 'A'"

expect_error "review3_adt_duplicate_variant_in_type" \
  'type T = A | B | A; 1' \
  "duplicate variant 'A'"

expect_error "review3_adt_duplicate_field" \
  'type T = A(x, x); A(1, 2)' \
  "duplicate field 'x'"

expect_error "review3_adt_pattern_arity_mismatch" \
  'type Option = None | Some(val); match Some(1) { Some(a, b) => a }' \
  "pattern 'Some(a, b)' has 2 field(s); variant 'Some' has 1"

expect_error "review3_adt_match_non_adt_static_error" \
  'type T = A | B; match runtime_val(0) { A => 1, B => 2 }' \
  "illegal primitive argument type"

expect_eval "review3_wb_is_variant_not_shadowing_user_binding" \
  'let __wb_is_variant = fn(a, b) => a + b in __wb_is_variant(20, 22)' \
  "42"

expect_eval "review3_check_core_while" \
  'check core while => 0; 5' \
  "5"

expect_error "review3_head_tag_mismatch_traps" \
  'head(["a"]) + 1' \
  "head expected int64 element, got string"

expect_error "review3_head_str_tag_mismatch_traps" \
  'head_str([1, 2])' \
  "head_str expected string element, got int64"

expect_error "review3_head_bool_tag_mismatch_traps" \
  'head_bool([5])' \
  "head_bool expected bool element, got int64"

# Relative imports, module ADTs, hygienic exported macros, and cycle detection
mkdir -p "$AOT_TMP_DIR/rev3/sub"
printf 'def fb = fn(x) => x * 2;\nexport fb;\n' > "$AOT_TMP_DIR/rev3/sub/b.ml"
printf 'import "b.ml";\ndef fa = fn(x) => fb(x) + 1;\nexport fa;\n' > "$AOT_TMP_DIR/rev3/sub/a.ml"
printf 'import "a.ml";\nfa(20)\n' > "$AOT_TMP_DIR/rev3/sub/main.ml"
if [[ "$("$LANGWB" "$AOT_TMP_DIR/rev3/sub/main.ml")" == "41" ]]; then
  PASS=$((PASS + 1))
else
  echo "FAIL [review3_relative_import_from_file_dir]: expected 41" >&2
  FAIL=$((FAIL + 1))
fi

printf 'type Option = None | Some(val);\ndef mk = fn(x) => Some(x);\nexport Option, mk;\n' > "$AOT_TMP_DIR/rev3/t.ml"
expect_eval "review3_import_module_adt" \
  "import \"$AOT_TMP_DIR/rev3/t.ml\"; match mk(42) { Some(v) => v, None => 0 }" \
  "42"

printf 'type SecretOpt = SN | SS(v);\ndef wrap_s = fn(x) => match SS(x) { SS(v) => v, SN => 0 };\nexport wrap_s;\n' > "$AOT_TMP_DIR/rev3/t_priv.ml"
expect_eval "review4_import_private_adt_used_internally" \
  "import \"$AOT_TMP_DIR/rev3/t_priv.ml\"; wrap_s(42)" \
  "42"
expect_error "review4_import_private_adt_ctor_hidden" \
  "import \"$AOT_TMP_DIR/rev3/t_priv.ml\"; SS(9)" \
  "constructor 'SS' of type 'SecretOpt' is not exported by module 't_priv.ml'"

printf 'def secret = 41;\nsyntax_rule addsec($x:expr) => ($x + secret);\nexport addsec;\n' > "$AOT_TMP_DIR/rev3/priv.ml"
expect_eval "review3_import_hygienic_macro_private_def" \
  "import \"$AOT_TMP_DIR/rev3/priv.ml\"; let secret = 999 in addsec(1)" \
  "42"

printf 'import "c2.ml";\ndef f1 = fn(x) => x;\nexport f1;\n' > "$AOT_TMP_DIR/rev3/c1.ml"
printf 'import "c1.ml";\ndef f2 = fn(x) => f1(x) + 1;\nexport f2;\n' > "$AOT_TMP_DIR/rev3/c2.ml"
expect_error "review3_import_cycle_error" \
  "import \"$AOT_TMP_DIR/rev3/c1.ml\"; f2(1)" \
  "import cycle: c1.ml -> c2.ml -> c1.ml"

# 22. Review 2026-11-23 worktree (9d9fc47) regression tests
# REPL survives bad == and ADT non-ADT match without exiting
repl_survive=$(printf '{x:1} == {x:1}\n1 + 1\n' | "$LANGWB" --repl 2>&1)
if [[ "$repl_survive" == *"illegal primitive argument type"* ]] && [[ "$repl_survive" == *"=> 2"* ]]; then
  PASS=$((PASS + 1))
else
  echo "FAIL [review4_repl_survives_type_violation]: got '$repl_survive'" >&2
  FAIL=$((FAIL + 1))
fi

# REPL prints newline before => when print() wrote without newline
repl_nl=$(printf 'print(1)\n' | "$LANGWB" --repl 2>&1)
if [[ "$repl_nl" == *$'1\n=> 0'* ]]; then
  PASS=$((PASS + 1))
else
  echo "FAIL [review4_repl_print_newline]: got '$repl_nl'" >&2
  FAIL=$((FAIL + 1))
fi

expect_eval "review4_eval_print_newline" \
  '{ print("a"); 5 }' \
  "$(printf 'a\n5')"

expect_error "review4_eq_rejects_int_bool" \
  '1 == true' \
  "illegal primitive argument type"

expect_error "review4_eq_rejects_list" \
  '[1] == [1]' \
  "illegal primitive argument type"

expect_eval "review4_adt_union_prints_some" \
  'type Option = None | Some(val); if runtime_val(1) == 1 then Some(1) else None' \
  "Some(1)"

expect_eval "review4_adt_union_prints_none" \
  'type Option = None | Some(val); if runtime_val(0) == 1 then Some(1) else None' \
  "None"

expect_eval "review4_record_closure_field_prints_fn" \
  'let q = {f: fn(x) => x} in q' \
  "{ f: <fn> }"

expect_error "review4_pattern_arity_multiline_location" \
  "$(printf 'type Option = None | Some(val); match Some(1) {\n  Some(a, b) => a }')" \
  "<input>:2:2: error: pattern 'Some(a, b)' has 2 field(s); variant 'Some' has 1"

# Two modules defining ADTs with the same type name but distinct variants
printf 'type Option = Nada | Algo(v);\ndef mk2 = fn(x) => Algo(x);\nexport Option, mk2;\n' > "$AOT_TMP_DIR/rev3/t2.ml"
expect_eval "review4_import_same_adt_type_name_distinct_variants" \
  "import \"$AOT_TMP_DIR/rev3/t.ml\"; import \"$AOT_TMP_DIR/rev3/t2.ml\"; match mk2(42) { Algo(v) => v, Nada => 0 }" \
  "42"

# LSP survives bad == with diagnostic AND resolves percent-encoded URI imports
mkdir -p "$AOT_TMP_DIR/rev3/my dir"
printf 'def qval = 42;\nexport qval;\n' > "$AOT_TMP_DIR/rev3/my dir/q.ml"
if python3 - "$LANGWB" "$AOT_TMP_DIR/rev3/my dir" <<'PYEOF'
import json, subprocess, sys, urllib.parse
langwb, spaced_dir = sys.argv[1], sys.argv[2]
def send(p, msg):
    body = json.dumps(msg).encode("utf-8")
    p.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body)
    p.stdin.flush()
def recv(p):
    headers = {}
    while True:
        line = p.stdout.readline()
        if not line: return None
        line = line.decode("ascii")
        if line in ("\r\n", "\n"): break
        k, v = line.split(":", 1)
        headers[k.strip().lower()] = v.strip()
    return json.loads(p.stdout.read(int(headers["content-length"])).decode("utf-8"))

p = subprocess.Popen([langwb, "--lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
send(p, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
assert recv(p)["id"] == 1
send(p, {"jsonrpc": "2.0", "method": "initialized", "params": {}})

# 1. Bad == must emit a diagnostic and keep the server alive
send(p, {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": "file:///bad_eq.ml", "languageId": "minilang", "version": 1, "text": "{x:1} == {x:1}"}
}})
diag1 = recv(p)
assert len(diag1["params"]["diagnostics"]) >= 1, f"expected diagnostic, got {diag1}"

# 2. Percent-encoded file:// URI with space in directory resolves relative import
uri = "file://" + urllib.parse.quote(spaced_dir + "/main.ml")
send(p, {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": uri, "languageId": "minilang", "version": 1, "text": 'import "q.ml"; qval'}
}})
diag2 = recv(p)
assert diag2["params"]["diagnostics"] == [], f"unexpected diag on spaced URI: {diag2}"

send(p, {"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": None})
assert recv(p)["id"] == 2
send(p, {"jsonrpc": "2.0", "method": "exit", "params": None})
p.wait(timeout=5)
assert p.returncode == 0
PYEOF
then
  PASS=$((PASS + 1))
else
  echo "FAIL [review4_lsp_survives_type_violation_and_decodes_uri]: LSP test failed" >&2
  FAIL=$((FAIL + 1))
fi

# 2026-11-23 follow-up: non-ADT `match` scrutinee is reported once, at the
# `match`, without leaking the template's `__m__` or a `<macro:…>` location.
nonadt_out=$("$LANGWB" -e 'type T = A | B; match runtime_val(0) { A => 1, B => 2 }' 2>&1 || true)
if [[ "$nonadt_out" == *"<input>:1:16: error: illegal primitive argument type 'match scrutinee (expected T)' illegal: int64"* ]] &&
   [[ "$nonadt_out" != *"__m__"* ]] && [[ "$nonadt_out" != *"<macro:"* ]] &&
   [[ $(grep -c "match scrutinee" <<<"$nonadt_out") -eq 1 ]]; then
  PASS=$((PASS + 1))
else
  echo "FAIL [review5_match_non_adt_scrutinee_message]: got '$nonadt_out'" >&2
  FAIL=$((FAIL + 1))
fi

# Exported `syntax_ext` and `export grammar` macros resolve the module's private
# `def`s, not the importer's bindings of the same name.
cat > "$AOT_TMP_DIR/rev3/priv_ext.ml" <<'MLEOF'
def secret = 41;
syntax_ext sec_ext {
  grammar expr += addsec_expr core(passthrough);
  grammar addsec_expr : 'addsec' expr;
  syntax_rule form addsec_expr(_, $x:expr) => "($x + secret)";
};
export sec_ext;
MLEOF
expect_eval "review5_import_syntax_ext_hygienic_private_def" \
  "import \"$AOT_TMP_DIR/rev3/priv_ext.ml\"; let secret = 999 in using syntax sec_ext in { addsec 1 }" \
  "42"
cat > "$AOT_TMP_DIR/rev3/priv_gram.ml" <<'MLEOF'
def secret = 41;
grammar expr += addsec2_expr core(passthrough);
grammar addsec2_expr : 'addsec2' expr;
syntax_rule form addsec2_expr(_, $x:expr) => "($x + secret)";
export grammar, addsec2_expr;
MLEOF
expect_eval "review5_import_export_grammar_hygienic_private_def" \
  "import \"$AOT_TMP_DIR/rev3/priv_gram.ml\"; let secret = 999 in addsec2 1" \
  "42"

# Same-file macros resolve template identifiers to the file's own top-level
# `def`s (they are wrapped with the file's scope), and stay hygienic when the
# use site shadows the name.
expect_eval "review5_same_file_syntax_rule_refs_def" \
  'def secret = 41; syntax_rule addsec($x:expr) => ($x + secret); addsec(1)' \
  "42"
expect_eval "review5_same_file_syntax_rule_def_not_captured_by_let" \
  'def secret = 41; syntax_rule addsec($x:expr) => ($x + secret); let secret = 999 in addsec(1)' \
  "42"
expect_eval "review5_same_file_syntax_rule_def_not_captured_by_block_def" \
  'def secret = 41; syntax_rule addsec($x:expr) => ($x + secret); let y = 2 in { def secret = 5; addsec(y) }' \
  "43"
expect_eval "review5_same_file_guest_macro_refs_def" \
  'def secret = 41; macro addsec => stx_splice(stx_parse("expr", "(__a__ + secret)"), "__a__", stx_arg(stx_call(), 0)); let secret = 999 in addsec(1)' \
  "42"
expect_eval "review5_same_file_syntax_ext_refs_def" "
  def secret = 41;
  syntax_ext e {
    grammar expr += as_expr core(passthrough);
    grammar as_expr : 'addsec' expr;
    syntax_rule form as_expr(_, \$x:expr) => \"(\$x + secret)\";
  };
  let secret = 9 in using syntax e in { addsec 1 }
" "42"
printf 'def secret = 41;\nsyntax_rule addsec($x:expr) => ($x + secret);\ndef use_it = fn(y) => let secret = 0 in addsec(y);\nexport use_it, addsec;\n' > "$AOT_TMP_DIR/rev3/inner_macro.ml"
expect_eval "review5_module_uses_own_macro_internally" \
  "import \"$AOT_TMP_DIR/rev3/inner_macro.ml\"; let secret = 7 in use_it(1) + addsec(0)" \
  "83"

# Diagnostics inside an imported module name the module file.
printf 'def f = fn(x) => x + 1;\ndef g = fn(y) =>\n  nope(y);\nexport g;\n' > "$AOT_TMP_DIR/rev3/bad_mod.ml"
expect_error "review5_module_error_names_file" \
  "import \"$AOT_TMP_DIR/rev3/bad_mod.ml\"; g(1)" \
  "bad_mod.ml:3:2: unbound identifier 'nope'"
printf 'type T = A | B;\ndef h = fn(x) => match x { A => 1, B => 2 };\ndef k = fn(u) => h(3);\nexport k;\n' > "$AOT_TMP_DIR/rev3/bad_mod_ifa.ml"
expect_error "review5_module_ifa_error_names_file" \
  "import \"$AOT_TMP_DIR/rev3/bad_mod_ifa.ml\"; k(0)" \
  "bad_mod_ifa.ml:2:17: error: illegal primitive argument type 'match scrutinee (expected T)'"
printf 'type T = A | A;\n' > "$AOT_TMP_DIR/rev3/bad_mod_decl.ml"
expect_error "review5_module_decl_error_names_file" \
  "import \"$AOT_TMP_DIR/rev3/bad_mod_decl.ml\"; 1" \
  "bad_mod_decl.ml:1:13: duplicate variant 'A' in type 'T'"
expect_error "review5_main_file_error_unprefixed" 'nope' "langwb: 1:0: unbound identifier 'nope'"

# The LSP reports a module's diagnostic at the top of the open document, not at
# the module's line number.
if python3 - "$LANGWB" "$AOT_TMP_DIR/rev3" <<'PYEOF'
import json, subprocess, sys, urllib.parse
langwb, d = sys.argv[1], sys.argv[2]
def send(p, msg):
    body = json.dumps(msg).encode("utf-8")
    p.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body)
    p.stdin.flush()
def recv(p):
    headers = {}
    while True:
        line = p.stdout.readline()
        if not line: return None
        line = line.decode("ascii")
        if line in ("\r\n", "\n"): break
        k, v = line.split(":", 1)
        headers[k.strip().lower()] = v.strip()
    return json.loads(p.stdout.read(int(headers["content-length"])).decode("utf-8"))
p = subprocess.Popen([langwb, "--lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
send(p, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
assert recv(p)["id"] == 1
uri = "file://" + urllib.parse.quote(d + "/uses_bad.ml")
send(p, {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": uri, "languageId": "minilang", "version": 1,
                     "text": 'import "bad_mod.ml";\n\n\ng(1)'}}})
diags = recv(p)["params"]["diagnostics"]
mod = [x for x in diags if "bad_mod.ml:3:2" in x["message"]]
assert mod, f"no module diagnostic: {diags}"
assert mod[0]["range"]["start"]["line"] == 0, f"module diagnostic misplaced: {mod[0]}"
send(p, {"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": None})
assert recv(p)["id"] == 2
send(p, {"jsonrpc": "2.0", "method": "exit", "params": None})
p.wait(timeout=5)
PYEOF
then
  PASS=$((PASS + 1))
else
  echo "FAIL [review5_lsp_module_diag_not_misplaced]: LSP test failed" >&2
  FAIL=$((FAIL + 1))
fi

# `export grammar;` alone also exports the form macros for its productions.
cat > "$AOT_TMP_DIR/rev3/gram_only.ml" <<'MLEOF'
def secret = 41;
grammar expr += addsec3_expr core(passthrough);
grammar addsec3_expr : 'addsec3' expr;
syntax_rule form addsec3_expr(_, $x:expr) => "($x + secret)";
export grammar;
MLEOF
expect_eval "review5_export_grammar_exports_form_macros" \
  "import \"$AOT_TMP_DIR/rev3/gram_only.ml\"; let secret = 999 in addsec3 1" \
  "42"

# Readable non-bool condition, bare-constructor hint, private-constructor
# explanation, constructor-only printing, and Stage-1 macro def hygiene.
expect_error "review6_while_literal_condition_named" 'while 0 do 1' \
  "'while condition (expected bool)' illegal: int64"
expect_error "review6_if_literal_condition_named" 'let y = 1 in if "a" then 1 else 2' \
  "<input>:1:13: error: illegal primitive argument type 'if condition (expected bool)' illegal: string"
expect_eval "review6_bool_literal_condition_ok" 'let i = 0 in if true then 1 else 2' "1"
expect_error "review6_bare_ctor_pattern_hint" \
  'type Option = None | Some(val); match Some(1) { Some => 1, None => 0 }' \
  "pattern 'Some' has 0 field(s); variant 'Some' has 1; write 'Some(_)' to ignore the field(s)"
expect_error "review6_private_ctor_in_pattern_explained" \
  "import \"$AOT_TMP_DIR/rev3/t_priv.ml\"; match wrap_s(1) { SS(v) => v, _ => 0 }" \
  "constructor 'SS' of type 'SecretOpt' is not exported by module 't_priv.ml'"
printf 'type Option = None | Some(val);\ndef mk = fn(x) => Some(x);\nexport mk;\n' > "$AOT_TMP_DIR/rev3/opt_a.ml"
printf 'type Option = Nada | Algo(v);\ndef mk2 = fn(x) => Algo(x);\nexport mk2;\n' > "$AOT_TMP_DIR/rev3/opt_b.ml"
expect_eval "review6_qualified_adt_prints_ctor_name" \
  "import \"$AOT_TMP_DIR/rev3/opt_a.ml\"; import \"$AOT_TMP_DIR/rev3/opt_b.ml\"; {a: mk(1), b: mk2(2)}" \
  "{ a: Some(1), b: Algo(2) }"
expect_eval "review6_stage1_macro_refs_def" \
  'def secret = 41; stage1_macro addsec1 => stx_splice(stx_parse("expr", "(__a__ + secret)"), "__a__", stx_arg(stx_call(), 0)); addsec1(1)' \
  "42"
expect_eval "review6_stage1_macro_def_not_captured_by_let" \
  'def secret = 41; stage1_macro addsec1 => stx_splice(stx_parse("expr", "(__a__ + secret)"), "__a__", stx_arg(stx_call(), 0)); let secret = 999 in addsec1(1)' \
  "42"

# A closure called from two sites with different constants: the closure-call
# inliner must not kill the closure construction that the other call still uses.
expect_eval "review6_closure_two_constant_calls" \
  'let s = 41 in let f = fn(x) => x + s in f(1) + f(2)' "85"
expect_eval "review6_closure_two_calls_runtime_capture" \
  'let s = runtime_val(41) in let f = fn(x) => x + s in f(1) + f(2) + f(3)' "129"
expect_eval "review6_closure_two_captures_two_calls" \
  'let a = 1 in let b = 2 in let g = fn(x, y) => x * a + y * b in g(1, 2) + g(3, 4)' "16"

# Constructor equality, reserved `_tag`, misplaced `import`.
ADT_PRE='type Option = None | Some(val); type Color = Red | Green | Blue;'
expect_eval "review7_eq_nullary_ctors" "$ADT_PRE {a: None == None, b: Red == Green, c: Red != Green}" \
  "{ a: true, b: false, c: true }"
expect_eval "review7_eq_union_vs_nullary" \
  "$ADT_PRE let o = if runtime_val(0) == 1 then Some(3) else None in {a: o == None, b: None != o, c: Some(1) == None}" \
  "{ a: true, b: false, c: false }"
expect_eval "review7_eq_enum_runtime" \
  "$ADT_PRE let c = if runtime_val(1) == 1 then Green else Red in {g: c == Green, r: c == Red}" \
  "{ g: true, r: false }"
expect_eval "review7_eq_in_function_contours" \
  "$ADT_PRE def is_none = fn(o) => o == None; {a: is_none(Some(1)), b: is_none(None)}" \
  "{ a: false, b: true }"
expect_error "review7_eq_payload_ctors_rejected" "$ADT_PRE Some(1) == Some(1)" \
  "illegal primitive argument type expression illegal: Option::Some"
expect_error "review7_eq_different_adts_rejected" "$ADT_PRE None == Red" "illegal primitive argument type"
expect_error "review7_assign_tag_rejected" "$ADT_PRE let o = None in { o._tag = 5; o }" \
  "cannot assign to '_tag': it is reserved for the variant of ADT values"
expect_eval "review7_assign_variant_field_allowed" "$ADT_PRE let o = Some(1) in { o.val = 5; o }" "Some(5)"
expect_error "review7_import_in_block_explained" '{ import "x.ml"; 1 }' \
  "'import' is only allowed at the top level of a file"
expect_eval "review7_ident_named_import_ok" 'let import = 3 in import + 1' "4"

# Recursive values print through per-type printer functions: any depth, with
# `<cycle>` for a value that reaches itself and `...` past 1000 levels.
LIST_PRE='type L = Nil | Cons(h, t); rec build = fn(n) => if n == 0 then Nil else Cons(n, build(n - 1)) in'
expect_eval "review7_print_cycle_self" \
  'type L = Nil | Cons(h, t); let c = Cons(1, Nil) in { c.t = c; c }' "Cons(1, <cycle>)"
expect_eval "review7_print_deep_list" "$LIST_PRE build(9)" \
  "Cons(9, Cons(8, Cons(7, Cons(6, Cons(5, Cons(4, Cons(3, Cons(2, Cons(1, Nil)))))))))"
expect_eval "review7_print_literal_list_past_old_limit" \
  'type L = Nil | Cons(h, t); Cons(1, Cons(2, Cons(3, Cons(4, Cons(5, Cons(6, Cons(7, Cons(8, Nil))))))))' \
  "Cons(1, Cons(2, Cons(3, Cons(4, Cons(5, Cons(6, Cons(7, Cons(8, Nil))))))))"
expect_eval "review7_print_shared_not_cycle" \
  'type L = Nil | Cons(h, t); let c = Cons(1, Nil) in {a: c, b: c}' "{ a: Cons(1, Nil), b: Cons(1, Nil) }"
deep_out=$("$LANGWB" -e "$LIST_PRE build(1200)" 2>&1 || true)
if [[ "$deep_out" == "Cons(1200, Cons(1199, "* ]] && [[ "$deep_out" == *"Cons(201, ...)"* ]] &&
   [[ "$deep_out" != *"<union>"* ]]; then
  PASS=$((PASS + 1))
else
  echo "FAIL [review7_print_depth_limit]: got '${deep_out:0:120}...'" >&2
  FAIL=$((FAIL + 1))
fi

# Field assignment through a record class IFA split into clones (WbSymbol::copy
# must keep the clone's members).
LPRE='type L = Nil | Cons(h, t);'
expect_eval "review8_set_field_on_split_ctor" "$LPRE let c = Cons(1, Cons(2, Nil)) in { c.t.h = 5; c }" \
  "Cons(1, Cons(5, Nil))"
expect_eval "review8_set_tail_on_split_ctor" "$LPRE let c = Cons(1, Cons(2, Nil)) in { c.t.t = Cons(3, Nil); c }" \
  "Cons(1, Cons(2, Cons(3, Nil)))"
expect_eval "review8_set_cycle_through_nested_field" \
  "$LPRE let c = Cons(1, Cons(2, Cons(3, Nil))) in { c.t.t = c; c }" "Cons(1, Cons(2, <cycle>))"
expect_eval "review8_set_field_on_split_record" \
  'let mk = fn(a, b) => {h: a, t: b} in let c = mk(1, mk(2, 0)) in { c.t.h = 5; c }' \
  "{ h: 1, t: { h: 5, t: 0 } }"
expect_error "review8_set_field_maybe_nil_rejected" \
  "$LPRE let c = if runtime_val(1) == 1 then Cons(1, Nil) else Nil in { c.h = 5; c }" \
  "unresolved member 'h' of class 'L::Nil'"

# 2026-11-23 review of 7d79d9e..eecfcf9.
# §2.1: a union of two clones of one class with differently typed members
# gets distinct classtags (reads_fields demand), so printing picks the right one.
expect_eval "review9_print_union_split_ctor_int" \
  'type L = Nil | Cons(h, t); if runtime_val(1) == 1 then Cons(1, Nil) else Cons("a", Nil)' "Cons(1, Nil)"
expect_eval "review9_print_union_split_ctor_str" \
  'type L = Nil | Cons(h, t); if runtime_val(0) == 1 then Cons(1, Nil) else Cons("a", Nil)' 'Cons("a", Nil)'
expect_eval "review9_print_union_split_record" \
  '{ def mk = fn(x) => { a: x }; if runtime_val(0) == 1 then mk(1) else mk("hi") }' '{ a: "hi" }'
expect_eval "review9_print_union_split_nested" \
  'type L = Nil | Cons(h, t); let x = if runtime_val(1) == 1 then Cons(1, Nil) else Cons("a", Nil) in {v: x}' \
  "{ v: Cons(1, Nil) }"
# §2.2: `_tag` is reserved in type declarations and record literals.
expect_error "review9_tag_field_in_type_rejected" 'type Box = B(_tag); B(42)' \
  "'_tag' cannot be a field of variant 'B'"
expect_error "review9_tag_field_in_record_rejected" '{ _tag: 7 }' "'_tag' cannot be a record field"
# §2.3: exported macros can use the module's unexported helper macros; the
# helpers stay invisible to the importer, and same-named importer macros do not
# interfere. Exported guest `macro`s keep their module Workbench alive.
printf 'syntax_rule priv_inc($x:expr) => ($x + 1);\nsyntax_rule pub_inc2($x:expr) => priv_inc(priv_inc($x));\nexport pub_inc2;\n' > "$AOT_TMP_DIR/rev3/helper_mac.ml"
expect_eval "review9_exported_macro_uses_private_helper" \
  "import \"$AOT_TMP_DIR/rev3/helper_mac.ml\"; pub_inc2(40)" "42"
expect_eval "review9_exported_macro_helper_in_let_body" \
  "import \"$AOT_TMP_DIR/rev3/helper_mac.ml\"; let y = 5 in y + pub_inc2(0)" "7"
expect_error "review9_private_helper_hidden" \
  "import \"$AOT_TMP_DIR/rev3/helper_mac.ml\"; priv_inc(1)" "unbound identifier 'priv_inc'"
expect_eval "review9_private_helper_vs_importer_macro" \
  "import \"$AOT_TMP_DIR/rev3/helper_mac.ml\"; syntax_rule priv_inc(\$x:expr) => (\$x * 100); priv_inc(2) + pub_inc2(5)" \
  "207"
printf 'def secret = 41;\nmacro addsec => stx_splice(stx_parse("expr", "(__a__ + secret)"), "__a__", stx_arg(stx_call(), 0));\nexport addsec;\n' > "$AOT_TMP_DIR/rev3/guest_mac.ml"
expect_eval "review9_exported_guest_macro" \
  "import \"$AOT_TMP_DIR/rev3/guest_mac.ml\"; let secret = 9 in addsec(1)" "42"
expect_eval "review9_free_id_eq_top_level_def" \
  'def otherwise = 5; macro is_else => let c = stx_call() in if stx_free_id_eq(stx_arg(c, 0), stx_parse("expr", "otherwise")) != 0 then stx_parse("expr", "1") else stx_parse("expr", "0"); {a: is_else(otherwise), b: is_else(foo), c: let otherwise = 2 in is_else(otherwise)}' \
  "{ a: 1, b: 0, c: 0 }"
# §2.4: an LSP diagnostic from a module whose name starts with a digit is
# placed at the top of the open document, not read as a line number.
mkdir -p "$AOT_TMP_DIR/rev3/digits"
printf 'def g = fn(y) =>\n  nope(y);\nexport g;\n' > "$AOT_TMP_DIR/rev3/digits/3mod.ml"
if python3 - "$LANGWB" "$AOT_TMP_DIR/rev3/digits" <<'PYEOF'
import json, subprocess, sys, urllib.parse
langwb, d = sys.argv[1], sys.argv[2]
def send(p, msg):
    body = json.dumps(msg).encode("utf-8")
    p.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body)
    p.stdin.flush()
def recv(p):
    headers = {}
    while True:
        line = p.stdout.readline()
        if not line: return None
        line = line.decode("ascii")
        if line in ("\r\n", "\n"): break
        k, v = line.split(":", 1)
        headers[k.strip().lower()] = v.strip()
    return json.loads(p.stdout.read(int(headers["content-length"])).decode("utf-8"))
p = subprocess.Popen([langwb, "--lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
send(p, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
assert recv(p)["id"] == 1
uri = "file://" + urllib.parse.quote(d + "/main.ml")
send(p, {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": uri, "languageId": "minilang", "version": 1,
                     "text": 'import "3mod.ml";\n\n\ng(1)'}}})
diags = recv(p)["params"]["diagnostics"]
mod = [x for x in diags if "3mod.ml:2:2" in x["message"]]
assert mod, f"no module diagnostic: {diags}"
assert mod[0]["range"]["start"]["line"] == 0, f"module diagnostic misplaced: {mod[0]}"
send(p, {"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": None})
assert recv(p)["id"] == 2
send(p, {"jsonrpc": "2.0", "method": "exit", "params": None})
p.wait(timeout=5)
PYEOF
then
  PASS=$((PASS + 1))
else
  echo "FAIL [review9_lsp_digit_module_name]: LSP test failed" >&2
  FAIL=$((FAIL + 1))
fi

# Transitive helper macros across module boundaries (b_priv -> b_pub -> a_pub)
printf 'syntax_rule b_priv($x:expr) => ($x + 1);\nsyntax_rule b_pub($x:expr) => b_priv($x);\nexport b_pub;\n' > "$AOT_TMP_DIR/rev3/trans_b.ml"
printf 'import "trans_b.ml";\nsyntax_rule a_pub($x:expr) => b_pub($x);\nexport a_pub;\n' > "$AOT_TMP_DIR/rev3/trans_a.ml"
expect_eval "review5_transitive_helper_macro_across_modules" \
  "import \"$AOT_TMP_DIR/rev3/trans_a.ml\"; a_pub(41)" \
  "42"
expect_error "review5_transitive_unexported_macro_stays_hidden" \
  "import \"$AOT_TMP_DIR/rev3/trans_a.ml\"; b_pub(41)" \
  "unbound identifier 'b_pub'"

# `for_syntax` helpers (including recursive functions) work in `macro`,
# `form_macro`, `stage1_macro`, `check`, `hook`, and `prim ... transfer` bodies
# after `expr := ...` replaces the guest grammar without any
# `let ... = ... in ...` rule.
cat > "$AOT_TMP_DIR/rev3/replaced_for_syntax.lang" <<'LANGEOF'
for_syntax k_bias => 10;
for_syntax mk_plus_bias => fn(x) =>
  stx_splice(stx_parse("expr", "(__x__ + 10)"), "__x__", x);

grammar {
  expr := 'bind' "[a-zA-Z_][a-zA-Z0-9_]*" ':=' expr 'within' expr $right 0 core(let, bind=1, rhs=3, body=5)
        | 'lam' '(' params ')' '->' expr                          $right 0 core(lambda, bind=2, body=5)
        | 'cond' expr 'then' expr 'else' expr                     $right 0 core(if, test=1, then=3, else=5)
        | expr '==' expr                                          $left 1  core(binop, lhs=0, op=1, rhs=2)
        | expr '+' expr                                           $left 3  core(binop, lhs=0, op=1, rhs=2)
        | expr '-' expr                                           $left 3  core(binop, lhs=0, op=1, rhs=2)
        | expr '*' expr                                           $left 4  core(binop, lhs=0, op=1, rhs=2)
        | expr '(' args ')'                                       $left 5  core(app, fn=0, args=2)
        | expr '(' ')'                                            $left 5  core(app, fn=0)
        | '(' expr ')'                                                     core(paren, inner=1)
        | "\"([^\"\\]|\\[^])*\""                                           core(string)
        | "[0-9]+"                                                         core(datum)
        | biased_form                                                      core(passthrough)
        | "[a-zA-Z_][a-zA-Z0-9_]*"                                         core(ident);
  biased_form : 'biased' '(' expr ')';
};

for_syntax unroll_mul => lam(sym_stx, n) ->
  cond n == 1 then sym_stx
  else stx_splice(
         stx_splice(stx_parse("expr", "(__a__ * __b__)"), "__a__", sym_stx),
         "__b__", unroll_mul(sym_stx, n - 1));

macro add_bias => mk_plus_bias(stx_arg(stx_call(), 0));

form_macro biased_form => mk_plus_bias(stx_child(stx_call(), 2));

stage1_macro pow_const =>
  bind c := stx_call() within
    bind b := stx_arg(c, 0) within
      bind e := stx_arg(c, 1) within
        cond attr_is_const_i64(e) == 1 then
          stx_splice(
            stx_splice(stx_parse("expr", "bind __t := __b__ within __body__"),
                       "__b__", b),
            "__body__", unroll_mul(stx_parse("expr", "__t"), attr_const_i64(e)))
        else b;

prim my_abs c_call "llabs" transfer =>
  cond fa_arg_is_const_i64(0) == 1 then
    fa_emit_const_i64(fa_arg_const_i64(0) + (k_bias - 10))
  else fa_emit_i64();

check call guard_fn =>
  bind n := stx_call() within
    bind a := stx_arg(n, 0) within
      cond attr_is_const_i64(a) == 1 then
        cond attr_const_i64(a) == k_bias then
          stx_error(a, "forbidden bias constant")
        else 0
      else 0;
LANGEOF

expect_eval "for_syntax_under_replaced_grammar" \
  "$(printf '#lang %s/rev3/replaced_for_syntax.lang\nbind guard_fn := lam(x) -> x within bind x := add_bias(2) within pow_const(my_abs(3), 3) + guard_fn(x)' "$AOT_TMP_DIR")" \
  "39"
expect_error "for_syntax_check_under_replaced_grammar" \
  "$(printf '#lang %s/rev3/replaced_for_syntax.lang\nbind guard_fn := lam(x) -> x within guard_fn(10)' "$AOT_TMP_DIR")" \
  "forbidden bias constant"
expect_error "for_syntax_replaced_grammar_rejects_minilang_let" \
  "$(printf '#lang %s/rev3/replaced_for_syntax.lang\nlet x = 1 in x' "$AOT_TMP_DIR")" \
  "syntax error"
expect_eval "for_syntax_form_macro_under_replaced_grammar" \
  "$(printf '#lang %s/rev3/replaced_for_syntax.lang\nbind guard_fn := lam(x) -> x within biased(5)' "$AOT_TMP_DIR")" \
  "15"

# A child `#lang` declares a `for_syntax` helper in the replaced syntax and
# uses it from `hook module_begin`.
cat > "$AOT_TMP_DIR/rev3/replaced_for_syntax_hook.lang" <<LANGEOF
#lang $AOT_TMP_DIR/rev3/replaced_for_syntax.lang
for_syntax wrap_magic => lam(body) ->
  stx_splice(
    stx_splice(stx_parse("expr", "bind __id__ := 32 within __body__"),
               "__id__", stx_unhygienic_id("magic")),
    "__body__", body);
hook module_begin => wrap_magic(stx_call());
LANGEOF
expect_eval "for_syntax_hook_under_replaced_grammar" \
  "$(printf '#lang %s/rev3/replaced_for_syntax_hook.lang\nbind guard_fn := lam(x) -> x within magic + 10' "$AOT_TMP_DIR")" \
  "42"

# Under the default minilang grammar: lambda helpers are bound recursively
# (`LetRec`), while non-lambda helpers stay non-recursive (`Let`), so a
# redefinition can still refer to the previous binding of the same name.
expect_eval "for_syntax_recursive_helper_minilang" \
  'for_syntax fact_stx => fn(n) => if n == 0 then 1 else n * fact_stx(n - 1); macro fact5 => stx_int(fact_stx(5)); fact5()' \
  "120"
expect_eval "for_syntax_nonlambda_shadows_previous" \
  'for_syntax k => 1; for_syntax k => k + 41; macro getk => stx_int(k); getk()' \
  "42"
expect_eval "for_syntax_paren_recursive_capturing_helper" \
  'for_syntax step => 7; for_syntax mul_step => (fn(n) => if n == 0 then 0 else step + mul_step(n - 1)); macro six_steps => stx_int(mul_step(6)); six_steps()' \
  "42"

# Macro-lowered operators in their own non-terminal (FL-2). The `|>`, `**` and
# prefix `-` stand-ins lower to subtraction so the grouping is observable:
# left-assoc `10 |> 3 |> 1` = 6, right-assoc = 8.
op_prio_grammar_head='grammar {
  expr := expr '"'"'+'"'"' expr $left 6 core(binop, lhs=0, op=1, rhs=2)
        | expr '"'"'-'"'"' expr $left 6 core(binop, lhs=0, op=1, rhs=2)
        | expr '"'"'*'"'"' expr $left 7 core(binop, lhs=0, op=1, rhs=2)
        | '"'"'('"'"' expr '"'"')'"'"' core(paren, inner=1)
        | "[0-9]+" core(datum)
        | "[a-zA-Z_][a-zA-Z0-9_]*" core(ident);'
op_prio_grammar_macros='syntax_rule form pipe_e($a:expr, _, $b:expr) => "($a - $b)";
syntax_rule form neg_e(_, $e:expr) => "(0 - $e)";
syntax_rule form pow_e($a:expr, _, $b:expr) => "($a - $b)";'
cat > "$AOT_TMP_DIR/rev3/op_prio.lang" <<LANGEOF
$op_prio_grammar_head
  expr += pipe_e core(passthrough) | neg_e core(passthrough) | pow_e core(passthrough);
  pipe_e : expr pipe_op expr;   pipe_op : '|>' \$binary_op_left 1;
  neg_e  : neg_op expr;         neg_op  : '-'  \$unary_op_right 8;
  pow_e  : expr pow_op expr;    pow_op  : '**' \$binary_op_right 9;
};
$op_prio_grammar_macros
LANGEOF
cat > "$AOT_TMP_DIR/rev3/rule_prio.lang" <<LANGEOF
$op_prio_grammar_head
  expr += pipe_e \$left 1 core(passthrough);
  pipe_e : expr '|>' expr \$left 1;
};
$op_prio_grammar_macros
LANGEOF
op_prio_case() {
  expect_eval "op_priority_subproduction_$1" \
    "$(printf '#lang %s/rev3/op_prio.lang\n%s' "$AOT_TMP_DIR" "$2")" "$3"
}
op_prio_case pipe_left_assoc '10 |> 3 |> 1' "6"
op_prio_case pow_right_assoc '10 ** 3 ** 1' "8"
op_prio_case neg_tighter_than_binary '- 10 - 3' "-13"
op_prio_case pow_tighter_than_neg '- 5 ** 3' "-2"
op_prio_case pow_neg_rhs '2 ** - 3' "5"
op_prio_case pipe_loosest '20 |> 2 * 3' "14"
op_prio_case pow_tighter_than_mul '2 * 10 ** 3' "14"
op_prio_case binary_minus_unaffected '1 - - 2' "3"
# Records the DParser behavior FL-2 works around: rule-level `$left` on a
# sub-production whose ends are not its own LHS is not treated as binary, so
# `|>` groups to the right. If this starts returning 6, revisit FL-2.
expect_eval "op_priority_subproduction_rule_level_left_is_right_assoc" \
  "$(printf '#lang %s/rev3/rule_prio.lang\n10 |> 3 |> 1' "$AOT_TMP_DIR")" "8"

# FL-3: a malformed top-level form reports a syntax error where the form
# breaks (parsed as `stmts`), not "expected ';'" after a shorter `stmt`/`expr`
# prefix or at a leading keyword parsed as an identifier.
expect_error "top_level_error_inside_def_rhs" 'def x = 1 +; x' "1:11: syntax error"
expect_error "top_level_error_unclosed_paren_in_def" 'def x = 1; def y = (2; x' "1:21: syntax error"
expect_error "top_level_error_two_exprs_after_def" 'def x = 1; x x' "1:13: syntax error"
expect_error "top_level_error_missing_semicolon_no_semi_program" 'def x = 1 x' "1:10: syntax error"
expect_error "top_level_error_missing_semicolon_newline" "$(printf 'def x = 1\n x')" "2:1: syntax error"
expect_error "top_level_error_program_without_final_expr" 'def x = 1' \
  "1:0: expected an expression (a program must end with one)"
mkdir -p "$AOT_TMP_DIR/fl3"
printf 'export y;\ndef y = 2 +;\n' > "$AOT_TMP_DIR/fl3/bad_mod.ml"
expect_error "top_level_error_in_imported_module" \
  "import \"$AOT_TMP_DIR/fl3/bad_mod.ml\"; y" "2:11: syntax error"

# FL-6: `stx_text(h)` returns a node's source text as a string, so Phase-1
# code can build templates that name an identifier (e.g. as a string literal).
expect_eval "stx_text_ident_to_string_literal" \
  'macro name_of => stx_parse("expr", str_concat(str_concat("\"", stx_text(stx_arg(stx_call(), 0))), "\"")); name_of(hello)' \
  '"hello"'
expect_eval "stx_text_joins_terminals" \
  'macro t => stx_int(str_len(stx_text(stx_arg(stx_call(), 0)))); t(foo + 12)' "8"
expect_eval "stx_text_string_literal_keeps_quotes" \
  'macro t => stx_int(str_len(stx_text(stx_arg(stx_call(), 0)))); t("ab")' "4"

# Step 11 — MLite (`examples/langs/mlite.lang`), M0: replaced `expr`/`stmt`
# grammar with `val` / `fun` / `fn` / `let ... in ... end` / `if` / calls.
mlite_case() {
  expect_eval "mlite_m0_$1" "$(printf '#lang mlite\n%s' "$2")" "$3"
}
mlite_case fact 'fun fact(n) = if n == 0 then 1 else n * fact(n - 1);
fact(10)' "3628800"
mlite_case zero_arg_fun 'fun main() = 42;
main()' "42"
mlite_case zero_arg_fn '(fn () => 7)()' "7"
mlite_case val_and_multi_arg_fun 'val y = 5; fun add(a, b) = a + b; add(y, 2) * 3' "21"
mlite_case let_val 'let val x = 3 in x * x end' "9"
mlite_case let_val_in_binop 'fun f(x) = let val y = x in y end + 1; f(1)' "2"
mlite_case let_fun_recursive 'let fun f(n) = if n < 1 then 0 else n + f(n - 1) in f(4) end' "10"
mlite_case fn_value 'val f = fn (a, b) => a - b; f(10, 3) - 1 - 1' "5"
mlite_case if_else_extends_right 'if 1 < 2 then 10 else 20 + 5' "10"
mlite_case top_level_mutual_recursion 'fun is_even(n) = if n == 0 then 1 else is_odd(n - 1);
fun is_odd(n) = if n == 0 then 0 else is_even(n - 1);
is_even(10) + is_odd(7)' "2"
mlite_case comments_and_trailing_semicolon '# comment
fun sq(x) =
  x * x;   # trailing
val k = sq(3);
k + 1;' "10"
mlite_case println 'fun f() = println("hi"); f()' "$(printf 'hi\n0')"
expect_error "mlite_m0_rejects_minilang_let" "$(printf '#lang mlite\nlet x = 1 in x')" "2:4: syntax error"
expect_error "mlite_m0_rejects_minilang_block" \
  "$(printf '#lang mlite\nfun f(x) = { def y = 1; y }; f(1)')" "2:11: syntax error"
expect_error "mlite_m0_missing_semicolon" "$(printf '#lang mlite\nfun f(x) = x\nf(1)')" "3:0: syntax error"
expect_error "mlite_m0_arity" "$(printf '#lang mlite\nfun f(x) = x + 1;\nf(1, 2)')" \
  "wrong number of arguments to 'f': expected 1, got 2"
# Phase-1 declarations after `expr := ...` are written in MLite (F1).
mkdir -p "$AOT_TMP_DIR/mlite"
cat > "$AOT_TMP_DIR/mlite/m0_phase1.lang" <<'LANGEOF'
#lang mlite
for_syntax fact_stx => fn (n) => if n == 0 then 1 else n * fact_stx(n - 1);
macro fact5 => stx_int(fact_stx(5));
macro twice => let val c = stx_call() in
  stx_splice(stx_parse("expr", "(__a__ + __a__)"), "__a__", stx_arg(c, 0)) end;
LANGEOF
expect_eval "mlite_m0_phase1_in_mlite_syntax" \
  "$(printf '#lang %s/mlite/m0_phase1.lang\nfact5() + twice(1)' "$AOT_TMP_DIR")" "122"

if [[ "$FAIL" -ne 0 ]]; then
  echo "tests/run_tests.sh: $PASS passed, $FAIL failed" >&2
  exit 1
fi
echo "tests/run_tests.sh: $PASS passed, 0 failed"
