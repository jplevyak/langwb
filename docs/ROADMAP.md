# `langwb` Roadmap

This document tracks what `langwb` has completed and the next milestones toward a full zero-C++ programming language workbench built on DParser (scannerless GLR) and IFA (whole-program iterative flow analysis + LLVM ORCv2 LLJIT).

See [SKETCH.md](SKETCH.md) for architectural design decisions, [STEP11_DOGFOOD_LANG.md](STEP11_DOGFOOD_LANG.md) for the Step 11 dogfood language design, and [../reviews/](../reviews/) for the technical reviews that shaped Steps 1–10.5.

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
  - Nested GLR `Amb` resolution (`@(@p)`) preserving `PNode::ambiguities` on non-primary DParser branches and recursing into nested `Amb` alternatives while staying $O(k \cdot a)$ for independent sites; accurate file `L:C` offsets on declaration-body syntax errors; `true`/`false` literals and `\n`/`\r`/`\t` string escaping (list printing upgraded to runtime-tagged heterogeneous lists in Step 10).

- **Step 9 (Runtime Hygiene & Known Issues):**
  - **Unified JIT & AOT runtime:** Built `src/runtime/langwb_rt.c` directly into `langwb` (and `liblangwb_rt.a` for AOT) with a pluggable `_CG_wb_set_emit_sink`, removing all duplicate `_CG_wb_*` definitions from `workbench.cc` and `bridge.cc`.
  - **Diagnostics & capture hardening:** Human-readable record class names (`record { a, b }` instead of `__wb_rec_1`), upfront arity-mismatch errors (`wrong number of arguments to 'f': expected N, got M`), `check [core|call]` validation + unmatched-target warning, hard errors on missing recursive captures in `collect_free_captures`, and a per-`def` call-graph fixpoint for `Seq` closures so capture-free siblings remain plain functions.
- **Step 10 (Core Guest-Language Gaps):**
  - **Mutation & iteration:** Assignable locals (`x = expr`, `CoreFormKind::Assign`), mutable record fields (`p.x = expr`, `CoreFormKind::SetField`), and `while cond do body` loops (`CoreFormKind::While` via `if1_loop`).
  - **I/O & strings:** `print` / `println` (writing to `stdout` in both JIT and AOT across `int64`, `bool`, `string`, and lists), plus GC-allocated `str_concat`, `str_len`, `str_eq`, `str_slice`, `int_to_str`, and `str_to_int` in `src/runtime/langwb_rt.c`.
  - **Minimal tagged values & heterogeneous lists:** Runtime-tagged `WbCons` (`WB_TAG_INT`/`BOOL`/`STR`/`LIST`/`OBJ`) with `cons`, `head_tag`, `head_str`, `head_bool`, and `head_list`, supporting heterogeneous and nested lists (`[1, "two", true, [3, 4]]`) end-to-end in JIT `eval_value` and AOT binaries.
  - **Multi-file `import` / `export`:** Hygiene-preserving module imports (`import "path";` / `import name;`) with per-module `ScopeId`, explicit `export` lists (values, macros, `syntax_ext`, and opt-in `export grammar;`), private hiding, and single instantiation per program on diamond imports.
- **Step 10.5 (Sum Types / ADTs in IFA):**
  - Declarable variant types (`type Option = None | Some(val);`, `type Result = Ok(val) | Err(msg);`, recursive `type Expr = Lit(n) | Add(lhs, rhs);`) with constructor calls/nullary literals and `match` patterns (`None`, `Some(v)`, `Add(l, r)`).
  - Each variant lowers to a distinct `Type_RECORD` class (`T::V`) with its own `CreationSet`; `match` lowers to IFA's `P_prim_isinstance` with branch-sensitive SSU narrowing (preserving per-variant field types such as `Ok(int64)` vs `Err(string)` in the same `Result`), dead-arm elision, and contour specialization on variant (plus a `cg_isinstance_tag_targets` `Sym::id` matching fix in `ifa/codegen/codegen_common.cc`).

