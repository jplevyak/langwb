# Step 5 Showcase: Lexically Scoped Syntax (`using syntax`) + IFA-Disambiguated GLR (`Amb`)
#
# 1. `ext_inc` and `ext_scale` both define `'op' expr` with conflicting semantics,
#    yet coexist safely in the same expression via `using syntax ... in { ... }`.
# 2. `at_rec` and `at_int` intentionally make `'@' expr` GLR-ambiguous; DParser
#    emits an `Amb` node and IFA's whole-program type analysis disambiguates `@p`
#    (record field `.x`) vs `@n` (integer `+ 10`) in the same program.

syntax_ext ext_inc {
  grammar {
    expr   += op_inc core(passthrough);
    op_inc  : 'op' expr;
  };
  syntax_rule form op_inc(_, $x:expr) => "($x + 1)";
};

syntax_ext ext_scale {
  grammar {
    expr     += op_scale core(passthrough);
    op_scale  : 'op' expr;
  };
  syntax_rule form op_scale(_, $x:expr) => "($x * 10)";
};

grammar {
  expr   += at_rec core(passthrough)
          | at_int core(passthrough);
  at_rec  : '@' expr;
  at_int  : '@' expr;
};
syntax_rule form at_rec(_, $e:expr) => "$e.x";
syntax_rule form at_int(_, $e:expr) => "($e + 10)";

let p = { x: 2 } in
let n = 5 in
let a = using syntax ext_inc in { op n } in
let b = using syntax ext_scale in { op (@p) } in
a + b + (@n) + 1
