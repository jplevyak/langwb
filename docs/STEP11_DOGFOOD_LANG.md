# Step 11 Proposal — Dogfood Language: **MLite**

> **Goal (from [ROADMAP.md](ROADMAP.md)):** Implement one non-trivial language
> with **zero C++** under `examples/langs/`, ship a real program & test suite in
> `tests/run_tests.sh`, and record every workaround and missing capability so
> that subsequent roadmap work (Steps 12–13 and beyond) is driven by real
> language-authoring friction rather than guesswork.

---

## 0. Approach: Programs First, Features Second

The point of Step 11 is to discover friction, not to tour every workbench
mechanism. MLite is therefore driven by **target programs written first**
(§3); a construct enters `mlite.lang` only when a target program needs it.

Mechanisms that no target program needs organically (`hook`, `syntax_ext`,
`check`, possibly `prim`) are still exercised — the roadmap asks for them — but
are tagged **coverage** rather than **organic** in the feature matrix and the
friction log, so that "we needed it and it hurt" is never confused with "we
forced it in and it hurt".

Work proceeds in milestones (§6); each milestone ends with its friction-log
entries written, not deferred to the end.

---

## 1. Language Choice: Why **MLite** (over a Small Scheme or Toy DSL)?

| Candidate | Strengths | Weaknesses as a `langwb` stress test |
|---|---|---|
| **Small Scheme (`mscheme`)** | Great test of hygienic `syntax-rules` macros, closures, tail/mutual recursion, and list processing. | Uniform S-expression grammar barely exercises DParser's GLR/precedence/associativity engine, `field_lhs` extension, or surface-syntax disambiguation. |
| **Narrow Domain DSL** (e.g. SQL or regex) | Exercises custom grammar and Stage-1 specialization. | Too small to stress multi-file `import`/`export`, ADTs, general-purpose control flow, closures, and AOT standalone binaries. |
| **MLite (Small ML-family language)** | Rich, non-trivial surface syntax *plus* ADTs, pattern matching, pipelines, imperative escape hatches, hygienic macros, Stage-1 specialization, C FFI, and a multi-file stdlib. | Slightly larger grammar than Scheme, which is *the point*: it tests whether a real surface language can be written cleanly in `.lang` alone. |

**MLite** is a compact, expression-oriented ML-family language (inspired by
OCaml / Standard ML / Elixir pipelines) implemented **entirely in
`examples/langs/mlite.lang`** (no MLite-specific C++; see §6.2), with a small
multi-file standard library under `examples/mlite/std/` and the target programs
under `examples/mlite/`.

---

## 2. Grammar Strategy: **Replace**, Not Extend

This is the decision everything else depends on.

**Facts about the workbench today:**

- `#lang` modules always load on top of the default minilang grammar
  (`load_lang` runs inside a `Workbench` initialised by `init_default_minilang`);
  there is no way for a `#lang` to start from the empty `--bare` grammar the way
  `prelude.lang` does. Replacement must use `grammar <nt> := ...`.
- `GrammarDef::clear_production` (`src/parse/grammar_def.cc`) clears a
  production's alternatives in place without reordering `productions_`, so
  `expr := ...` preserves `expr` as DParser's start non-terminal.
- Declaration bodies (`macro`, `form_macro`, `stage1_macro`, `check`, `hook`,
  `prim ... transfer`, `for_syntax`) and unquoted `syntax_rule` templates are
  parsed eagerly with the **active guest grammar at the point of declaration**
  (`Workbench::parse_and_apply_decl_prefix`).
- Previously, `Workbench::wrap_for_syntax_stx` wrapped every macro/check/hook/
  transfer body by reparsing `"let " + name + " = 0 in 0"` against the active
  guest grammar, which broke all post-`:=` declarations whenever any
  `for_syntax` helper existed. That general-purpose bug is fixed as M0 prep
  under §6.2 and logged as **FL-1** (§9).

**Options:**

| | Extend (`expr += ...`) | Replace (`expr := ...`) |
|---|---|---|
| User programs | Can also write `fn(x) =>`, `{ def ... }`, `match e { }`, `unless`, `aif` — MLite is a superset of minilang, not a separate language. | Only MLite syntax is accepted. |
| Phase-1 code in `mlite.lang` | Written in minilang. | Declarations placed after the `:=` are written in **MLite** — the language author dogfoods their own language for macros too. |
| Diagnostics | Leak minilang constructs. | Expose exactly where lowering leaks (Step 12 input). |
| Effort | Low. | Higher; surfaces friction items F1–F3 (§5) on day one. |

**Decision: Replace.** `mlite.lang` is laid out as:

