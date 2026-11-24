# Step 11 Proposal — Dogfood Language: **MLite**

> **Goal (from [ROADMAP.md](ROADMAP.md)):** Implement one non-trivial language
> with **zero C++** under `examples/langs/`, ship a real program & test suite in
> `tests/run_tests.sh`, and record every workaround and missing capability so
> that subsequent roadmap work (Steps 12–13 and beyond) is driven by real
> language-authoring friction rather than guesswork.

---

## 1. Language Choice: Why **MLite** (over a Small Scheme or Toy DSL)?

Three candidate archetypes were considered for Step 11:

| Candidate | Strengths | Weaknesses as a `langwb` stress test |
|---|---|---|
| **Small Scheme (`mscheme`)** | Great test of hygienic `syntax-rules` macros, closures, tail/mutual recursion, and list processing. | Uniform S-expression grammar barely exercises DParser's GLR/precedence/associativity engine, `field_lhs` extension, or surface-syntax disambiguation. |
| **Narrow Domain DSL** (e.g. SQL or regex) | Exercises custom grammar and Stage-1 specialization. | Too small to stress multi-file `import`/`export`, ADTs, general-purpose control flow, closures, and AOT standalone binaries. |
| **MLite (Small ML-family language)** | Rich, non-trivial surface syntax *plus* ADTs, pattern matching, pipelines, imperative escape hatches, hygienic macros, Stage-1 specialization, C FFI, and a multi-file stdlib. | Slightly larger grammar than Scheme, which is *the point*: it tests whether a real surface language can be written cleanly in `.lang` alone. |

**MLite** is a compact, expression-oriented ML-family language (inspired by
OCaml / Standard ML / Elixir pipelines) implemented **entirely in
`examples/langs/mlite.lang`** (zero C++ changes), accompanied by a small
multi-file standard library under `examples/mlite/std/` and real showcase
programs under `examples/mlite/`.

---

## 2. Feature Matrix & Workbench Justifications

Every feature in MLite is included to exercise a specific `langwb` mechanism and
stress-test a realistic language-authoring pattern.

### 2.1 Surface Syntax & Custom Grammar (`grammar`, `$left`/`$right`, `core(...)`)

| MLite Construct | Syntax | Justification / `langwb` Mechanism Exercised |
|---|---|---|
| Value & function bindings | `val x = e;` / `fun f(x, y) = e;` | Replaces `minilang`'s `def f = fn(...) => ...` ceremony with idiomatic ML declarations; exercises `grammar` rule addition and `syntax_rule form` lowering to `CoreFormKind::Define` + `Lambda`. |
| Local `let ... in ... end` | `let val x = e1 in e2 end` | Exercises custom delimited non-terminals and multi-binding `let` desugaring (nested `let`s via hygienic `syntax_rule`). |
| Explicit blocks `do ... end` | `do s1; s2; e end` | Separates statement sequences (`do ... end`) from record literals (`{ a: 1, b: 2 }`), eliminating the `{` ambiguity in `minilang` and testing `CoreFormKind::Seq` via custom grammar. |
| ML `case ... of ... end` | `case e of None => 0 \| Some(v) => v end` | Tests custom pattern-matching syntax with `\|`-separated arms lowering to `match` / `__wb_is_variant` / `__wb_variant_field`, exercising IFA SSU narrowing and dead-arm elision through a custom front end. |
| Pipeline operator `\|>` | `x \|> f` and `x \|> f(y, z)` | Exercises custom infix precedence (`$left 0`), form-macro AST inspection (rewriting `x \|> f(y)` → `f(x, y)` and `x \|> f` → `f(x)`), and hygiene across staged calls. |
| Boolean short-circuit `and` / `or` & `not` | `a and b`, `a or b`, `not a` | Exercises operator precedence layered below comparisons (`==`, `<`, …) and hygienic Stage-0 desugaring into `if ... then ... else ...` so the RHS is lazily evaluated. |
| Counted loop `for ... to ... do ... end` | `for i = lo to hi do body end` | Exercises hygienic loop desugaring into `let` + `while` + mutation without capturing user variables (`hi` evaluated once, loop counter scoped hygienically). |
| Imperative mutation (`:=`) | `var x = 0; x := x + 1; r.f := 2` | Distinguishes binding/equality (`=`) from mutation (`:=`), exercising `CoreFormKind::Assign`, `CoreFormKind::SetField` + `field_lhs`, and auto-boxing when a `var` is captured by a closure. |

