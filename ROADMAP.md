# `langwb` Roadmap

This document tracks what `langwb` has completed and the next milestones toward a full zero-C++ programming language workbench built on DParser (scannerless GLR) and IFA (whole-program iterative flow analysis + LLVM ORCv2 LLJIT).

See [SKETCH.md](SKETCH.md) for architectural design decisions, [reviews/2026-11-21-review.md](reviews/2026-11-21-review.md) for the technical review that shaped Steps 1–4, [reviews/2026-11-22-review.md](reviews/2026-11-22-review.md) for the design review that shaped Hardening Step 4.5 and Steps 5–8, and [reviews/2026-11-22-review-1f88a19..ce2fa9b.md](reviews/2026-11-22-review-1f88a19..ce2fa9b.md) / [reviews/2026-11-22-review-ce2fa9b..HEAD.md](reviews/2026-11-22-review-ce2fa9b..HEAD.md) for the follow-up hardening passes.

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
- **Hardening Step 4.5 (`ca5b727`..`8bdf821`):** Real closure capture of outer locals (via IF1 `Partial_OK` closures + LLVM partial-application closure emission in `ifa/codegen/cg_emit_llvm.cc`, including curried and recursive capturing closures); extracted contour cloning into `src/workbench/contour_clone.cc`, unified the staged pipeline into `Workbench::run_staged_and_jit_i64`, and added a bounded `analyze → Stage-1 → analyze` re-analysis loop so demand-driven constant splitting propagates through expressions (`pow_opt(2, n + 1)`) and nested calls (`f(n) => g(n)`); threaded `SyntaxObj` `line:col` and macro expansion provenance onto `WbSymbol`/`WbAST` so IFA errors report `<input>:L:C` or `<macro:name@L:C>`.
- **Step 5 (Scoped Syntax, GLR Ambiguity as Data & Grammar Hygiene):**
  - Lexically scoped syntax regions (`syntax_ext <name> { ... };` and `using syntax <ext> in { ... }`, plus loading `.lang` modules as scoped extensions) with balanced raw-block isolation, `GrammarCache` reuse, enclosing lexical scope preservation, and zero leakage outside the region.
  - GLR ambiguity as data (`CoreFormKind::Amb` via DParser's `ambiguity_fn` with zero `dparser/` edits) with 3-tier disambiguation: Tier 1 (parse precedence / `using syntax` isolation), Tier 2 (expansion-tier elimination of invalid/unexpanded alternatives), Tier 3 (IFA analysis-tier type-directed disambiguation, e.g. resolving `@p` on records vs `@n` on integers in the same program), and clear diagnostics listing conflicting alternatives when unresolved.
  - Grammar hygiene for macro-generated non-terminals: `grammar_add_rule` automatically scopes newly introduced helper non-terminals per macro invocation and rewrites references in subsequent rules from the same macro call.
- **Step 6 (Richer Guest Language & Metaprogramming Ergonomics):**
  - Immutable GC-allocated lists (`[]`, `[e1, e2, ...]`, `nil`, `cons`, `head`, `tail`, `is_nil`, `list_len`), structural `match` (`match scrut { pat => body, ... }` for integers, wildcards, bindings, `[]` / `[h | t]`, and `{ x: a, y }` record destructuring), and `attr_field_name(stx, idx)` so Stage-1 macros like `sum_fields` can be written 100% in guest code.
  - Grammar-checked quasiquote syntax (`` `[nt| ... ,(expr) ... ] ``) parsed and validated at compile time against the active `GrammarDef` with guaranteed-fresh splice placeholders.
  - Generalized top-level `EvalValue` / `eval_value` supporting `int64`, `bool` (`true`/`false`), `string`, `int64` `list`, `record`, and `<fn>` values in the CLI and REPL.
- **Step 7 (Deeper IFA Integration & Diagnostics):**
  - Declarative Stage-1 static semantics (`check <core_or_call_name> => <guest-expr>;`) with `stx_error(node, msg)`, `attr_is_type`, `attr_has_field`, and `attr_is_monomorphic` in the Phase-1 Meta-ABI so `.lang` modules can enforce domain-specific contracts and compile-time lints with exact `line:col` diagnostics.
  - Higher-order contour cloning extended to `def` blocks (`CoreFormKind::Seq`) and record-stored functions (`let ops = { total: fn(p) => ... } in ops.total(...)`), plus unified `Fun`-clone contour collection in `analyze_syntax` so polymorphic functions called with mixed-arity records (`{a, b}` + `{x}`) specialize cleanly.
- **Step 8 (Compiler Tooling — AOT Driver & Language Server):**
  - **AOT compiler driver (`langwb build <file> [-o <out>] [--emit-llvm]`):** Compiles guest programs through the full staged pipeline (`Parse → Stage-0 → IFA Analyze → Stage-1 / Contour Cloning → Final IFA → LLVM IR`) and either emits LLVM IR (`--emit-llvm`) or links a standalone native executable with Boehm GC and the precompiled `liblangwb_rt.a` runtime (`src/runtime/langwb_rt.c`) via `fork`/`execvp` + `mkstemps` in `$TMPDIR` (supporting `int64`, `bool`, `string`, `int64` `list`, and `record` results).
  - **Language Server (`langwb --lsp`):** JSON-RPC 2.0 stdio LSP server backed by `Workbench::inspect_document`, a recursive-descent `JsonParser`, `tmpfile()` stderr capture (no pipe-buffer deadlock), `positionEncoding: "utf-8"`, live diagnostics (`textDocument/publishDiagnostics` with `<macro:name@L:C>` call-site mapping), hover-to-see-inferred-type/const/fields (`textDocument/hover`), and go-to-definition (`textDocument/definition`).
- **Post-Step-8 Ergonomics & Review Hardening (`4595a57`, `455c599`):**
  - Native DParser `.g`-style `grammar` declarations (`+=` extend, `:` define new, `:=` replace, `|` alternatives, `grammar { ... }` blocks) rejecting legacy quoted RHS rules, plus unquoted eagerly grammar-checked `syntax_rule` templates with same-length placeholder column preservation.
  - Eagerly parsed `SyntaxExtDelta` replaying `GrammarOp`s (`clear`/`add`) and `stage0` macros cleanly without re-parsing on each `using syntax`.
  - Nested GLR `Amb` resolution (`@(@p)`) preserving `PNode::ambiguities` on non-primary DParser branches and recursing into nested `Amb` alternatives while staying $O(k \cdot a)$ for independent sites; accurate file `L:C` offsets on declaration-body syntax errors; `true`/`false` literals, `\n`/`\r`/`\t` string escaping, and `int64`-only list enforcement.

---

## Future Enhancements (Optional)

- **Typed / heterogeneous lists:** Extend `wb_list` beyond `int64` elements (e.g. creation-set-typed or tagged elements) so lists of strings, records, or nested lists work end-to-end.
- **Unified JIT & AOT runtime:** Share `src/runtime/langwb_rt.c` into the `langwb` binary so JIT and AOT runtime primitives cannot drift (keeping only the output sink separate).
- **Structured diagnostics & richer LSP:** Replace stderr text parsing with a structured `std::vector<Diagnostic>` (with `relatedInformation` for macro provenance), multi-line hover spans, and debounced/isolated LSP evaluation.
- **Speculative unioned tables (Phase 2 of Step 5.1):** If profiling shows many small `using syntax` regions needing a single unioned table, compile unioned library extensions with speculative guards (`${reject}`) via DParser's `D_Scope`. Keep balanced-span re-parsing as the default isolation mode.
- **Runtime grammar library:** Expose `GrammarDef` / `DynamicGrammar` as a guest-accessible library so programs compiled by `langwb` can construct and run GLR parsers at their own runtime.
- **Re-entrant `IFAContext` (if needed):** Refactor IFA's process-global state only if a concrete use case requires running IFA *inside* an active IFA analysis pass (persistent LLJIT modules already handle nested Stage-0/Stage-1 macro execution).