1. A short preamble of declarations written in minilang before the grammar is
   replaced (only items genuinely needed before MLite's grammar exists, if any).
   Keep this as small as possible and log every item that *had* to go here.
2. `grammar { expr := ...; stmt := ...; ... }` — the MLite grammar, written on
   a **flat `expr` non-terminal** using DParser `$left` / `$right` /
   `$unary_right` modifiers and inline token regexes (`"[a-zA-Z_][a-zA-Z0-9_]*"`,
   `"[0-9]+"`). Pre-existing non-terminals in `grammar_def_` (`args`, `params`,
   `fields`, `field_init`, `stmts`, `field_lhs`, `raw_block`) stay reusable as
   CST building blocks (`ident`/`number`/`str` belong to `decl_grammar_`, not
   `grammar_def_`), while `expr` and `stmt` are completely replaced via `:=`.
   (`match_expr` cannot be reused; see §4.1 and F3.)
3. All remaining `for_syntax` helpers, macros, checks, hooks, `prim`s, and
   `syntax_ext` declarations, written in MLite. (Note: `load_lang` only accepts
   `.lang` declarations, not runtime `fun` statements — runtime prelude
   functions like `safe_div`/`assert` are injected via `hook module_begin` or a
   stdlib module; see §4.3, §4.4, and F8.)

If Replace turns out to be blocked (not merely painful), fall back to Extend,
and record the blocker as the top friction-log entry.

---

## 3. Target Programs (Written Before `mlite.lang`)

These are written first, in MLite as we want it to look, and drive the feature
set. Each has a golden expected stdout checked by the test suite.

| Program | What it forces | Expected-output style |
|---|---|---|
| `expr_eval.mlite` | ADTs, nested `case`, `Result`/`Option` plumbing, recursion, `\|>`, multi-file imports, Stage-1 `fast_pow` on constant exponents. | Prints each evaluated expression and its `Ok`/`Err` result. |
| `wordcount.mlite` | Strings (`str_len`, `str_slice`, `str_concat`, `int_to_str`), `MList`, `while`/`for` loops, `var`/`:=`, records, anonymous `fn (...) =>`, `and`/`or`, stdlib `list` folds. | Prints word → count pairs for an embedded text. |
| `graph_cycle.mlite` | Mutable ADT/record fields, closures capturing `var`s (auto-boxing), cycle printing (`<cycle>`), heterogeneous split-clone unions (`Cons(1, Nil)` vs `Cons("a", Nil)`). | Prints a cyclic graph and a closure counter's history. |
| `aot_cli.mlite` | Standalone AOT build (`langwb build`), `prim`/`c_call` (`llabs` and standard C `clock()`). | Deterministic portion printed; timing value only checked for non-negative `int64`. |

### 3.1 Sample: `examples/mlite/expr_eval.mlite`

```ml
#lang mlite
import "std/result.mlite";

type Expr =
    Lit(n)
  | Add(lhs, rhs)
  | Mul(lhs, rhs)
  | Div(lhs, rhs)
  | Cube(base);

fun eval_expr(e) =
  case e of
    Lit(n)       => Ok(n)
  | Add(l, r)    =>
      case eval_expr(l) of
        Err(m) => Err(m)
      | Ok(lv) =>
          case eval_expr(r) of
            Err(m) => Err(m)
          | Ok(rv) => Ok(lv + rv)
          end
      end
  | Mul(l, r)    => ...
  | Div(l, r)    =>
      case eval_expr(r) of
        Err(m) => Err(m)
      | Ok(rv) =>
          if rv == 0 then Err("division by zero")
          else
            case eval_expr(l) of
              Err(m) => Err(m)
            | Ok(lv) => Ok(lv / rv)
            end
      end
  | Cube(b)      =>
      case eval_expr(b) of
        Err(m) => Err(m)
      | Ok(bv) => Ok(fast_pow(bv, 3))   # constant exponent -> Stage-1 unroll
      end
  end;

fun main_result() =
  let val prog = Add(Mul(Lit(3), Lit(10)), Div(Lit(24), Lit(2))) in
    prog |> eval_expr |> unwrap_ok_or(0)
  end;

main_result()
```

The nested `Err(m) => Err(m)` boilerplate is deliberate: it is the input for
friction hypothesis H3 (whether a `try`/`?` macro or nested patterns are
needed).

---

## 4. Feature Matrix & Workbench Justifications

Each row is tagged **O** (organic — a target program needs it) or **C**
(coverage — included to exercise a roadmap-named mechanism). Coverage rows may
be cut (§6.1).

### 4.1 Surface Syntax & Custom Grammar

| Tag | MLite Construct | Syntax | Mechanism / Known Risk |
|---|---|---|---|
| O | Value & function bindings | `val x = e;` / `fun f(x, y) = e;` / `fun f() = e;` | Cannot be a `syntax_rule` (templates validate only as `expr`), **but works as pure `core(...)` grammar rules**: `stmt := 'val' "[a-zA-Z_][a-zA-Z0-9_]*" '=' expr $right 0 core(define, bind=1, rhs=3) \| 'var' "[a-zA-Z_][a-zA-Z0-9_]*" '=' expr $right 0 core(define, bind=1, rhs=3) \| 'fun' "[a-zA-Z_][a-zA-Z0-9_]*" fun_rhs core(define, bind=1, rhs=2) \| expr core(passthrough, inner=0);` with `fun_rhs : '(' params? ')' '=' expr $right 0 core(lambda, bind=1, body=4)`. **0-arg trap:** `collect_params` (`bridge.cc`) treats any non-`,` terminal under `bind` as a parameter name, so `'(' ')' '=' expr` with `bind=0` would bind `"("` as a parameter; an optional `params?` at `bind=1` avoids this (an empty `params_opt : params \| ;` alternative is not expressible in `.lang` grammar declarations; see FL-5), paired with `expr '(' ')' core(app, fn=0)` for 0-arg calls. **Mutability gap:** `core(define)` prevents `:=` on `fun` (`def_fn_binders`), not on `val` — enforce via `check` or log in §9. See F2. |
| O | Anonymous functions | `fn (x, y) => e` / `fn () => e` | Expression-level closures via `'fn' '(' params? ')' '=>' expr core(lambda, bind=2, body=5)`; needed for higher-order `std/list.mlite` calls (`map_l`, `filter_l`, `fold_l`) and captured-`var` closures in `graph_cycle.mlite`. |
| O | Local `let ... in ... end` | `let val x = e1; val y = e2 in e3 end` | Single binding lowers directly via `core(let)` (or `let fun f(x) = e1 in e2 end` via `core(letrec)`); multi-binding `let` desugars via `form_macro` into nested `let`s. |
| O | Explicit blocks `do ... end` & loops | `do s1; s2; e end`, `while c do body end` | `do ... end` lowers to `core(seq)` (freeing `{ ... }` exclusively for record literals); `while ... do ... end` lowers to `core(while)`. |
| O | `case ... of ... end` | `case e of None => 0 \| Some(v) => v end` | **`match` is not a core form**, and rewriting `case` into `match_expr` via `syntax_rule` fails for three reasons: (1) `syntax_rule` validates/expands templates as `expr`, so `"match $e { ... }"` cannot parse once `match_expr` is removed from `expr`; (2) `match_expr`'s C++ expander (`workbench.cc`) reparses hardcoded minilang `"let __v__ = __m__ in __b__"` templates against the active grammar; (3) `syntax_rule form` variadics match direct CST children and do not flatten DParser's nested `('\|' case_arm)*` repeater node. **Primary route:** `form_macro case_expr` + recursive `for_syntax` helper emitting MLite `let val` / `if` over `__wb_is_variant(scrut, "V", arity, "Pat")` and `__wb_variant_field`. Note: unknown-variant, private-variant, and pattern-arity diagnostics in `bridge.cc` attach directly to `__wb_is_variant` (only scrutinee type-mismatch location/labeling checks `origin_macro == "match_expr"`). See F3. |
| O | Pipeline `\|>` | `x \|> f`, `x \|> f(y, z)` | Left-associative infix; `form_macro` inspecting the RHS (`ident` vs `app`) to produce `f(x)` / `f(x, y, z)` with `x` evaluated first. |
| O | `and` / `or` / `not` | `a and b`, `a or b`, `not a` | `and`/`or` bind below comparisons; `not` is prefix at level 8 like unary `-` (ML-style: `not a == b` is `(not a) == b`; write `not (a == b)`), see §4.1.1. Hygienic desugar to `if ... then ... else ...` (short-circuiting RHS). Unary `not`/`-` tests DParser unary/binary disambiguation (H1). |
| O | Counted loop | `for i = lo to hi do body end` | Hygienic desugar to `let val` + `var` + `while ... do ... end` + `:=`; `hi` evaluated once. |
| O | Mutation | `var x = 0; x := x + 1; r.f := 2` | `core(assign)`, `core(set_field)` + replaced `field_lhs`, auto-boxing for captured `var`s. |

#### 4.1.1 Precedence, Disambiguation, Semicolons, & Re-declared Core Rules

Because `DynamicGrammar::parse` rejects ambiguous GLR parses, `expr := ...`
must give an unambiguous precedence and associativity to every operator and
open-ended form:

| Level (low → high) | DParser Modifier | Forms / Operators |
|---|---|---|
| 0 | `$right 0` (open prefix / rhs) | `fn ( params? ) => expr`, `if expr then expr else expr`, `"[a-zA-Z_][a-zA-Z0-9_]*" ':=' expr`, `field_lhs '.' "[a-zA-Z_][a-zA-Z0-9_]*" ':=' expr` |
| 1 | `$binary_op_left 1` (on op production) | `expr \|> expr` |
| 2 | `$binary_op_left 2` (on op production) | `expr or expr` |
| 3 | `$binary_op_left 3` (on op production) | `expr and expr` |
| 4 | `$binary_op_left 4` (on op production) | `==`, `!=` |
| 5 | `$binary_op_left 5` (on op production) | `<`, `<=`, `>`, `>=` |
| 6 | `$binary_op_left 6` (on op production) | `+`, `-` |
| 7 | `$binary_op_left 7` (on op production) | `*`, `/`, `%` |
| 8 | `$unary_op_right 8` (on op production) | `- expr`, `not expr` |
| 9 | `$binary_op_right 9` (`syntax_ext power` op production) | `**` |
| 10 | `$left 10` (postfix) | `expr ( args )`, `expr ( )`, `expr . "[a-zA-Z_][a-zA-Z0-9_]*"` |
| — | Closed delimiters / atoms | `"[0-9]+"`, `"\"([^\"\\]\|\\[^])*\""`, `"[a-zA-Z_][a-zA-Z0-9_]*"` (`true`/`false`), `( expr )`, `{ fields }`, `let ... in ... end`, `do stmts end`, `case ... of ... end`, `while ... do ... end`, `for ... do ... end`, `using syntax "[a-zA-Z_][a-zA-Z0-9_]*" in raw_block` |

**Structural invariants & rules wiped by `expr := ...` that `mlite.lang` must maintain:**

- **Flat `expr` + inline identifier regex (do NOT stratify `expr -> term -> factor` or wrap identifiers in an `id_tok` non-terminal):**
  1. `collect_call_args` (`expander.cc`, `bridge.cc`) collects call arguments by checking `node->symbol == "expr"`.
  2. Both `syntax_rule` (`apply_splices` in `expander.cc`) and `stx_splice` (`splice_in_place` in `meta_abi.cc`) match expression placeholders only when `expr` directly holds a single terminal child (`expr -> terminal`), so wrapping `"[a-zA-Z_][a-zA-Z0-9_]*"` in a helper non-terminal leaves a stale `core(ident)` wrapper around spliced non-identifier expressions.
- **Precedence on macro-lowered operators (`|>`, `or`, `and`, `**`, unary `-`, `not`):**
  Because `CoreFormKind` has no `UnaryOp` and custom infix/prefix forms dispatch
  by CST non-terminal name (`expr += pipe_e core(passthrough);`), rule-level
  `$left N` / `$right N` cannot make sub-productions binary-left: DParser
  (`dparser/gram.c` `convert_priorities`) only promotes `$left N` to
  `ASSOC_BINARY_LEFT` when `elems[0]` and `elems[n-1]` equal the rule's own LHS
  production (`pipe_e != expr`), so even putting `$left 1` on both `expr` and
  `pipe_e` leaves `10 |> 3 |> 1` right-associated as `10 |> (3 |> 1)`. Instead,
  attach DParser's explicit **operator-priority** modifiers (`$binary_op_left N`,
  `$binary_op_right N`, `$unary_op_right N`; see `dparser/docs/reference/disambiguation.md`)
  to a 1-token operator production (logged as **FL-2**; regression test
  `op_priority_subproduction_*` in `tests/run_tests.sh`):
  ```
  expr   += pipe_e core(passthrough) | neg_e core(passthrough) | pow_e core(passthrough);
  pipe_e  : expr pipe_op expr;   pipe_op : '|>' $binary_op_left 1;
  neg_e   : neg_op expr;         neg_op  : '-'  $unary_op_right 8;
  pow_e   : expr pow_op expr;    pow_op  : '**' $binary_op_right 9;
  ```
  This preserves left-associativity on `|>`, `or`, `and`, right-associativity on
  `**`, and prefix binding on unary `-` / `not`. **All** binary operators must
  use this form, including the `core(binop)` ones (`expr add_op expr
  core(binop, lhs=0, op=1, rhs=2); add_op : '+' $binary_op_left 6 | '-'
  $binary_op_left 6;`): mixed with rule-level `expr '*' expr $left 7`, a
  lower-priority operator production to the *right* of a rule-level operator
  groups wrongly (`2 * 10 |> 3` parses as `2 * (10 |> 3)`, `1 < 2 and true` as
  `1 < (2 and true)`); see FL-2.