### 2.2 Algebraic Data Types, Records, & Printing

- **MLite `type` declarations:** `type Option = None | Some(val);`,
  `type Result = Ok(val) | Err(msg);`, and recursive user ADTs
  (`type Expr = Lit(n) | Neg(e) | Add(l, r) | Div(l, r);`).
- **Justification:** Exercises IFA's per-variant `Type_RECORD` classes,
  branch-sensitive SSU narrowing (`Ok(int64)` vs `Err(string)` in one `Result`),
  nullary constructor equality (`x == None`), per-type cycle-safe value printing
  (`_wb_print_<id>`), and `WbSymbol::copy` on IFA-split record classes.
- **Concrete stress test:** A recursive `Expr` evaluator returning `Result` and
  a mutable linked structure / graph demonstrating cycle printing (`<cycle>`)
  and heterogeneous split-clone unions (`Cons(1, Nil)` vs `Cons("a", Nil)`).

### 2.3 Hygienic Macros, `#lang` Hooks, & Scoped Syntax Extensions (`syntax_ext`)

- **Hygienic Stage-0 macros (`syntax_rule`):**
  - Control/ergonomic macros: `and`, `or`, `for`, `|>`, `unless`, `assert(cond, msg)`.
  - Verifies Flatt's set-of-scopes hygiene (`decl_scope_`), single-evaluation
    bindings (no double-evaluating `lo`/`hi` or `x` in `|>`), and transitive
    private helper macros across modules.
- **`#lang` interposition hook (`hook`):**
  - `hook datum` or `hook app` in `mlite.lang` — e.g. a lightweight `hook app`
    or `hook module_begin` that wraps top-level programs or instruments a
    designated form (such as `trace_calls` or checked arithmetic), proving that
    Racket-style `#lang` interposition works cleanly in a full language.
- **Lexically scoped syntax extension (`syntax_ext` + `using syntax`):**
  - Ship a scoped extension (e.g. `syntax_ext vec_ops` or `syntax_ext infix_math`
    adding `**` exponentiation and `|x|` absolute value, or a small
    `syntax_ext query` DSL) activated only inside `using syntax ... in { ... }`.
  - **Justification:** Exercises `SyntaxExtDelta` replay, lexical scoping of
    grammar rules (no leakage outside the region), and GLR ambiguity resolution
    (Tiers 1–3) inside a real guest program.

### 2.4 IFA-Driven Stage-1 Macros & Static Checks (`stage1_macro`, `check`)

This is `langwb`'s unique differentiator over traditional macro systems (Racket,
Rust `macro_rules!`): macros and static checks can inspect **IFA's inferred
types and constants**.

- **Stage-1 specialization (`stage1_macro`):**
  - `fast_pow(base, exp)`: uses `stx_is_const(exp)` and `stx_const_i64(exp)` to
    unroll small constant exponents (`0..8`) into straight-line multiplications
    at compile time, falling back to a runtime loop when `exp` is dynamic.
  - Because `fast_pow` can be called through wrapper functions (`fun cube(x) = fast_pow(x, 3)`),
    this directly exercises **contour-aware Stage-1 cloning** (IFA's demand-driven
    function splitting).
- **Static safety checks (`check call`):**
  - `check call safe_div`: rejects provable division by zero at compile time
    when `stx_is_const(rhs) && stx_const_i64(rhs) == 0`, even when the zero
    propagates through variables, `if` branches, or helper calls.
  - `check call assert`: warns or rejects statically-known `false` assertions
    when IFA folds the condition to `0`.
  - **Justification:** Demonstrates how a `.lang` author can encode domain-specific
    static analyses in ~10 lines of guest code with zero C++.

### 2.5 Foreign Function Interface (`prim` / `c_call`)

- Register a small set of native C primitives in `mlite.lang` via `prim` +
  `c_call` (e.g., `c_abs` / `llabs` from libc, or `c_putchar` / `c_clock_ms`),
  with custom IFA transfer functions so IFA knows their return types (and
  constant-folds pure ones when inputs are known constants).
- **Justification:** Proves that a `.lang` language can bind host C libraries
  and link cleanly in both LLJIT (`langwb`) and standalone AOT binaries
  (`langwb build`).

### 2.6 Multi-File Standard Library (`import` / `export`)

Structure MLite as a real multi-file ecosystem rather than a single monolithic
file:

