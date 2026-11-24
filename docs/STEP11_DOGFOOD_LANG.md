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
`examples/langs/mlite.lang`** (zero C++ changes), with a small multi-file
standard library under `examples/mlite/std/` and the target programs under
`examples/mlite/`.

---

## 2. Grammar Strategy: **Replace**, Not Extend

This is the decision everything else depends on.

**Facts about the workbench today:**

- `#lang` modules always load on top of the default minilang grammar
  (`load_lang` runs inside a `Workbench` initialised by `init_default_minilang`);
  there is no way for a `#lang` to start from the empty `--bare` grammar the way
  `prelude.lang` does. Replacement must use `grammar <nt> := ...`.
- Declaration bodies (`macro`, `form_macro`, `stage1_macro`, `check`, `hook`,
  `prim ... transfer`, `for_syntax`) and unquoted `syntax_rule` templates are
  parsed eagerly with the **active guest grammar at the point of declaration**
  (`Workbench::parse_and_apply_decl_prefix`).

**Options:**

| | Extend (`expr += ...`) | Replace (`expr := ...`) |
|---|---|---|
| User programs | Can also write `fn(x) =>`, `{ def ... }`, `match e { }`, `unless`, `aif` — MLite is a superset of minilang, not a separate language. | Only MLite syntax is accepted. |
| Phase-1 code in `mlite.lang` | Written in minilang. | Declarations placed after the `:=` are written in **MLite** — the language author dogfoods their own language for macros too. |
| Diagnostics | Leak minilang constructs. | Expose exactly where lowering leaks (Step 12 input). |
| Effort | Low. | Higher; surfaces friction items F1–F3 (§5) on day one. |

**Decision: Replace.** `mlite.lang` is laid out as:

1. A short preamble of declarations that must be written in minilang
   (anything needed before the MLite grammar exists, e.g. `for_syntax` helpers
   that the grammar's own macros use). Keep this as small as possible and log
   every item that *had* to go here.
2. `grammar { expr := ...; ... }` — the MLite grammar. Internal helper
   non-terminals that the workbench's built-in lowering depends on (e.g.
   `match_expr`, `list_lit`) stay defined but are reachable only through MLite
   constructs.
3. All remaining macros, checks, hooks, and prims, written in MLite.

If Replace turns out to be blocked (not merely painful), fall back to Extend,
and record the blocker as the top friction-log entry.

---

## 3. Target Programs (Written Before `mlite.lang`)

These are written first, in MLite as we want it to look, and drive the feature
set. Each has a golden expected stdout checked by the test suite.