- Atoms & grouping (`ident`, `number`, and `str` live in `decl_grammar_`, not
  `grammar_def_`): `"[0-9]+" core(datum)`, `"\"([^\"\\]|\\[^])*\"" core(string)`,
  `"[a-zA-Z_][a-zA-Z0-9_]*" core(ident)` (also required so `bridge.cc` resolves
  `true` and `false`, and because `syntax_rule` rewrites each `$name` in a
  template into an identifier-shaped placeholder before parsing the template
  with the active grammar (`Workbench::register_syntax_rule`), so without it
  every `syntax_rule` expansion fails with `form macro '<name>' failed`),
  `'(' expr ')' core(paren, inner=1)`.
- Records & field mutation: `'{' fields '}' core(record, fields=1)`,
  `expr '.' "[a-zA-Z_][a-zA-Z0-9_]*" $left 10 core(field, obj=0, field=2)`, and
  `field_lhs '.' "[a-zA-Z_][a-zA-Z0-9_]*" ':=' expr $right 0 core(set_field, obj=0, field=2, rhs=4)`
  (reusing `init_default_minilang`'s `field_lhs` receiver production for `r.f`,
  `r.a.b`, `f(x).g`, `(e).f`).
- Scoped syntax hook: `'using' 'syntax' "[a-zA-Z_][a-zA-Z0-9_]*" 'in' raw_block $right 0 core(using_syntax, bind=2, body=4)` —
  must use `raw_block` (unparsed `{ ... }` text) rather than `'{' expr '}'` so
  the inner expression is parsed *after* `using_syntax` activates the
  `syntax_ext` grammar delta (`syntax_ext power` itself must be declared *after*
  `expr := ...`).

**Semicolon conventions:**

- **Top level & imported modules:** `;` is a separator *between* top-level
  forms (`val`, `var`, `fun`, expressions); declarations (`import`, `export`,
  `type`) end with `;`; the final top-level expression in a main program has no
  trailing `;` (`workbench.cc:3062`).
- **`do ... end` blocks:** `do s1; s2; ...; sn end` (`;` between statements/
  final expression).
- **`let ... in ... end`:** `let val x = e1; val y = e2 in body end` (`;`
  between bindings, optional single binding without `;`).

### 4.2 Algebraic Data Types, Records, & Built-in Carryover (O)

- `type Option = None | Some(val);`, `type Result = Ok(val) | Err(msg);`,
  recursive user ADTs. `type` is a workbench-level declaration (`decl_type`)
  and works unchanged under a replaced expression grammar.
- Exercises per-variant `Type_RECORD` classes, SSU narrowing (`Ok(int64)` vs
  `Err(string)`), nullary constructor equality, `_wb_print_<id>` cycle-safe
  printing, and `WbSymbol::copy` on IFA-split record classes.
- **Inherited built-ins under `#lang` (facet of F7):** Because `load_lang`
  starts from `init_default_minilang`, MLite inherits:
  - Built-in I/O and string primitives (`print`, `println`, `str_concat`,
    `str_len`, `str_eq`, `str_slice`, `int_to_str`, `str_to_int`), which
    `wordcount.mlite` uses directly.
  - *(Correction, M2: there is no pre-registered `type List = Nil | Cons(...)`;
    minilang lists are `[...]` literals over the `cons` / `head` / `tail`
    primitives, which MLite's grammar does not expose. `graph_cycle.mlite`
    declares its own `type List`, and `std/list.mlite` keeps the `MList` /
    `MNil` / `MCons` names so it can be imported next to a program's own
    `List`, since variant names are global.)*

### 4.3 Hygienic Macros, `#lang` Hooks, & Scoped Syntax

- **(O) Stage-0 `syntax_rule` / `form_macro` macros:** `and`, `or`, `not`,
  `for`, `|>`, `unless`. Verifies set-of-scopes hygiene, single evaluation, and
  transitive private helper macros across modules.
- **(C) `#lang` hook — pick exactly one: `hook module_begin`** that injects a
  dedicated MLite prelude binding (e.g. `mlite_id`, decoupled from
  `std/result.mlite` so cutting `module_begin` under §6.1 never breaks target
  programs). Two workbench behaviors to verify and log:
  1. `import_module_file` calls `expand(..., run_module_begin=false)`, so
     `module_begin` runs **only on the entry module**, not inside imported
     `.mlite` modules.
  2. Multi-statement entry files pass a synthetic `CoreFormKind::Seq`
     (`symbol = "module"`) to `module_begin`, whereas single-expression files
     pass an `expr` CST node.
  (`hook app` is rejected: it also fires on calls produced by macro expansion,
  including MLite's own desugarings.)
- **(C) Scoped syntax extension — pick exactly one: `syntax_ext power`**
  (declared *after* `expr := ...`) adding right-associative `**` exponentiation
  at precedence level 9 (`$binary_op_right 9` on `pow_op`: tighter than `*` and
  unary `-`, so `-x ** 2` is `-(x ** 2)` while `2 ** -3` is `2 ** (-3)`),
  activated via `using syntax power in { ... }` and lowering to `fast_pow`.
  (`|x|` absolute value is rejected: it collides with `|` case arms.) Exercises
  `SyntaxExtDelta` replay and lexical non-leakage.

### 4.4 IFA-Driven Stage-1 Macros & Static Checks

- **(O) `stage1_macro fast_pow(base, exp)`** — a zero-C++ `.lang` port of the
  built-in C++ `pow_opt` (`workbench.cc`). Uses `attr_is_const_i64(exp)` /
  `attr_const_i64(exp)` to unroll constant exponents `0..8` into
  `let val t = base in t * t * ... end` (single evaluation of `base`), else
  falls back to a runtime loop. Building an N-ary product uses a recursive
  `for_syntax` helper over `stx_parse` / `stx_splice` (see F5 and FL-1). Called
  through wrappers (`fun cube(x) = fast_pow(x, 3)`) to exercise contour-aware
  Stage-1 cloning.
- **(C) `check call safe_div`** — rejects a divisor with
  `attr_is_const_i64(rhs) && attr_const_i64(rhs) == 0`.
  Scope of the claim: direct constants, constants through `val` bindings and
  `if` branches that fold. Zero propagating *through a helper call* is caught
  only if IFA gives the helper a separate contour for that call; when `h(0)`
  and `h(5)` share a contour the constant merges and the check misses it unless
  it uses `attr_contour_is_const_i64` / `attr_contour_const_i64`. The test
  suite records the actual behavior for both cases rather than asserting it.
- **`assert` and `safe_div` are runtime functions, not macros.** `check` runs
  on the Stage-0 *expanded* tree, so `check call assert` / `check call safe_div`
  would never see a `syntax_rule`. Because `.lang` files cannot contain runtime
  `fun` statements directly (`load_lang` only parses `.lang` declarations; see
  F8), `safe_div` and `assert` are provided either via `hook module_begin`
  (entry modules) or in an imported stdlib module (e.g. `std/result.mlite`),
  paired with `check call` rules in `mlite.lang`.

### 4.5 Foreign Function Interface (C)

- `prim` + `c_call` for standard C `llabs` (1 arg, pure; transfer function
  constant-folds via `fa_arg_is_const_i64` / `fa_emit_const_i64`) and standard C
  `clock` (`prim clock_ticks c_call "clock"`, 0 args, impure; transfer emits
  `fa_emit_i64()`).
- **Known limit:** transfer functions can only emit `int64`
  (`fa_emit_i64` / `fa_emit_const_i64`), so string- or record-returning C
  functions are out of scope. Log any program that wanted one.

### 4.6 Multi-File Standard Library (O)

```
examples/langs/mlite.lang        # #lang mlite: grammar (replaced), macros, checks, prims, hook
examples/mlite/
├── std/
│   ├── option.mlite             # type Option, map_opt, unwrap_or, is_some
│   ├── result.mlite             # type Result, map_ok, and_then, unwrap_ok_or, ok_opt, show_result, ok_or macro (imports option)
│   └── list.mlite               # type MList (MNil | MCons), map_l, filter_l, fold_l, rev_l, range_l, len_l
├── expr_eval.mlite
├── wordcount.mlite
├── graph_cycle.mlite
└── aot_cli.mlite
```

- Imported modules read their own `#lang` header, so each stdlib file starts
  with `#lang mlite`. Import specs must include the `.mlite` extension (the
  resolver only auto-appends `.ml` / `.lang`).
- Exercises relative imports, diamond imports (`expr_eval` → `result` →
  `option`, plus `expr_eval` → `option`), private helper hiding (private
  `fun`s and private ADT constructors), exported macros with transitive
  private helpers, and exporting `fun`/`val`-produced `core(define)` bindings.

---

## 5. Friction Hypotheses

### 5.1 Already known from reading the workbench (verify, then log)

- **F1 — Declaration ordering & `for_syntax` under a replaced grammar.**
  Phase-1 code after `expr := ...` must be written in MLite; anything needed
  before MLite's grammar exists must precede it in minilang. (`wrap_for_syntax_stx`
  also hardcoded minilang `let` syntax — fixed in M0 prep; see FL-1.)
