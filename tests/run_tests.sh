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

if [[ "$FAIL" -ne 0 ]]; then
  echo "tests/run_tests.sh: $PASS passed, $FAIL failed" >&2
  exit 1
fi
echo "tests/run_tests.sh: $PASS passed, 0 failed"