| Program | What it forces | Expected-output style |
|---|---|---|
| `expr_eval.mlite` | ADTs, nested `case`, `Result`/`Option` plumbing, recursion, `\|>`, multi-file imports, Stage-1 `fast_pow` on constant exponents. | Prints each evaluated expression and its `Ok`/`Err` result. |
| `wordcount.mlite` | Strings, lists, loops, `var`/`:=`, records, `and`/`or`, stdlib `list` folds. | Prints word → count pairs for an embedded text. |
| `graph_cycle.mlite` | Mutable ADT/record fields, closures capturing `var`s (auto-boxing), cycle printing (`<cycle>`), heterogeneous split-clone unions (`Cons(1, Nil)` vs `Cons("a", Nil)`). | Prints a cyclic graph and a closure counter's history. |
| `aot_cli.mlite` | Standalone AOT build (`langwb build`), `prim`/`c_call` (e.g. `clock_ms`). | Deterministic portion printed; timing value only checked for type. |

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
| O | Value & function bindings | `val x = e;` / `fun f(x, y) = e;` | **Cannot be a `syntax_rule`**: templates are validated only against `expr`, but `def` is a `stmt`; and `core(define, ...)` takes an already-built lambda as `rhs`, with no core form that assembles `name(params) = body` into one. Candidate routes: a `stmt` form macro returning `stx_parse("stmt", ...)`, or a grammar rule whose `rhs` child is a lambda-shaped non-terminal. See F2. |
| O | Local `let ... in ... end` | `let val x = e1 in e2 end` | Multi-binding `let` desugared to nested core `let`s. |
| O | Explicit blocks `do ... end` | `do s1; s2; e end` | Lowers to `core(seq)`; frees `{ ... }` for record literals only. |
| O | `case ... of ... end` | `case e of None => 0 \| Some(v) => v end` | **`match` is not a core form**: it is a C++ form macro bound to minilang's `match e { P => b, ... }` syntax (`workbench.cc` `match_expr`), and match-specific diagnostics key on `origin_macro == "match_expr"` (`bridge.cc`). Plan: keep `match_expr` defined, rewrite `case` into it with a variadic `syntax_rule` (`$arms <sep> ...`) mapping `\|`-separated arms to `,`-separated arms. Fallback: emit `__wb_is_variant` / `__wb_variant_field` directly (leaks internal names, loses match diagnostics). See F3. |
| O | Pipeline `\|>` | `x \|> f`, `x \|> f(y, z)` | Lowest-precedence `$left` infix; form macro inspecting the RHS (`ident` vs `app`) to produce `f(x)` / `f(x, y, z)`. |
| O | `and` / `or` / `not` | `a and b`, `not a` | Precedence below comparisons; hygienic desugar to `if` (lazy RHS). Unary `not`/`-` tests DParser unary/binary disambiguation (H1). |
| O | Counted loop | `for i = lo to hi do body end` | Hygienic desugar to `let` + `while` + `Assign`; `hi` evaluated once. |
| O | Mutation | `var x = 0; x := x + 1; r.f := 2` | `core(assign)`, `core(set_field)` + `field_lhs`, auto-boxing for captured `var`s. |

### 4.2 Algebraic Data Types, Records, & Printing (O)

- `type Option = None | Some(val);`, `type Result = Ok(val) | Err(msg);`,
  recursive user ADTs. `type` is a workbench-level declaration and should work
  unchanged under the replaced grammar — verify.
- Exercises per-variant `Type_RECORD` classes, SSU narrowing (`Ok(int64)` vs
  `Err(string)`), nullary constructor equality, `_wb_print_<id>` cycle-safe
  printing, and `WbSymbol::copy` on IFA-split record classes.

### 4.3 Hygienic Macros, `#lang` Hooks, & Scoped Syntax

- **(O) Stage-0 `syntax_rule` macros:** `and`, `or`, `for`, `|>`, `unless`.
  Verifies set-of-scopes hygiene, single evaluation, and transitive private
  helper macros across modules.