- **F2 — Statement bindings (`fun`/`val`/`var`) & `val` mutability.**
  `syntax_rule` templates validate only as `expr`, so `fun`/`val`/`var` must use
  `stmt` grammar rules with `core(define)` + a `fun_rhs` `core(lambda)` helper
  over `params?` (avoiding the 0-arg `collect_params` terminal trap); also,
  `core(define)` only blocks `:=` on `fun` bindings, not non-lambda `val`s.
- **F3 — `match` is a C++ macro coupled to minilang `let` syntax, not a core form.**
  `match_expr` cannot be targeted after replacing `let`, and `syntax_rule form`
  variadics do not flatten DParser `('|' arm)*` repeaters, so `case` must use a
  `form_macro` emitting `__wb_is_variant` / `__wb_variant_field` directly.
- **F4 — Checks see expanded code.** Macros cannot be targets of `check call`.
- **F5 — No Meta-ABI for building variable-length syntax.** N arms, N-ary
  products, multi-binding `let`, etc. require recursive string/template-splice
  helpers.
- **F6 — Transfer functions are `int64`-only.**
- **F7 — `#lang` cannot start from a bare grammar.** Pre-registered minilang
  primitives remain in scope (no ADTs are pre-registered; see the §4.2
  correction).
- **F8 — `#lang` files cannot contain runtime `fun`/`val` definitions.**
  `Workbench::load_lang` (`workbench.cc:382`) accepts only `.lang` declarations,
  not runtime statements, so language-provided runtime helpers (`safe_div`,
  `assert`) must be injected by `hook module_begin` or imported from a stdlib
  module.

### 5.2 Hypotheses to test

1. **H1 Unary operators (`-x`, `not x`)** vs binary `a - b` and open `if` /
   `fn` forms under DParser precedence (§4.1.1).
2. **H2 Error messages in MLite's terms:** how much minilang / IF1 lowering
   leaks for a bad `case` arm, non-ADT `case` scrutinee, arity error, or type
   error (Step 12 input).
3. **H3 Nested patterns & `Result` ergonomics:** is a `try`/`?` macro or nested
   patterns needed (see §3.1)?
4. **H4 Top-level mutual recursion** of `fun f ...; fun g ...;` through
   `core(define)` in the top-level `Seq`.
5. **H5 Import paths** consistent across `langwb <file>`, `langwb build`, and
   `langwb --lsp`.
6. **H6 LSP on macro-expanded forms:** hover/definition on `fun`, `val`,
   `case`, `|>` after Stage-0 expansion.

---

## 6. Milestones

Each milestone: make the relevant target programs (or slices of them) pass,
add their tests, and write friction-log entries before moving on.

| Milestone | Scope | Exit criterion |
|---|---|---|
| **M0** ✅ | Replaced grammar skeleton (§4.1.1); `val`/`fun`/`fn`/`let`/`if`/calls/arithmetic; verify F1/F2 routes. | `fun fact(n) = ...; fact(10)` and 0-arg `fun main() = ...; main()` run under JIT. **Done:** `examples/langs/mlite.lang`; tests `mlite_m0_*`; F1 (Phase-1 code in MLite syntax) and F2 (`stmt` + `core(define)` + `fun_rhs`) routes confirmed; FL-3..FL-5. |
| **M1** ✅ | `type` + `case` (`form_macro case_expr` route). | `expr_eval.mlite` minus `fast_pow` and imports. **Done:** `case` grammar + guest `form_macro case_expr` in `mlite.lang`; `examples/mlite/expr_eval.mlite` (+ `.expected`, `\|>` / imports / `fast_pow` pending); tests `mlite_m1_*`; F3 and F5 confirmed; FL-6..FL-8. |
| **M2** ✅ | `var`/`:=`/`while`/`for`/`do`/`\|>`/`and`/`or`/`not`, records, strings. | `wordcount.mlite`, `graph_cycle.mlite`. **Done:** both programs (+ `.expected`); `expr_eval.mlite` now uses `\|>` / unary `-`; tests `mlite_m2_*`; FL-2 corrected (all binary operators as operator-priority productions), FL-9..FL-11. |
| **M3** ✅ | Multi-file stdlib, exports, private helpers. | `expr_eval.mlite` with imports; negative unexported-helper test. **Done:** `examples/mlite/std/{option,result,list}.mlite` (`result` imports `option`; private `show_tagged` / `pick_ok` / `range_acc`; exported `syntax_rule ok_or` with a private helper); `expr_eval.mlite` imports `result` + `option` (diamond), `wordcount.mlite` imports `list`; tests `mlite_m3_*`; FL-12..FL-16 (FL-12..FL-14 fixed in C++, each with a minilang-only test). |
| **M4** ✅ | `fast_pow`, `safe_div`, `assert`, `prim`s (`llabs`, `clock`), `hook module_begin`, `syntax_ext power`. | All target programs; negative static-check tests. **Done:** M4 section of `mlite.lang` (`stage1_macro fast_pow`, `check call safe_div` / `assert`, `prim abs_i` / `clock_ticks` / `exit_with`, `hook module_begin` binding `mlite_version`, `syntax_ext power` + `using syntax`); `safe_div` in `std/result.mlite`, new `std/assert.mlite`; `expr_eval.mlite` uses `fast_pow` (via `cube`), `safe_div`, `assert`; new `examples/mlite/aot_cli.mlite` (JIT; AOT in M5); tests `mlite_m4_*`; FL-17..FL-19 (FL-17 and FL-19 fixed in C++). |
| **M5** | AOT + LSP. | `aot_cli.mlite` standalone binary; LSP diagnostics/hover/definition test. |

### 6.1 Timebox & Cut List

Timebox Step 11 so that it produces a friction log rather than an ever-growing
language. If time runs short, cut in this order (coverage first, never cut the
log): `syntax_ext power` → `hook module_begin` → `check call assert` →
`clock` `prim` → `for` loop. M0–M3 plus `fast_pow` and `safe_div` is the minimum
shippable Step 11.

### 6.2 C++ Change Policy

"Zero C++" means no MLite-specific C++. A C++ change is allowed only if it
fixes a genuine general-purpose workbench bug or gap, and then it must:

1. land as its own commit, separate from `mlite.lang` work;
2. have a friction-log entry explaining what blocked MLite;
3. have a regression test that does **not** depend on MLite.

---

## 7. Friction Log Format

The log lives in §9 of this document (appended as work proceeds) and is
summarized into [ROADMAP.md](ROADMAP.md) at the end of Step 11. One entry per
issue:

