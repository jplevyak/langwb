# `langwb` Roadmap

This document tracks what `langwb` has completed and the next milestones toward a full zero-C++ programming language workbench built on DParser (scannerless GLR) and IFA (whole-program iterative flow analysis + LLVM ORCv2 LLJIT).

See [SKETCH.md](SKETCH.md) for architectural design decisions and [reviews/2026-11-21-review.md](reviews/2026-11-21-review.md) for the technical review that shaped Steps 1–5.

---

## Completed Milestones

### Foundation (Phases 0–4)
- **Phase 0:** In-memory DParser (`mkdparse_from_string` → binary tables) → CST → IF1 → IFA → LLVM LLJIT vertical slice.
- **Phase 1:** `PrimRegistry` with persistent C++23 transfer functions, debug monotonicity checking, and inline LLVM IR emission (`llvm_emit_get_var` / `llvm_emit_put_result`).
- **Phase 2:** Composable `GrammarDef` IR, content-addressed `GrammarCache`, and Racket-style sets-of-scopes hygiene (`SyntaxObj`, `ScopeSet`, `BindingEnv`, form & call macros, explicit `unhygienic_ident` opt-out).
- **Phase 3:** `AnalysisAttrs` snapshot surviving `ifa_reset()`, staged `Stage-0 → IFA Analyze → Stage-1` attribute-driven macros (`pow_opt`, `if_const`, `sum_fields`).
- **Phase 4:** `Workbench` runtime, `comptime(...)` via LLJIT, CLI file runner, and interactive REPL.

### Racket-Inspired Zero-C++ Language Construction
- In-process LLJIT execution (`llvm_jit_execute_in_process`), persistent pre-compiled LLJIT modules (`compile_persistent_i64` / `run_persistent_i64`), and Phase-1 Meta-ABI (`stx_*`, `attr_*`, `grammar_add_rule`).
- `#lang <module>` protocol with nested `#lang` inheritance and Racket-style core interposition hooks (`hook module_begin`, `hook datum`, `hook app`, `hook field`, `hook record`) with phase separation (`run_hooks = false`).
- `for_syntax` shared Phase-1 bindings, `stx_local_expand` (re-entrant Turnstile-style cooperative expansion), syntax properties (`stx_set_prop` / `stx_get_prop` / `stx_has_prop`), declarative `syntax_rule` pattern macros with DParser syntax classes (`$x:expr`, `$id:ident`, `$n:int`), and foreign C primitives with guest-written IFA transfer functions (`prim ... c_call ... transfer => ...`).

### Post-Review Hardening & Core Architecture (Steps 1–4)
- **Step 1 (`7bb971c`):** Stage-1 macro fallbacks (returning `stx_call()` declines rewrite cleanly; `pow_opt` falls back to `_CG_wb_pow_i64`), quiet `parse_check`, `tests/run_tests.sh` suite, and README status/limitations.
- **Step 2 (`cce7c06`):** Per-rule `CoreRuleMeta` (`core(...)` role annotations via DParser rule indices with zero `dparser/` edits), expander/hooks/lowering dispatched on core tags rather than literal text, dedicated name-resolution pass with `line:col` + macro expansion provenance traces, `rec` (`letrec`), general call expressions (`(fn(x)=>x)(3)`, `p.f(1)`), and tree-walking Meta-ABI (`stx_is_core`, `stx_num_args`, `stx_text_eq`, `stx_symbol_eq`).
- **Step 3 (`b82590f`):** `seq`/`define` blocks (`{ def ...; ... }`) with Racket-style two-pass partial expansion and mutual recursion, self-hosted DParser `decl_grammar_` with `parse_prefix`, form-by-form top-level parsing (mid-file declarations & macro-generated `grammar_add_rule` taking effect for subsequent forms), variadic ellipsis (`$xs:expr...`) in `syntax_rule`, `stx_binding_eq`, and zero-C++ `examples/langs/prelude.lang` booted under `langwb --bare`.
- **Step 4 (`694e807`):** Per-contour IFA attributes (`ContourAttr`, `FnContourInfo`, `attr_contour_*`), decision-keyed syntactic function cloning (zero clones when contours agree; polymorphic `sum_fields` specialized per record shape), and bounded demand-driven constant splitting (`pow_opt(2, n)` unrolled per constant call site while runtime call sites use the generic fallback).

---

## Step 5 — Scoped Syntax, Speculative Parsing & GLR Ambiguity as Data

**Goal:** Make composing independently written syntax extensions safe, lexically scoped, and semantically disambiguated — leveraging DParser's GLR engine on purpose rather than only as a deterministic CST builder.

### 5.1 Lexically Scoped Syntax Regions (`using syntax <ext> in ...`)
- Allow a language module or expression to activate a syntax extension only inside a lexical region:
  ```text
  using syntax sql in
    select x from t where x > 3
  ```
