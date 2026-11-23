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
' "expected"

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

expect_error "review2_non_int_list_rejected" \
  '["a", "b"]' \
  "error:"

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

if [[ "$FAIL" -ne 0 ]]; then
  echo "tests/run_tests.sh: $PASS passed, $FAIL failed" >&2
  exit 1
fi
echo "tests/run_tests.sh: $PASS passed, 0 failed"