```
### FL-<n>: <short title>
- Milestone: M<k>            Tag: organic | coverage
- Symptom: what the language author hit (exact error text if any)
- Workaround: what mlite.lang does instead (or "blocked")
- Proposed fix: workbench change that would remove the workaround
- Target: Step 12 | Step 13 | Deferred | fixed-in-Step-11 (<commit>)
```

---

## 8. Acceptance Criteria for Step 11

1. **`examples/langs/mlite.lang` implemented with a replaced grammar** (or, if
   Replace was blocked, Extend with the blocker logged) and no MLite-specific
   C++ (§6.2).
2. **Multi-file stdlib + the four target programs** under `examples/mlite/`,
   covering every **O** row of §4 and every **C** row not cut under §6.1.
3. **End-to-end tests in `tests/run_tests.sh`**, each checking exact stdout and
   exit status:
   - JIT (`langwb <file>`) for every target program;
   - AOT (`langwb build <file> -o ...`) for at least `aot_cli.mlite` and
     `expr_eval.mlite`;
   - LSP (`langwb --lsp`): diagnostics, hover, and definition on an MLite file;
   - negative tests (static `safe_div` by zero, unexported stdlib helper, bad
     `case` arity, statically false `assert`) that check the **error text**,
     recorded verbatim as the Step 12 baseline.
4. **Friction log complete** (§7 format, §9 location), every entry tagged
   organic/coverage and given a target step, and summarized into
   `docs/ROADMAP.md` as the input for Steps 12–13.

---

## 9. Friction Log

### FL-1: `for_syntax` hardcoded minilang `let` surface syntax and lacked direct recursion
- Milestone: M0 (pre-flight audit)            Tag: organic
- Symptom: `Workbench::wrap_for_syntax_stx` (`src/workbench/workbench.cc`) wrapped every `macro`, `form_macro`, `stage1_macro`, `check`, `hook`, and `prim ... transfer` body by calling `gram.parse("let " + name + " = 0 in 0", "expr")` against the active guest grammar. Once `expr := ...` replaced minilang's `let x = e1 in e2` with MLite's `let val x = e1 in e2 end`, `gram.parse(...)` returned `nullptr` and caused every subsequent macro/check/hook/transfer declaration to fail whenever any `for_syntax` helper existed. Additionally, `wrap_for_syntax_stx` always emitted `CoreFormKind::Let`, preventing `for_syntax` lambda helpers (needed for recursive `case` arm lowering and `fast_pow` unrolling) from calling themselves **directly** by their `for_syntax` name (`for_syntax f => fn(n) => ... f(n - 1)`).
- Workaround: Keep minilang's `'let' ident '=' expr 'in' expr` rule (with `core(let, ...)`) alive in the replaced `expr` grammar (leaking minilang surface syntax into MLite), and for recursive helpers wrap an inner `core(letrec)` form inside the helper's RHS (`for_syntax f => rec go = fn(n) => ... go(n - 1) in go;`) if the guest `expr` grammar exposes one.
- Proposed fix: Construct synthetic `CoreFormKind::Let` / `CoreFormKind::LetRec` `SyntaxObj` nodes directly in `Workbench::wrap_for_syntax_stx` without reparsing surface text through `DynamicGrammar`, choosing `LetRec` when the helper's RHS is a lambda (through `paren` / `passthrough`) and `Let` otherwise.
- Behavior change: a lambda helper that reuses an earlier helper's name now refers to itself rather than to the earlier binding (e.g. `for_syntax f => fn(x) => f(x) + 1` after a prior `f` recurses forever). Non-lambda helpers keep `Let` shadowing (`for_syntax k => k + 41` sees the previous `k`). Give lambda helpers distinct names.
- Target: fixed-in-Step-11 (`508ccfe`); tests `for_syntax_*` in `tests/run_tests.sh`

### FL-2: Rule-level `$left N` on an operator sub-production silently gives right associativity
- Milestone: M0 (pre-flight audit)            Tag: organic
- Symptom: Macro-lowered infix operators (`|>`, `or`, `and`, `**`) must live in their own non-terminal so a `form_macro` / `syntax_rule form` can dispatch on it (`expr += pipe_e core(passthrough); pipe_e : expr '|>' expr $left 1;`). DParser's `convert_priorities` (`dparser/gram.c`) only turns `$left N` into a binary-left rule when the rule's first and last elements are its own LHS non-terminal; `pipe_e`'s are `expr`, so the rule is not treated as binary and `10 |> 3 |> 1` parses as `10 |> (3 |> 1)`, even with `$left 1` on both the `expr` alternative and `pipe_e`. No error or warning is produced. (Right-associative `**` looks correct only because the default grouping happens to match.)
- Workaround: Attach DParser operator priorities to a 1-token operator production instead: `pipe_e : expr pipe_op expr; pipe_op : '|>' $binary_op_left 1;` (likewise `$binary_op_right` for `**`, `$unary_op_right` for prefix `-` / `not`). **Correction (M2):** they do *not* mix with rule-level `$left N` on `core(binop)` rules: a rule-level operator to the left of a lower-priority operator production binds too loosely (`2 * 10 |> 3` → `2 * (10 |> 3)`; MLite hit it as `1 < 2 and true` → `1 < (2 and true)`, an `if condition (expected bool)` error), while the other direction (`20 |> 2 * 3`) happens to work. Write every binary operator, `core(binop)` ones included, as `expr op_nt expr` with operator priorities on `op_nt` (the `op` role then points at the operator non-terminal, whose text is the operator).
- Proposed fix: Have the workbench warn when a `$left N` / `$right N` rule priority is attached to a rule that DParser cannot classify as binary or unary (no self-recursive end element), pointing at the operator-priority form; or document the operator-priority form in the `.lang` grammar reference.
- Target: Step 12 (diagnostic); tests `op_priority_subproduction_*` and `op_priority_uniform_*` in `tests/run_tests.sh`

### FL-3: Malformed top-level forms reported at the wrong place (or as "expected ';'")
- Milestone: M0            Tag: organic
- Symptom: Top-level forms are parsed as the longest `stmt` prefix, falling back to an `expr` prefix, and a failure was then diagnosed by re-parsing the remainder as `expr`. Because keywords are not reserved (FL-4), the `expr` fallback parses a leading `fun` / `def` as an identifier, so a broken form was blamed on the token after the keyword or reported as `expected ';' between top-level forms`: `fun f(x) = x +; f(1)` → `2:13: expected ';' between top-level forms`; `fun f(x) = { def y = 1; y }; f(1)` → `2:4: expected ';' ...`; `fun f(x) = x` newline `f(1)` (no `;` anywhere, so the single-expression path) → `2:4: syntax error`. Same in plain minilang (`def x = 1 +; x`, `def x = 1 x`).
- Workaround: none needed after the fix.
- Proposed fix: on failure, re-parse the remainder non-quietly as `stmts` (`stmt (';' stmt)*`, falling back to `stmt` / the default start), which reports the first token where the form actually breaks; only when that succeeds fall back to `expected ';' ...` (multi-form path) or `expected an expression (a program must end with one)` (single-expression path). Remaining gap: syntax errors inside an imported module still print `L:C` without the module's file name (`DynamicGrammar::parse` has no file prefix).
- Target: fixed-in-Step-11 (`DynamicGrammar::has_start_symbol`, `report_top_level_form_error` in `src/workbench/workbench.cc`); tests `top_level_error_*` in `tests/run_tests.sh`. File name on imported-module syntax errors: Step 12.

### FL-4: Keywords are not reserved
- Milestone: M0            Tag: organic
- Symptom: With scannerless DParser, a `.lang` keyword (`'fun'`, `'val'`, `'then'`, `'end'`) and the identifier regex both match, so `val then = 1; then + 1` and `val fun = 3; fun` are accepted, and every keyword is also a valid `core(ident)` expression. Besides being surprising for an ML, this is what made FL-3's diagnostics misleading.
- Workaround: none (MLite programs simply avoid keywords as names).
- Proposed fix: a `.lang`-level way to declare reserved words for an identifier terminal (e.g. a `reserved 'fun' 'val' ...;` declaration lowered to a DParser `${scan ...}`/speculative reject on the identifier regex), so keywords never parse as identifiers.
- Target: Step 12