```
examples/langs/mlite.lang        # Core #lang mlite grammar, macros, Stage-1 checks, prims
examples/mlite/
├── std/
│   ├── option.mlite             # type Option, map_opt, unwrap_or, is_some (explicit export)
│   ├── result.mlite             # type Result, map_ok, and_then (explicit export)
│   └── list.mlite               # type MList, map_l, filter_l, fold_l, range_l, len_l
├── expr_eval.mlite              # Recursive Expr AST interpreter with Result + Option env
├── pipeline_demo.mlite          # Multi-file stdlib + |> pipelines + Stage-1 fast_pow + safe_div
├── graph_cycle.mlite            # Mutable ADT nodes, auto-boxed closure counters, cycle printing
└── aot_cli.mlite                # Standalone AOT-compiled program (langwb build)
```

- **Justification:** Exercises relative imports from importing file directories,
  diamond imports (`expr_eval.mlite` importing both `std/option.mlite` and
  `std/result.mlite`), private helper hiding (both private `def`s and private
  ADT constructors), exported macros with transitive private helper macros, and
  LSP hover/diagnostics/go-to-definition across multi-file `#lang mlite` projects.

---

## 3. Sample MLite Program

To make the target concrete, here is what `examples/mlite/expr_eval.mlite`
should look like:

```ml
#lang mlite
import "std/option.mlite";
import "std/result.mlite";

type Expr =
    Lit(n)
  | Add(lhs, rhs)
  | Mul(lhs, rhs)
  | Div(lhs, rhs)
  | Pow(base, exp);

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
  | Pow(b, k)    => ...
  end;

fun main_result() =
  let val prog = Add(Mul(Lit(3), Lit(10)), Div(Lit(24), Lit(2))) in
    prog |> eval_expr |> unwrap_ok_or(0)
  end;

main_result()
```

---

## 4. What Step 11 Is Expected to Surface (Friction Hypotheses)

A key deliverable of Step 11 is an honest friction log. Before writing a single
line of `mlite.lang`, here are the specific pain points we expect to test and
document:

1. **Unary operators (`-x`, `not x`):** `minilang`'s base grammar has no unary
   minus; adding unary `-` in `.lang` will test DParser precedence/disambiguation
   between binary `a - b` and unary `-x`.
2. **Error messages in MLite's own terms:** When a user writes a bad `case` arm
   or type error in MLite, how much of the underlying `minilang` / IF1 lowering
   leaks through? (Directly informs **Step 12**.)
3. **Nested patterns & `Result` ergonomics:** Writing nested `case` expressions
   for `Ok`/`Err` will show whether single-level constructor patterns are
   ergonomic enough or whether a `try`/`?` macro (or nested patterns) is needed.
4. **Multi-binding `let` & top-level `fun` recursion:** Testing whether
   `fun f(x) = ...; fun g(x) = ...;` (mutually recursive top-level functions)
   desugars cleanly through `CoreFormKind::Define` inside `eval_impl`'s
   top-level `Seq`.
5. **Stdlib import paths:** Checking whether `import "std/option.mlite"` works
   consistently from both `langwb examples/mlite/expr_eval.mlite`,
   `langwb build`, and `langwb --lsp`.
6. **Syntax-error recovery & LSP hover on macro-expanded MLite forms:** Checking
   how `textDocument/hover` and `textDocument/definition` behave on `fun`,
   `val`, `case`, and `|>` after Stage-0 expansion.

---

## 5. Acceptance Criteria for Step 11

1. **`examples/langs/mlite.lang` implemented with zero C++ changes** (any C++
   fixes needed must be genuine general-purpose workbench bugs, not
   MLite-specific hooks).
2. **Multi-file stdlib + 4 showcase programs** under `examples/mlite/` covering
   custom grammar, ADTs, `case`, `|>`, `for`, `var`/`:=`, `syntax_ext`,
   `stage1_macro` (`fast_pow`), `check call` (`safe_div`), `c_call`, and
   `import`/`export`.
3. **End-to-end test suite in `tests/run_tests.sh`** verifying MLite under:
   - JIT (`langwb <file>`)
   - AOT (`langwb build <file> -o ...`)
   - LSP (`langwb --lsp` diagnostics + hover + definition on an MLite file)
   - Negative tests (static `safe_div` by zero, unexported stdlib private helper,
     bad `case` arity).
4. **Friction log appended** (either in `docs/STEP11_DOGFOOD_LANG.md` or
   `docs/ROADMAP.md`) capturing every workaround and missing capability to feed
   directly into Steps 12–13.