- **(C) `#lang` hook — pick exactly one: `hook module_begin`** that injects
  MLite's prelude bindings (e.g. `unwrap_ok_or` if it is not imported).
  `hook app` is rejected: it would also fire on calls produced by macro
  expansion (including MLite's own desugarings), making it a poor first test.
- **(C) Scoped syntax extension — pick exactly one: `syntax_ext power`** adding
  right-associative `**` exponentiation, activated via
  `using syntax power in { ... }` and lowering to `fast_pow`. (`|x|` absolute
  value is rejected: it collides with `|` case arms; interesting for GLR
  ambiguity, but too costly for a first dogfood.) Exercises `SyntaxExtDelta`
  replay and lexical non-leakage.

### 4.4 IFA-Driven Stage-1 Macros & Static Checks

- **(O) `stage1_macro fast_pow(base, exp)`** — a zero-C++ `.lang` port of the
  built-in C++ `pow_opt` (`workbench.cc`). Uses `attr_is_const_i64(exp)` /
  `attr_const_i64(exp)` to unroll constant exponents `0..8` into
  `let t = base in t * t * ...` (single evaluation of `base`), else falls back
  to a runtime loop. Building an N-ary product requires a recursive
  `for_syntax` helper over `stx_parse` / `stx_splice` (see F5). Called through
  wrappers (`fun cube(x) = fast_pow(x, 3)`) to exercise contour-aware Stage-1
  cloning.
- **(C) `check call safe_div`** — rejects a divisor with
  `attr_is_const_i64(rhs) && attr_const_i64(rhs) == 0`.
  Scope of the claim: direct constants, constants through `val` bindings and
  `if` branches that fold. Zero propagating *through a helper call* is caught
  only if IFA gives the helper a separate contour for that call; when `h(0)`
  and `h(5)` share a contour the constant merges and the check misses it unless
  it uses `attr_contour_is_const_i64` / `attr_contour_const_i64`. The test
  suite records the actual behavior for both cases rather than asserting it.
- **`assert` is a function, not a macro.** `check` runs on the Stage-0
  *expanded* tree, so a `check call assert` would never see a `syntax_rule`
  `assert`. MLite defines `fun assert(cond, msg) = ...` and (C) a
  `check call assert` that rejects a condition IFA folds to `0`.

### 4.5 Foreign Function Interface (C)

- `prim` + `c_call` for `llabs` (pure; transfer function constant-folds) and a
  clock primitive (impure; transfer emits `int64`).
- **Known limit:** transfer functions can only emit `int64`
  (`fa_emit_i64` / `fa_emit_const_i64`), so string- or record-returning C
  functions are out of scope. Log any program that wanted one.

### 4.6 Multi-File Standard Library (O)

```
examples/langs/mlite.lang        # #lang mlite: grammar (replaced), macros, checks, prims, hook
examples/mlite/
├── std/
│   ├── option.mlite             # type Option, map_opt, unwrap_or, is_some
│   ├── result.mlite             # type Result, map_ok, and_then, unwrap_ok_or (imports option)
│   └── list.mlite               # type MList, map_l, filter_l, fold_l, range_l, len_l
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
  private helpers, and whether `fun`-produced `def`s are exportable.

---

## 5. Friction Hypotheses

### 5.1 Already known from reading the workbench (verify, then log)

- **F1 — Declaration ordering under a replaced grammar.** Phase-1 code after
  `expr := ...` must be MLite; anything needed to *build* the grammar must
  precede it in minilang.
- **F2 — No statement-level `syntax_rule`.** Templates validate only as `expr`,
  and `core(define)` needs a pre-built lambda, so `fun`/`val` need a form
  macro or grammar trick.
- **F3 — `match` is a C++ macro on minilang syntax, not a core form.** Custom
  pattern syntax must either target `match_expr` or the internal
  `__wb_is_variant` helpers.
- **F4 — Checks see expanded code.** Macros cannot be targets of `check call`.
- **F5 — No Meta-ABI for building variable-length syntax.** N arms, N-ary
  products, etc. require recursive string-splice helpers.
- **F6 — Transfer functions are `int64`-only.**
- **F7 — `#lang` cannot start from a bare grammar.**

### 5.2 Hypotheses to test

1. **H1 Unary operators (`-x`, `not x`)** vs binary `a - b` under DParser
   precedence.
2. **H2 Error messages in MLite's terms:** how much minilang / IF1 lowering
   leaks for a bad `case` arm, arity error, or type error (Step 12 input).
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
| **M0** | Replaced grammar skeleton; `val`/`fun`/`let`/`if`/calls/arithmetic; decide F1/F2 routes. | `fun fact(n) = ...; fact(10)` runs under JIT. |
| **M1** | `type` + `case` (F3 route chosen). | `expr_eval.mlite` minus `fast_pow` and imports. |
| **M2** | `var`/`:=`/`for`/`do`/`\|>`/`and`/`or`/`not`, records. | `wordcount.mlite`, `graph_cycle.mlite`. |
| **M3** | Multi-file stdlib, exports, private helpers. | `expr_eval.mlite` with imports; negative unexported-helper test. |
| **M4** | `fast_pow`, `safe_div`, `assert`, `prim`s, hook, `syntax_ext`. | All target programs; negative static-check tests. |
| **M5** | AOT + LSP. | `aot_cli.mlite` standalone binary; LSP diagnostics/hover/definition test. |

### 6.1 Timebox & Cut List

Timebox Step 11 so that it produces a friction log rather than an ever-growing
language. If time runs short, cut in this order (coverage first, never cut the
log): `syntax_ext power` → `hook module_begin` → `check call assert` →
clock `prim` → `for` loop. M0–M3 plus `fast_pow` and `safe_div` is the minimum
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

*(Entries are appended here during M0–M5.)*