### FL-5: Grammar declarations cannot express an empty alternative
- Milestone: M0            Tag: organic
- Symptom: The plan called for `params_opt : params | ;` so 0-arg `fun f() = ...` / `fn () => ...` bind no parameters, but `.lang` grammar alternatives require at least one element (`gram_rhs : gram_elem+`), so the empty alternative is a syntax error in the declaration.
- Workaround: `'(' params? ')'` with `bind` pointing at the optional `params?` element, which yields an element with no terminals when empty, so `collect_params` binds nothing (`fun_rhs : '(' params? ')' '=' expr core(lambda, bind=1, body=4)`, `'fn' '(' params? ')' '=>' expr core(lambda, bind=2, body=5)`). Calls still need a separate `expr '(' ')' core(app, fn=0)` rule (or `args?`).
- Proposed fix: none required; optionally accept an empty alternative (or an explicit `/* empty */`-style marker) in `gram_alt` for parity with DParser `.g` files.
- Target: Deferred

### FL-6: Phase-1 code cannot read an identifier's text as a string
- Milestone: M1            Tag: organic
- Symptom: The `case` lowering must emit `__wb_is_variant(scrut, "Ctor", arity, "Ctor(a, b)")`, whose variant name and shown pattern are **string literals** naming the pattern's constructor. The Meta-ABI could compare a node's text (`stx_text_eq`, `stx_symbol_eq`) but not return it, so a guest `form_macro` had no way to spell an arbitrary constructor name into a template; splicing the identifier node in place of the string fails because the lowering requires a quoted literal.
- Workaround: none possible in `.lang`.
- Proposed fix: `stx_text(h) -> string` (GC-allocated): an identifier's name, a literal's spelling (string literals keep their quotes), or the terminals of a larger node joined by single spaces; combined with `str_concat` and `stx_parse` it builds templates from source names.
- Target: fixed-in-Step-11 (`_CG_stx_text` in `src/ifa_bridge/meta_abi.cc`); tests `stx_text_*` in `tests/run_tests.sh`

### FL-7: Guest macros cannot give template nodes a source location
- Milestone: M1            Tag: organic
- Symptom: Nodes built by `stx_parse` carry template-relative line/col, and the ADT diagnostics on `__wb_is_variant` print `node->line:col` as if it were in the input. MLite's `case` therefore reported `<input>:1:3: error: pattern 'Some(a, b)' has 2 field(s); variant 'Some' has 1` (and the same `1:3` for unknown variants) wherever the pattern was, and a non-ADT scrutinee as `<macro:case_expr@3:0>:1:3: error: ...`. The C++ `match_expr` avoids this by assigning the pattern's line/col to its test node directly, and the scrutinee diagnostic only recognized `origin_macro == "match_expr"`.
- Workaround: none possible in `.lang`.
- Proposed fix: `stx_copy_loc(dst, src)` (in-place on the `dst` node only) in the Meta-ABI, which MLite's `case` uses to point each `__wb_is_variant` test at the pattern's constructor; and report a non-ADT scrutinee at the call site of whichever macro produced the `__wb_is_variant`. Now: `<input>:3:16: error: pattern 'Some(a, b)' ...`, `<input>:4: error: illegal primitive argument type 'match scrutinee (expected Option)' illegal: int64`. Remaining Step 12 input: the scrutinee message is IF1-flavored and has no column; a more general answer is a template API where `stx_splice` / `stx_parse` take a location source.
- Target: fixed-in-Step-11 (`_CG_stx_copy_loc` in `src/ifa_bridge/meta_abi.cc`, `at_match` in `src/ifa_bridge/bridge.cc`); tests `stx_copy_loc_*`, `guest_macro_non_adt_scrutinee_at_call_site` in `tests/run_tests.sh`. Message wording: Step 12.