- **Step 11 (Dogfood: MLite, a small ML in `.lang`):** see [STEP11_DOGFOOD_LANG.md](STEP11_DOGFOOD_LANG.md).
  - `examples/langs/mlite.lang` replaces the minilang grammar and implements `val` / `var` / `fun` / `fn`, multi-binding `let`, `if`, `case ... of` over ADTs (a guest `form_macro`), `while` / `for` / `do` / `unless`, records, `|>`, `and` / `or` / `not`, Stage-1 `fast_pow`, `check call safe_div` / `assert`, C prims (`llabs`, `clock`, `exit`), a `module_begin` hook, and `syntax_ext power` (`**`), with no MLite-specific C++.
  - Multi-file stdlib `examples/mlite/std/{option,result,list,assert}.mlite` and four target programs (`expr_eval`, `wordcount`, `graph_cycle`, `aot_cli`) tested under the JIT, as AOT binaries, and through the LSP.
  - Friction log FL-1..FL-23. General workbench fixes made along the way (each with a minilang-only test): `for_syntax` under replaced grammars (FL-1), top-level syntax-error locations (FL-3), Meta-ABI `stx_text` / `stx_copy_loc` (FL-6/7), `syntax_rule` template termination (FL-9), imports of `#lang` modules / nested ADTs / aliases used by value defs (FL-12..14), `#lang` checks on library functions (FL-17), prim monotonicity with constant widening (FL-19), working-directory-independent `#lang` lookup (FL-20), and LSP hover / `using syntax` positions / cross-file definition (FL-21..23).

---

## Next Steps

Step 11's dogfood language (MLite) produced the friction log that drives Steps 12–13. Steps are ordered; items under *Deferred* wait for a concrete need or profiling data.

### Step 12 — Diagnostics in the Language Author's Terms
- **Structured diagnostics:** replace stderr text parsing with `std::vector<Diagnostic>` (severity, range, message, `relatedInformation` for macro provenance) produced throughout the pipeline.
- **Custom language diagnostics:** beyond the `__wb_*` name leaks and arity message fixed under *Known Issues*, map the remaining IFA failures (`expression illegal: int64`, `expression has no type`, etc.) back to guest constructs, and let `.lang` modules customize error messages for their own forms.
- **Richer LSP:** multi-line hover spans and debounced / isolated document evaluation, built on the structured diagnostics.
- **From the Step 11 friction log** (baselines recorded verbatim in `tests/run_tests.sh`):
  - Template-relative positions in `syntax_rule` expansions (FL-11) and macro-relocated template nodes surfacing in hover (`__wb_is_variant(...)` on a `case` pattern); `match` / `__wb_fn_N` / `__wb_deffn_*` / `<input>` wording leaks and duplicated `expression has no type` errors (H2).
  - A diagnostic for operator-priority productions mixed with rule-level `$left N` (FL-2), and opt-in reserved keywords (FL-4).
  - Immutable bindings (`val` is assignable, FL-10): a `const` flag on binding forms or a Meta-ABI path from an identifier to its binder.
  - Module semantics: constructor visibility that follows import edges (FL-15) and `module_begin` scoped to the entry module's own forms (FL-18).


### Step 13 — Inspecting a Language Under Construction
- **`langwb explore <file>`:** integrate the IFA HTML explorer (`ifa/common/html.cc`, `explorer.js`) to visualize each stage: parse and `Amb` alternatives → Stage-0 expansion with hygiene scopes → IFA types / constants / contours → Stage-1 rewrites and clones. This is the workbench analogue of Racket's macro stepper.
- **Remaining analysis attributes (SKETCH.md goal 5):** expose liveness, purity, call targets, and execution frequency to Stage-1 macros via the Meta-ABI.
- **From the Step 11 friction log:** Meta-ABI list builders / splicing helpers for variable-length syntax (FL-8, F5); non-`int64` constant queries and transfer results (`bool` constants for `check call assert`, `void` / string C functions; F6); contour-insensitive checks such as `safe_div` missing a zero merged across shared contours.

### Deferred (needs a concrete use case or data)
- **Step 11 leftovers:** empty grammar alternatives in `.lang` (FL-5; `params?` suffices so far), Boehm finalization-cycle warnings from `gc_cleanup` syntax nodes (FL-16; filtered in tests), mutual recursion among `for_syntax` helpers (FL-8), and primitives as first-class values (`show_result(r, int_to_str)`).
- **Benchmark / profiling harness:** measure grammar recompilation, `GrammarCache` hit rates, and per-macro / `comptime` JIT cost. Prerequisite for the next item.
- **Speculative unioned tables (Phase 2 of Step 5.1):** if profiling shows many small `using syntax` regions needing a single unioned table, compile unioned library extensions with speculative guards (`${reject}`) via DParser's `D_Scope`. Balanced-span re-parsing stays the default isolation mode.
- **Runtime grammar library:** expose `GrammarDef` / `DynamicGrammar` as a guest-accessible library so programs compiled by `langwb` can construct and run GLR parsers at their own runtime.

### Non-Goals
- **Re-entrant `IFAContext`:** IFA's process-global state stays as is; persistent LLJIT modules already handle nested Stage-0/Stage-1 macro execution, and no use case requires running IFA inside an active IFA pass.