- **Phase 1 (file-local extensions — safe & lexically isolated):** Parse the body of `using syntax <ext> in { ... }` as an opaque balanced token span, then re-parse that span at `expr` with `base_grammar + ext` via `GrammarCache`. Avoids cross-extension lexical/scanner collisions completely.
- **Phase 2 (library extensions — speculative guards):** Compile the union of extensions visible to a module once, attach speculative guard metadata to the per-rule table (using DParser's action index + `D_Scope` per-path symbol table), and `${reject}` disabled extension rules at shift/reduce time.
  - *Constraint:* Measure scannerless GLR lexical overlap on 2–3 real extensions before relying on unioned tables globally; keep balanced-span re-parsing as an explicit isolation mode.
  - *Rule:* Never run guest macros, fresh-scope allocation, or JIT calls speculatively (speculative actions must be pure, cheap, and rollback-safe).

### 5.2 GLR Ambiguity as Data (`amb` Nodes)
- Install a custom `ambiguity_fn` on `D_Parser` so ambiguous parses are **not** silently resolved by DParser's default heuristics (or aborted during parsing); instead, record all alternatives in an `Amb` `SyntaxObj` node (`CoreFormKind::Amb`).
- Follow the three-tier resolution rule ("resolve at the earliest tier that has the information"):
  1. **Parse tier (speculative):** Surface syntax visibility (`using syntax`, surface keyword/type declarations via `D_Scope`).
  2. **Expansion tier:** Resolve an `Amb` node when exactly one alternative's head is a bound macro or valid core form.
  3. **Analysis tier (IFA):** Resolve type-dependent ambiguities (e.g. an operator or syntax form that means one thing for records and another for integers) using `AnalysisAttrs` on demand.
  4. **Unresolved ambiguity:** Emit a clear diagnostic naming the conflicting alternatives and their source spans rather than choosing silently.

### 5.3 Grammar Hygiene for Macro-Generated Non-Terminals
- Scope non-terminal names introduced by Stage-0 `grammar_add_rule` with the macro's introduction scope so two independent macros generating a helper production named `item` cannot accidentally merge.

---

## Step 6 — Richer Guest Language & Metaprogramming Ergonomics

**Goal:** Make `minilang` (and languages built on `prelude.lang`) comfortable enough to write complex compilers, type-checkers, and tree-walking macros without ever missing C++.

- **Closure capture of outer locals:** Support capturing enclosing `let`/`def` variables in IFA closures (`fn(...) => ...` inside `let`/`def`) so recursive helpers don't need explicit environment parameters.
- **Lists / sequences & `match`:** Add immutable lists (or tuples/vectors) and structural pattern matching (`match`), plus `stx_children` / `attr_fields` iteration so macros like `sum_fields` can be written 100% in guest code.
- **Grammar-checked quasiquote syntax:** Upgrade `syntax_rule` string templates to native quasiquote syntax (e.g. `` `[expr| if ,(c) then ,(t) else ,(e) ] ``) parsed directly against the active `GrammarDef` with compile-time non-terminal checking and zero `__pat_x__` identifier-collision risk.
- **Richer runtime values:** Generalize top-level evaluation beyond `int64` (`eval_value` supporting integers, booleans, strings, records, and printable results in the REPL/CLI).

---

## Step 7 — Deeper IFA Integration & Diagnostics

**Goal:** Close the remaining gaps between user-level language semantics and IFA's whole-program analyzer.

- **Source-mapped IFA diagnostics:** Thread `SyntaxObj` `line:col` and macro expansion provenance (`origin_macro`, `origin_line`, `origin_col`) onto IF1 `WbSymbol` / `WbAST` nodes so IFA errors (e.g., unresolved record fields, type mismatches) report user source locations and macro backtraces instead of `__wb_rec_1` at `<unknown>:0`.
- **Declarative static semantics:** Allow `.lang` modules to declare custom Stage-1 type/lint checks over `AnalysisAttrs` and emit domain-specific errors in the guest language's own terminology.
- **Higher-order contour cloning & bounded fixpoint iteration:** Extend Step 4's decision-keyed cloning beyond direct `let f = fn(...)` bindings (including `def` blocks and indirect/record-stored functions) and support bounded `analyze → Stage-1 → analyze` iteration when a Stage-1 rewrite exposes new types or constants to downstream macros.
- **Upstream IFA demand hook (optional):** If value-splitting inside deeper call chains (beyond direct call sites) is needed, add a bounded `IFACallbacks` split-demand hook in `ifa/` and upstream via `make push-ifa`.

---

## Step 8 — Compiler Tooling & Showcases

**Goal:** Turn `langwb` from a JIT/REPL lab into an end-to-end language-building toolkit.

- **AOT compiler driver (`langwb build x.ml -o x`):** Wire IFA's LLVM AOT codegen + linker path to emit standalone native executables.
- **Language Server (LSP):** Leverage in-memory `GrammarCache`, source spans, and `AnalysisAttrs` to provide hover-to-see-inferred-type, go-to-definition (from the Step 2 resolution map), and show-macro-expansion in editors.
- **Runtime grammar library:** Expose `GrammarDef` / `DynamicGrammar` as a guest-accessible library so programs compiled by `langwb` can construct and run GLR parsers at their own runtime.
- **Re-entrant `IFAContext` (if needed):** Refactor IFA's process-global state only if a concrete use case requires running IFA *inside* an active IFA analysis pass (persistent LLJIT modules already handle nested Stage-0/Stage-1 macro execution).