### FL-8: Phase-1 ergonomics for syntax-building code
- Milestone: M1            Tag: organic
- Symptom: Writing `case` lowering in guest code (~90 lines of `mlite.lang`) hit several small costs that compound:
  1. Meta-ABI predicates return `int64`, not `bool`, so every test is `if stx_symbol_eq(p, "pat_int") == 1 then ...`.
  2. Templates are assembled with nested `str_concat` (a `cat3` helper), with `\"` escapes for every string literal that names a constructor (`case_q`).
  3. No list of syntax handles at Phase 1 (MLite's grammar has no list literal, and the arms are a DParser `('|' arm)*` repeater), so arms are consumed by a generic right-to-left tree fold (`case_fold(n, i, acc)`) that stops at `case_arm` nodes; pattern variables needed a **right-recursive** `pat_vars : pat_var | pat_var ',' pat_vars` grammar purely so guest code can walk them with `stx_child(pv, 0)` / `stx_child(pv, 2)`.
  4. Every arm template is `stx_splice(stx_splice(stx_splice(stx_parse(...), "__k__", ...), "__b__", ...), "__n__", ...)`: one call per placeholder.
  5. `for_syntax` helpers only see earlier helpers, so mutually recursive helpers must be fused into one function (avoided here by ordering).
- Workaround: the helpers above (`cat3`, `case_q`, `case_fold`, `case_bind_vars`, right-recursive `pat_vars`).
- Proposed fix: `bool`-returning Meta-ABI predicates; a multi-placeholder splice (`stx_splice_n(tmpl, "__a__", a, "__b__", b, ...)` or quasiquote with `,(expr)` holes available under replaced grammars); a Phase-1 handle list / `stx_children_of(n, "case_arm")` that flattens repeaters; mutually recursive `for_syntax` groups.
- Target: Step 13 (Meta-ABI), Deferred (mutual recursion)

### M1 notes on hypotheses
- **F3 (`match` coupled to minilang):** confirmed; the `form_macro case_expr` route works end-to-end once FL-6 (`stx_text`) and FL-7 (`stx_copy_loc`) exist, and needed no other workbench change. Scrutinee hygiene (`__m__`) and single evaluation hold.
- **H2 (errors in MLite's terms):** pattern arity and unknown variant are reported at the pattern; a non-ADT scrutinee still reads `illegal primitive argument type 'match scrutinee (expected Option)' illegal: int64` (line only); a missing case is the runtime `non-exhaustive match` with no location.
- **H3 (nested patterns / `Result` plumbing):** `expr_eval.mlite` needs 8 `Err(m) => Err(m)` arms for 5 constructors; patterns are flat (`Ctor(var, ...)`), so nested patterns or a `try`/`?` macro would remove most of that. Revisit after M3 (`std/result.mlite`'s `and_then`).

### FL-9: `syntax_rule` templates ended at the first `;` outside parens/braces
- Milestone: M2            Tag: organic
- Symptom: `macro` / `form_macro` / `check` bodies end where the guest grammar's `expr` ends, but `syntax_rule` templates were cut at the first `;` outside `()` / `{}` / strings (`find_decl_terminator`). MLite's `for` desugaring `let val hi = $hi in do var $i = $lo; while ... end end end;` was truncated after `$lo`, failing with `langwb: 3:19: syntax error` / `invalid declaration in 'examples/langs/mlite.lang' near 'syntax_rule form for_e(...'`. Any language with keyword-delimited blocks (`do ... end`, `begin ... end`) hits this.
- Workaround (before the fix): wrap the whole template in parentheses.
- Proposed fix: mask placeholders (`$x` -> `_x`, same length) over the remaining source and prefix-parse it as `expr` with the active guest grammar; the template ends there when a `;` follows. The character scan remains the fallback for reporting syntax errors.
- Target: fixed-in-Step-11 (`decl_syntax_rule_head` in `Workbench::parse_and_apply_decl_prefix`); tests `syntax_rule_template_*` in `tests/run_tests.sh`

### FL-10: `val` bindings are assignable
- Milestone: M2            Tag: organic
- Symptom: `val x = 1; x := 2; x` evaluates to `2` (likewise `let val x = 1 in x := 5 end`). `core(define)` only rejects assignment to *function* definitions (`cannot assign to function definition 'f'`); `val` and `var` lower identically, and a `check core assign` cannot tell them apart because the Meta-ABI has no way to reach an identifier's binding site (or a property set on it).
- Workaround: none; MLite programs simply do not assign to `val`s (pinned by test `mlite_m2_val_is_assignable_fl10`).
- Proposed fix: an immutability flag on binding forms (`core(define, ..., const)` / `core(let, ..., const)`) enforced by the resolver, or a Meta-ABI `stx_binder(id)` so a `check core assign` can inspect a syntax property set on the binder.
- Target: Step 12

### FL-11: Errors inside `syntax_rule` expansions report template-relative positions
- Milestone: M2            Tag: organic
- Symptom: A type error in code produced by a `syntax_rule` is reported at the template's own line/column under the macro's call-site banner, e.g. `not 1 == 2` → `<macro:not_e@2:89>:1:89: error: illegal primitive argument type 'if condition (expected bool)' illegal: int64` (the `if` exists only in the template). This is FL-7's problem for declarative macros, where there is no `stx_copy_loc` to call.
- Workaround: none.
- Proposed fix: give template nodes the call site's location (or the location of the nearest spliced pattern variable) when expanding a `syntax_rule`, and phrase `if` / `while` condition errors in terms of the macro (`operand of 'not' (expected bool)`).
- Target: Step 12

### M2 notes on hypotheses
- **H1 (unary vs binary operators):** no ambiguity once every binary operator is an operator-priority production: `- 3 * 2 + 10 % 4`, `1 - - 2`, `- r.x`, `- inc(1)` and `not (3 < 2)` (not misparsed as a call to `not`) all group as intended; `if` / `fn` / `:=` still extend to the right.
- **H2 (errors in MLite's terms):** calling a data-first list function with swapped arguments (`filter_l(p, xs)` written for `xs |> filter_l(p)`) reports `<input>:10:2: error: illegal primitive argument type 'match scrutinee (expected MList)' illegal: __wb_fn_10` with a `called from` chain; the internal closure name `__wb_fn_10` leaks.
- **`|>` convention:** `x |> f(a)` is `f(x, a)` (Elixir-style, as planned), so MLite's list functions take the list first (`fold_l(xs, acc, f)`), unlike SML/OCaml.
- **Blocks:** `do s1; ...; e end` is `'do' stmts 'end' core(seq, inner=1)`; `while` / `for` bodies are a `block : stmts core(seq, inner=0)` non-terminal (a syntax class for `$body:block` in `for`'s `syntax_rule`).


### FL-12: Importing a module whose `#lang` matches the importer's reloaded the language on top of it
- Milestone: M3            Tag: organic
- Symptom: `#lang mlite` + `import "std/list.mlite";` (itself `#lang mlite`) failed with `langwb: production 'eq_op' already exists; use '+=' to extend or ':=' to replace it` / `invalid declaration in 'examples/langs/mlite.lang'`. An imported module's workbench was cloned from the importer, whose language was already loaded, and then loaded the module's `#lang` again; any `.lang` that declares a new production (`nt : ...`) cannot be applied twice. Multi-file programs in any non-default language hit this.
- Workaround (before the fix): none short of dropping `#lang` from library modules, which then silently inherit whatever language the importer uses.
- Proposed fix: each workbench clone remembers the pre-`#lang` base it was (transitively) cloned from; a module with its own `#lang` header starts from that base and loads its language there, while a header-less module still inherits the importer's.
- Target: fixed-in-Step-11 (`Workbench::lang_base_`, used by `Workbench::import_module_file`); tests `import_module_with*_lang_header*` in `tests/run_tests.sh`

### FL-13: A module that imported an ADT re-marked its constructors private to itself
- Milestone: M3            Tag: organic
- Symptom: `std/result.mlite` imports `std/option.mlite` (which exports `Option`) without re-exporting it; importing `result` then failed inside `option`'s own code: `examples/mlite/std/option.mlite:7:4: error: constructor 'None' of type 'Option' is not exported by module 'option.mlite' (add 'export None;' or 'export Option;' there)`. At the end of a module, every ADT in its registry — including imported ones — had unexported constructors made private to *that* module's scope, so the defining module's code (and a diamond importer) lost them. Reproduces in plain minilang (`o.ml` → `r.ml` → main).
- Workaround (before the fix): re-export every imported type from every intermediate module.
- Proposed fix: only privatize ADTs defined by the module itself (`origin_module` unset or equal to the module); imported ADTs pass through with the visibility their defining module gave them.
- Target: fixed-in-Step-11 (`Workbench::import_module_file`); tests `import_nested_adt_*` in `tests/run_tests.sh`

### FL-14: A top-level value definition could not call an imported function
- Milestone: M3            Tag: organic
- Symptom: `import "std/result.mlite"; val v = Ok(1) |> map_ok(fn (x) => x + 1); ...` failed with `error: 'map_ok' has no type` / `error: 'map_ok' is used before assignment on every path`, while the same call in a `let` or the final expression worked. Same in minilang (`import "inc.ml"; def v = inc(1); v`). Top-level `Seq` lowering initializes all non-lambda `def`s (Pass 2a) before lambda bodies and the importer's `import_alias` bindings (Pass 2b), so the alias local was still unassigned when `v` read it.
- Workaround (before the fix): only call imported functions from inside functions that run after the top-level definitions, or from the final expression.
- Proposed fix: lower `import_alias` bindings in Pass 2a in statement order (an imported module's statements precede its importer's); an alias of a capturing lambda lowers that lambda there too once everything it captures is initialized, otherwise it stays in Pass 2b. The alias carries the target's arity for call checks.
- Target: fixed-in-Step-11 (`CoreFormKind::Seq` lowering in `src/ifa_bridge/bridge.cc`); tests `import_alias_*` in `tests/run_tests.sh`

### FL-15: ADT constructors are visible transitively; functions are not
- Milestone: M3            Tag: organic
- Symptom: `import "std/result.mlite"; Some(3)` works although `result` does not export (or re-export) `option`'s `Option`, while `unwrap_or(None, 0)` in the same program is `unbound identifier 'unwrap_or'`. Constructor visibility is one `private_scope` per variant, set by the defining module; once exported, a constructor reaches every module that (transitively) imports its type's module, whereas `def`s are re-bound per import edge by `import_alias`.
- Workaround: none needed for MLite's programs (they import `option` explicitly); pinned by `mlite_m3_ctor_visible_transitively_fl15`.
- Proposed fix: make constructor visibility follow import edges like `def`s (bind constructors per importer scope, or record the set of scopes that imported the type), and let a module re-export an imported type explicitly.
- Target: Step 12

### FL-16: Intermittent `GC Warning: Finalization cycle involving 0x...` on larger programs
- Milestone: M3            Tag: organic
- Symptom: About 1 run in 40 of `expr_eval.mlite` / `wordcount.mlite` (and occasionally smaller MLite programs with Phase-1 helpers) prints a burst of `GC Warning: Finalization cycle involving 0x...` lines to stderr before the program's output (also with the M2 binary, so it predates M3). `CstNode` and `SyntaxObj` derive from `gc_cleanup`; Boehm reports finalizable objects that reach each other (probably through conservatively scanned words, since `children` has no cycles) and simply leaves them unfinalized. The output is otherwise correct, but golden-output tests flaked.
- Workaround: `expect_eval` / `expect_file` in `tests/run_tests.sh` drop exactly these warning lines (`drop_gc_cycle_warnings`).
- Proposed fix: stop deriving syntax / CST nodes from `gc_cleanup` (their `std::string` / `std::vector` / `unordered_map` members are the only reason for finalizers: allocate those with GC allocators, or arena-free them per evaluation), or register finalizers with `GC_register_finalizer_no_order` semantics.
- Target: Deferred

### M3 notes on hypotheses
- **H3 (`Result` plumbing):** with `std/result.mlite`'s `and_then` / `map_ok` and `|>`, an arm that needed 9 lines of nested `case` (`Err(m) => Err(m)` twice) is two lines: `eval_expr(l) |> and_then(fn (lv) => eval_expr(r) |> map_ok(fn (rv) => lv * rv))`. `expr_eval.mlite` keeps `Add` in the nested-`case` style for comparison. Neither a `try` macro nor nested patterns is needed for this program; `case` with flat patterns plus combinators is enough. The remaining cost is one closure per bound intermediate.
- **H5 (import paths):** under `langwb <file>`, imports resolve relative to the importing file (`import "std/result.mlite";` from `examples/mlite/`, `import "option.mlite";` inside `std/`), then the working directory, then the `#lang` search path. Import specs need the `.mlite` extension (the resolver only appends `.ml` / `.lang`). Under `-e`, specs are relative to the working directory. `langwb build` and `--lsp` remain to be checked in M5.
- **Exports:** `export` of `fun` / `val` (`core(define)`), types (all constructors), and `syntax_rule` macros all work unchanged under the replaced grammar, because `import` / `export` are workbench declarations parsed before the guest grammar. Exported macros' private helpers (`pick_ok`) stay hygienic and hidden. Prims (`int_to_str`) are not first-class values, so `show_result(r, int_to_str)` must be written `show_result(r, fn (v) => int_to_str(v))`.

### FL-17: A `#lang`'s `check call` on a library function warned in every program that did not call it
- Milestone: M4            Tag: organic
- Symptom: with `check call safe_div` and `check call assert` in `mlite.lang` (the functions live in `std/result.mlite` / `std/assert.mlite`), every MLite program that never called them printed `langwb: warning: check target 'safe_div' did not match any core form, primitive, or function` (once per check). The unused-target warning exists to catch typos in a program's own `check`s, but a language's checks legitimately target library functions that most programs do not use.
- Workaround (before the fix): none (declaring the functions in the `.lang` is impossible, F8).
- Proposed fix: mark checks registered while `load_lang` applies a `.lang` file's declarations (`StaticCheckEntry::from_lang`) and skip the warning for targets whose checks all come from a language.
- Target: fixed-in-Step-11 (`Workbench::load_lang` / `register_static_check_stx`, `prepare_staged_syntax`); tests `lang_check_*` in `tests/run_tests.sh`

### FL-19: A constant-folding `prim` transfer function aborted when its argument widened
- Milestone: M4            Tag: organic
- Symptom: `fun f(x) = abs_i(x); f(0 - 9) + f(4)` (and minilang `#lang mydsl` with `my_abs`) aborted in DEBUG builds with `langwb: non-monotonic transfer function for primitive 'abs_i'` / `Assertion '!"non-monotonic primitive transfer function"' failed`. The transfer function folds a constant argument (`fa_emit_const_i64(9)`) and, once IFA widens the argument, emits `fa_emit_i64()`; the result goes from `{9}` to `{int64}`, which IFA's lattice treats as a widening (the abstract `int64` subsumes its constants), but the debug check required every earlier `CreationSet` to stay literally present.
- Workaround (before the fix): never fold constants in a transfer function that can see more than one argument value.
- Proposed fix: in `is_monotone_extension`, accept a constant `CreationSet` that is subsumed by its base type's `CreationSet` in the new result.
- Target: fixed-in-Step-11 (`src/ifa_bridge/prim_registry.cc`); test `prim_transfer_const_then_widened_is_monotone` in `tests/run_tests.sh`

### FL-18: `hook module_begin` also reaches imported modules
- Milestone: M4            Tag: coverage
- Symptom: §4.3 expected `module_begin` to run only for the entry module. It does run once, but on the entry module's top-level `Seq` *after* imported modules' statements have been spliced into it, so a binding it introduces with `stx_unhygienic_id` (MLite's `mlite_version`) is visible inside imported modules: `std`-style module `fun mv() = mlite_version; export mv;` imported into a `#lang mlite` program returns `4`, but evaluated on its own it would not exist. A module of a different `#lang` imported into an MLite program would see MLite's prelude too. (Verified the other §4.3 point: a single-expression entry file and a multi-form one both work with the same `let val __id__ = ... in __body__ end` template, since `core(let)` accepts a `Seq` body.)
- Workaround: none needed for MLite (all modules are MLite); pinned by `mlite_m4_module_begin_reaches_imports_fl18`.
- Proposed fix: pass `module_begin` only the entry module's own forms (with imported statements and import aliases outside the wrapper), or give the hook's injected identifiers the entry module's scope rather than none.
- Target: Step 12

### M4 notes on hypotheses and §5.1
- **`fast_pow` (§4.4):** constant exponents 0..8 unroll to `let val t = b in t * t * ... end` (verified with `--dump-expanded`: no `while` left), others become an inline `while` loop (no runtime helper needed, so F8 does not bite here). Stage-1 is contour-aware: `fun p(b, e) = fast_pow(b, e); p(2, 3) + p(3, 3)` unrolls inside `p`, while `p(2, 3) + p(3, 10)` loops. The N-ary product is a recursive `for_syntax` helper (F5); template-introduced `t`, `b`, `e`, `r`, `i` are hygienic against user names.
- **`check call safe_div` (§4.4):** catches a zero divisor written directly, through a `val`, through an `if` that folds, through `|>` (checks see expanded code, F4), through a helper called only with `0` (`h(0)`), and `x - x`. It **misses** `h(0) + h(5)`: the calls share a contour and the divisor merges to non-constant in every contour, so `attr_contour_is_const_i64` does not help (recorded by `mlite_m4_safe_div_shared_contour_missed`). Diagnostics point at the divisor, with `(expanded from 'pipe_e' at L:C)` when the call came from `|>`.
- **`check call assert`:** a purely syntactic check (`stx_is_ident` + `stx_text_eq(c, "false")`); `assert(2 < 1, ...)` passes the check and fails at run time instead: the Meta-ABI's constant queries (`attr_is_const_i64` / `attr_const_i64`) are int64-only, so a check cannot ask whether a `bool` is a known constant (F6). The run-time failure prints to stdout (there is no stderr print primitive) and calls C `exit(1)` through `prim exit_with c_call "exit"`, a `void` function declared with an `int64` transfer (F6); the JIT child exits and `langwb` then reports `evaluation failed` with status 1, like a non-exhaustive `case`.
- **F8 confirmed:** `safe_div` and `assert` are runtime `fun`s, so they live in `std/result.mlite` and `std/assert.mlite` rather than in `mlite.lang`; the language's checks target them by name (which required FL-17).
- **Prims (§4.5):** `abs_i` (C `llabs`) folds constants in its transfer function (which exposed FL-19 as soon as it saw two argument values); `clock_ticks` (C `clock`, 0 args) works as an impure `fa_emit_i64()` prim.
- **`syntax_ext power`:** `**` is right-associative, binds tighter than `*` and unary `-` (`- 2 ** 2` is `-4`, `2 ** - 1` is `2 ** (-1)`), is a syntax error outside `using syntax power in { ... }`, and lowers to `fast_pow` (so constant exponents unroll). `using syntax` needed only the grammar rule from §4.1.1.

### FL-20: `#lang` lookup depended on the working directory
- Milestone: M5            Tag: organic (H5)
- Symptom: `cd /tmp && langwb /path/to/langwb/examples/mlite/aot_cli.mlite` (and `langwb build ...`) failed with `langwb: cannot find #lang module 'mlite'`: the `#lang` search path was `.`, `langs`, `examples/langs`, all relative to the current directory, so MLite programs only ran from the repository root. Imports were already file-relative (M3), so the two lookups disagreed; an editor running `langwb --lsp` from another directory would have hit the same error on every document.
- Workaround (before the fix): run everything from the repository root.
- Proposed fix: search the source file's directory (and its `langs/`) first, the importing module's directory for imported modules, then the old working-directory entries, `LANGWB_LANG_PATH` (`:`-separated), and `langs/` / `examples/langs/` next to the `langwb` executable.
- Target: fixed-in-Step-11 (`default_lang_search_paths` / `with_source_dir` in `src/workbench/workbench.cc`); tests `lang_lookup_*` in `tests/run_tests.sh`

### FL-21: LSP hover returned nodes from imported modules and over-wide macro nodes
- Milestone: M5            Tag: organic (H6)
- Symptom: in a `#lang mlite` document importing `std/list.mlite`, hovering `scale` showed `xs :` and hovering a `case` arm variable showed `__wb_is_variant(__m__, "MNil", 0) : bool`. Imported modules' statements are spliced into the analyzed tree, and the position lookup matched any node on the same line/column without checking `SyntaxObj::file`. With that fixed, hovering an arm variable (`side` in `Square(side) => side * side`) still showed the arm's variant test: the `case` macro gives that template node the pattern's location (FL-7), its span is measured by its (template) text, and the lookup returned the first match in tree order.
- Workaround: none.
- Proposed fix: never match nodes with `file` set (still searching their children, which can hold spliced document nodes), and pick the narrowest analyzed node covering the cursor.
- Target: fixed-in-Step-11 (`find_node_at` / `find_annotated_at` in `src/workbench/lsp.cc`); tests `lsp_hover_*` in `tests/run_tests.sh`

### FL-22: `using syntax` bodies had block-relative source positions
- Milestone: M5            Tag: organic (H6)
- Symptom: everything inside `using syntax power in { ... }` was located as if the body started at `1:0`: a type error in the minilang `using syntax e in { zz "a" }` reported `<macro:zz_e@1:0>`, and LSP hover / definition inside the block found nothing (hover fell back to the enclosing expression). The `using_syntax` handler parses the raw block text on its own with the extension's grammar and never moved the resulting nodes.
- Workaround: none.
- Proposed fix: pass the raw `{ ... }` block node to the handler and offset the parsed body to the block's position (after `{` and leading whitespace, across lines), inheriting the block's file for imported modules.
- Target: fixed-in-Step-11 (`UsingSyntaxFn` in `src/macro/expander.h`, `Workbench::init_using_syntax_hook`); tests `using_syntax_body_error_position`, `lsp_using_syntax_body_hover_and_definition` in `tests/run_tests.sh`
