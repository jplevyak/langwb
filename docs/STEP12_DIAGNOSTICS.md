# Step 12 — Diagnostics in the Language Author's Terms

> **Goal (from [ROADMAP.md](ROADMAP.md)):** a language built in `.lang` should
> report errors, and answer LSP queries, in *its own* terms: its keywords, its
> forms, its source positions. It should not leak minilang, IF1, or IFA. Step 11
> ([STEP11_DOGFOOD_LANG.md](STEP11_DOGFOOD_LANG.md)) built MLite and recorded
> where today's diagnostics fall short. Its friction log and the error texts
> pinned verbatim in `tests/run_tests.sh` are this step's baseline.
>
> **Status: done (M0–M4).** §7 lists every changed expectation and what was left for later.

---

## 0. Approach: Baseline First, Plumbing Before Wording

1. **Plumbing first, with no visible change.** M0 routes every diagnostic
   through one structured type. The command-line renderer must reproduce
   today's text byte for byte, so all 471 tests pass unchanged. Only after
   that does any wording change.
2. **Every wording change is a before/after pair.** Step 11 pinned the
   current error texts (`expect_error`, `lsp_expect`, `mlite_m*_error`). A
   milestone that improves a message updates those expectations and records
   the change in §7 (before → after, with the test name). Silent rewording is
   not allowed.
3. **MLite is the acceptance corpus.** A milestone is done when its MLite
   error cases read in MLite's terms. Each general change also gets a
   minilang-only regression test (as in Step 11, §6.2 there).
4. **No new IFA or DParser policy in the subtrees.** IFA already exposes
   structured violations (§1). If an `ifa/` or `dparser/` change turns out to
   be needed (one is expected: the `d_fail` handler hook in §2.1), it is small
   and self-contained, gets its own commit with a test in that subtree, and is
   written so it can go upstream.

---

## 1. Where Diagnostics Come From Today

| Source | Mechanism | Volume |
|---|---|---|
| Workbench, parser, expander, bridge, CLI | `std::fprintf(stderr, ...)` with hand-formatted `L:C:` / `<file>:L:C:` / `<macro:NAME@L:C>:` prefixes | ~115 call sites (`workbench.cc` 48, `bridge.cc` 31, `cli/main.cc` 16, `expander.cc` 8, `contour_clone.cc` 6, `meta_abi.cc` 4, `parse.cc` 2, `prim_registry.cc` 1), plus `d_fail` inside `dparser/util.c` during grammar compilation, which prints `fail: ...` and calls **`exit(1)`** (`grammar zz_e : 'a' undefined_nt;` in a `.lang` or program kills `langwb`, including `langwb --lsp`, today) |
| IFA type violations | `FA::type_violations` (`Vec<ATypeViolation *>`: kind, `AVar`, offending `AType`, call chain), rendered to text by `show_violations` (`ifa/analysis/fa_debug.cc`) at the end of analysis (`fa.cc`: `if (show_violation_output) show_violations(fa, stderr)`) | every type error (`illegal primitive argument type ...`, `has no type`, `unresolved member ...`, `called from ...`) |
| Guest `check` / macros | `stx_error(node, msg)` → `fprintf` | per `.lang` |
| LSP | `CaptureStderr` redirects fd 2 during `inspect_document`, then `parse_diag_loc` re-parses each line's prefix back into a range | all of the above |

Facts that shape the design:

- **IFA already has a host hook.** `IFACallbacks::report_analysis_errors(Vec<ATypeViolation *> &)`
  (`ifa/ifa.h:194`) is called with the final violations, just before the
  text dump. langwb does not implement it. `FA::show_violation_output`
  switches the dump off. So structured IFA errors need **no `ifa/` change**.
- **The way back to syntax is lossy.** `bridge.cc` attaches a `WbAST` to each
  IF1 `Code` and `Sym` (`make_wb_ast` / `attach_loc`). It copies only
  `filename` / `line` / `col`, not the `SyntaxObj`. Mapping a violation back
  to a guest form, its core kind, macro origin, or syntax properties is
  therefore impossible today.
- **Positions are points, not ranges.** `SyntaxObj` has `line` / `col` and
  `text`. The LSP measures spans by `text.size()`, which is wrong for
  multi-line nodes and for macro-relocated template nodes (FL-21).
- **Syntax properties are `int64`-valued** (`SyntaxObj::props`,
  `stx_set_prop(h, key, int64)`). A guest-supplied description string needs
  a new Meta-ABI call.
- **The LSP runs guest Phase-1 code in-process.** `inspect_document` does
  not execute the program, but it does run `for_syntax` helpers, macros,
  `check`s, and transfer functions. An infinite loop in a macro hangs the
  server, and a DEBUG assertion kills it.
- **DParser binary tables already wire `spec_code` on every rule.** Every
  non-internal rule in `DynamicGrammar::compile` gets `action_index >= 0`, for
  which `write_tables.c` emits `#spec_code` relocations resolved by
  `read_binary_tables_from_string(..., spec_code, nullptr)` (langwb passes
  `nullptr` today). That gives FL-4 a speculative-rejection hook on
  identifier/binder reductions without changing `dparser/` or breaking
  scannerless subgrammars (`raw_block`, `case` patterns; whereas
  `${declare tokenize ...}` in `dparser/gram.c` merges `shift_actions` across
  *every* state in the grammar, `compute_declaration_states`, despite
  `terminals.md` describing it as limited to the named non-terminals).
  Constraints: one callback serves every reduction of every grammar
  (including `decl_grammar_`), so it must find its grammar's reserved set
  through the `D_Parser`. It identifies the rule through the `PNode` it
  receives (`PNode::reduction->action_index`, from dparser's internal
  `parse.h`), and it runs on *every* reduction once installed, so it should be
  installed only for grammars that declare `reserve`.

### 1.1 Baseline (from Step 11; pinned in `tests/run_tests.sh`)

| # | Today | Where pinned |
|---|---|---|
| B1 | `<macro:not_e@2:89>:1:89: error: illegal primitive argument type 'if condition (expected bool)' illegal: int64` for `not 1 == 2`: template-relative position, and an `if` the user never wrote (FL-11) | `mlite_m2_not_binds_tighter_than_eq` |
| B2 | `... 'match scrutinee (expected MList)' illegal: __wb_fn_10`: internal closure name, minilang's `match` (H2) | Step 11 §9, M2 notes |
| B3 | `unknown variant 'Some' in match pattern` from an MLite `case` | `mlite_m5_lsp_unknown_variant` |
| B4 | `expression has no type` reported alongside (and before) the real error, often several times | `mlite_m2_*`, LSP diagnostics |
| B5 | Main-program errors labelled `<input>` even when the program is a file | `mlite_m4_safe_div_*`, `mlite_m5_*` |
| B6 | `<macro:fast_pow@1:0>` for an error inside `using syntax power in { 2 ** "a" }` (`**` → `syntax_rule` → `fast_pow`; FL-11) | Step 11 M5 notes |
| B7 | Hover `fold_l : __wb_deffn_fold_l_1`; hover on a `case` pattern shows `__wb_is_variant(__m__, "Circle", 1, "Circle(r)") : bool` | `mlite_m5_lsp_hover_definition`, Step 11 H6 |
| B8 | Hover on a binder (`val scale`) and definition of a constructor return nothing | Step 11 H6 |
| B9 | `val x = 1; x := 2` is accepted (FL-10) | `mlite_m2_val_is_assignable_fl10` |
| B10 | Rule-level `$left N` on an operator sub-production silently misgroups (FL-2); keywords are identifiers (FL-4) | `op_priority_*`, FL-4 |

---

## 2. Design

### 2.1 The `Diagnostic` type

```cpp
struct SourceRange { const char *file; int line, col, end_line, end_col; };  // 1-based lines

struct Diagnostic {
  enum class Severity { Error, Warning, Note } severity;
  std::string code;            // stable id, e.g. "type.primitive-arg", "syntax", "check"
  SourceRange range;
  std::string message;         // in the guest language's terms (§2.3)
  std::vector<Diagnostic> notes;  // macro provenance ("in expansion of 'for' at 3:2"), call chain
  uint64_t node_id = 0;        // originating SyntaxObj, when known
};
```

- **A `DiagnosticSink`** lives on the `Workbench` (shared by clones, like
  `stats_`). Every current `fprintf(stderr, ...)` diagnostic site emits into
  it. Non-diagnostic output (`--dump-*`, REPL banners) stays as it is.
- **Grammar compilation errors.** Capturing `d_fail`'s `stderr` is not
  enough, because `d_fail` calls `exit(1)`. Two layers:
  1. `GrammarDef` validates what users commonly get wrong before calling
     DParser: unresolved non-terminals, empty literal terminals, and malformed
     `core(...)` roles. It reports them at the offending `grammar` declaration
     (`.lang` file, line, column) in the language author's terms.
  2. A backstop: small, upstreamable `dparser/` hooks,
     `d_set_fail_handler(void (*)(const char *msg))` and
     `d_set_warn_handler(void (*)(const char *msg))` in `dparser/util.c`,
     defaulting to today's `stderr` print-and-exit / print. `DynamicGrammar::compile`
     installs handlers that record warnings into the sink and record `d_fail`
     before `longjmp`ing out of `mkdparse_from_string` /
     `write_binary_tables_to_string` (the leaked tables are GC-owned), turning
     failures into a `Diagnostic` instead of exiting.

  Until then, M4's worker isolation turns the exit into a "crashed" diagnostic
  rather than a dead server.
- **Renderers:**
  - The CLI text renderer prints `file:L:C: error: message`, then indented
    `note:` lines.
  - The LSP renderer maps each `Diagnostic` to an LSP diagnostic directly,
    with notes as `relatedInformation`. `CaptureStderr` and
    `parse_diag_loc` are deleted.
  - In M0 the text renderer reproduces today's exact strings, macro-prefix
    form included; the format changes in M2.
- **File names (B5).** The main program is labelled by its path, as given
  on the command line or by the LSP URI. `<input>` remains only for `-e`, the
  REPL, and embedder strings.
- **Ranges.** `D_ParseNode` stores `d_loc_t start_loc` and `char *end` (no
  `end_loc` struct), so `build_cst_single` advances `(start_loc.line,
  start_loc.col)` over `[start_loc.s, end)` to populate `CstNode` and
  `SyntaxObj` `end_line` / `end_col`. These are preserved through `clone`,
  `stx_copy_loc` (both start and end), and `offset_syntax_loc` (which offsets
  `end_col` only when `end_line == 1`, before adding `line_delta`). This fixes
  multi-line spans and lets the LSP measure nodes properly instead of using
  `text.size()`.

### 2.2 IFA violations as structured data

- `WbAST` gains `const syntax::SyntaxObj *node` (set by `make_wb_ast` /
  `attach_loc`). This is a langwb-side change only.
- `WbCallbacks::report_analysis_errors` converts each `ATypeViolation` into a
  `Diagnostic` (skipping `is_uninformative_violation` entries and keeping
  `MAYBE_UNBOUND` at warning severity, matching `fa_debug.cc`'s
  `show_violations`). It uses the violation's `AVar` → `Var` → `Sym` / defining
  `Code` → `WbAST::node` to find the guest node, and the `send` / `pnode`
  chain for `called from` notes. langwb sets `fa->show_violation_output = false`.
- **Multi-pass & speculative analyses.** `ifa_analyze` runs multiple times in a
  staged compilation (`analyze_syntax` before Stage-1, bounded `contour_clone`
  re-analysis passes, Tier-3 `Amb` candidate trials, the non-staged
  `out_top_fields` probe, and the final pass, which is `run_staged_and_jit`'s
  `ifa_analyze` for JIT/AOT and `inspect_document`'s `analyze_syntax` of the
  final tree for the LSP):
  - Tier-3 `Amb` candidate trials (today under `QuietStderr`) and the
    `out_top_fields` probe run against a discarded local sink, so rejected
    alternatives and probes never leak violations.
  - Pre-Stage-1 `analyze_syntax` passes buffer their IFA diagnostics and commit
    them to the main sink *only* if compilation aborts before the final pass
    (preventing duplicate reports while still reporting final `analyze_syntax`
    errors in `inspect_document` / `--lsp`).
- **Deduplication (B4).** A `NOTYPE` ("has no type") violation whose node is
  an ancestor or descendant of a node with another violation is dropped. So
  is one that is downstream of an earlier error in the same expression. Only
  the root cause is reported, and a `NOTYPE` with no other violation nearby
  stays as an error.
- **Messages** are built from the kind plus the guest node (§2.3), not from
  IFA's text. `fa_debug.cc`'s wording remains the fallback for kinds with no
  guest mapping yet.

### 2.3 Guest terms

1. **Descriptions on generated nodes.** A new Meta-ABI call
   `stx_describe(h, "operand of 'not'")` stores a role description on a
   generated core node (preserved through `clone`, *not* propagated downward
   into spliced user sub-expressions, so `not (1 + "a")` still reports the `+`
   error rather than blaming `not`). The message builder uses a core node's
   description in place of its default IF1 label (e.g. `CoreFormKind::If`'s
   `test` check uses the `If` node's description): `'not' operand must be a
   bool, got int64` instead of B1's `'if condition (expected bool)'`. MLite's
   `and` / `or` / `not` / `for` / `case` / `|>` macros set descriptions on
   the nodes they generate. Declarative `syntax_rule` templates get a fallback
   provenance note automatically (`in expansion of 'not_e'`).
2. **Locations and provenance chains of template nodes (FL-11, B1, B6).**
   `SyntaxObj`'s single `(origin_macro, origin_line, origin_col)` triple is
   replaced by a small provenance chain. Today `with_origin` keeps the
   *first* origin a node gets. A node made by an inner expansion therefore
   records only the innermost macro and its call position. In B6
   (`2 ** "a"` → `pow_e` → Stage-1 `fast_pow`) that position is itself
   template-relative (`fast_pow@1:0`), and the `pow_e` step at the user's line
   is lost. With a chain, each expansion step adds its macro and call site.
   When a `syntax_rule` or guest macro expands:
   - template-introduced nodes inherit the call site's range;
   - spliced pattern variables / user arguments keep their own range;
   - each expansion step becomes a `note` ("in expansion of `not` at 2:0")
     instead of a `<macro:...@L:C>:` prefix.

   This is what FL-7's `stx_copy_loc` did by hand, now done by default
   (`stx_copy_loc` remains available when a macro wants a template node to point
   at a specific sub-clause such as a `case` arm pattern).
3. **Display names (B2, B7).** One table maps internal names to display
   names, and both the message builder and LSP hover use it:
   - `__wb_deffn_<f>_<n>` → `f`;
   - `__wb_fn_<n>` → `fn at L:C`;
   - `__m__` and other template temporaries → their description, or are
     omitted.
4. **Neutral wording (B3).** Messages produced by the workbench's ADT
   support say `pattern` / `constructor`, not `match`. A language can rename
   its own forms through descriptions (`case` arm).
5. **Deferred: a `diagnostic <form> => ...` declaration** that receives a
   structured error and returns a custom message. Descriptions cover every
   Step 11 case. Add the declaration only if MLite still needs it after M2.

### 2.4 Grammar and binding diagnostics (FL-2, FL-4, FL-10)

- **FL-2.** Warn when a grammar declaration gives rule-level `$left N` /
  `$right N` to a production whose operator is a non-terminal, or when the
  same `expr` mixes rule-level associativity with operator-priority
  productions. The warning names the production and points to the uniform
  form (`expr op_nt expr` with `$binary_op_left N` on `op_nt`).
- **FL-4.** Opt-in reserved keywords: a `reserve 'if', 'then', ...;` item
  inside `grammar { ... }`, stored in `GrammarDef` (and `cache_key()`) and
  enforced via `read_binary_tables_from_string`'s `spec_code` callback on
  identifier and binder slots (`CoreFormKind::Ident`, `bind` / `params`, and
  optionally record `field` slots), leaving `raw_block`, `case` constructor
  patterns, and scannerless states untouched while rejecting reserved words
  during GLR parsing and `parse_prefix`. When `DynamicGrammar::parse` fails at
  `p->loc.s` on a reserved word, the error names it (`syntax error: 'if' is a
  reserved keyword`). Minilang reserves nothing, so behaviour is unchanged.
- **FL-10.** A `const` flag on binding core forms
  (`core(define, bind=1, rhs=3, const)`, `core(let, ..., const)`,
  `core(letrec, ..., const)`, included in `GrammarDef::cache_key()`), enforced
  by the resolver. `x := e` on a const binding is an error at the
  assignment, with a note at the binding: `cannot assign to 'x': it is
  immutable` / `note: 'x' is bound here`. MLite marks `val` const; `var` and
  minilang `def` stay mutable. (This is preferred over a Meta-ABI path from
  an identifier to its binder, which Step 13's tools may still want.)

### 2.5 LSP

- **Hover and definition gaps (B7, B8).**
  - Hover on a binder shows its type; the bridge annotates binder nodes,
    not only uses.
  - Definition on a constructor goes to its `type` declaration.
  - Hover uses display names (§2.3), and hides template-only nodes that
    have no description.
  - Hover answers include a `range` (from §2.1's ranges), so multi-line
    spans highlight correctly.
- **Debounce & cancellation.** The parent server `poll()`s `STDIN_FILENO` and
  any active worker pipe so queued `didChange` notifications are coalesced and a
  newer `didChange` immediately `SIGKILL`s an in-flight worker for the same
  document rather than waiting out its timeout; `hover`/`definition` are
  answered from the last completed evaluation.
- **Isolation.** The parent `--lsp` process stays a pure JSON-RPC router: it
  never constructs a `Workbench` or initializes LLVM LLJIT, and it stays
  **single-threaded** so that `fork` is safe (no Boehm marker threads or LLVM
  threads to lose across `fork`, the hazard on macOS).
  - *Avoiding Boehm GC in the parent is not possible.* `libgccpp` replaces
    the global `operator new`, and `langwb`, `libstdc++`/`libc++`, and
    `libLLVM` all bind `_Znwm` to it (needed so standard C++ containers
    holding `gc`-managed `SyntaxObj *` / `IFAAST *` pointers are scanned via
    `GC_malloc_uncollectable`). Because dynamic-linker initializers in
    `libLLVM` run before `main` (and even before `__attribute__((constructor))`
    in the main binary), `GC_is_init_called()` is already `1` on entry to
    `main`. `GC_INIT()` stays first in `main` for main-thread stack-bottom
    registration on platforms that expect it.
  - Because `GC_init()` has already run before `main`, pre-init setters such
    as `GC_set_handle_fork(1)` and `GC_set_markers_count(1)` inside `main` are
    silent no-ops (`gc/gc.h` documents both as having no effect after GC
    initialization). Instead:
    - Each worker `fork()` is bracketed with `GC_atfork_prepare()`,
      `GC_atfork_parent()`, and `GC_atfork_child()` (`gc/gc.h`). When `libgc`
      was built with `HANDLE_FORK` enabled by default (e.g. Homebrew macOS,
      which registers `pthread_atfork` during `GC_init`), those three calls
      are automatic no-ops; when `HANDLE_FORK` was not compiled in
      (`GC_handle_fork == 0`), they run Boehm's fork hooks directly.
    - Boehm's `PARALLEL_MARK` collector does not spawn marker threads during
      `GC_init()` or single-threaded collections — only when
      `GC_start_mark_threads()` is called or a non-main thread is created
      (unless `GC_MARKERS` / `GC_NPROCS` is set in the environment). Keeping
      the parent single-threaded therefore keeps `GC_get_parallel() == 0`
      (`threads == 1`), verified by a debug-build check asserting that the
      process has one thread before each `fork`.
  - Today `main` constructs a `Workbench` before calling
    `run_lsp_server(wb)`. That changes to `run_lsp_server(const LspOptions &)`
    (carrying `--bare` and the search path), with each worker constructing
    its `Workbench` after `fork`.

  Each document
  evaluation runs in a forked worker with a wall-clock timeout (default 5 s,
  configurable). The worker sends back a serialized *document index*:
  diagnostics plus a per-range table of hover texts and definition targets. The
  server never holds the worker's `SyntaxObj` tree. A worker that crashes or
  times out produces one diagnostic at the top of the document ("evaluation of
  this document crashed / timed out in `<phase>`"), and the server keeps
  running. This matters most while writing a `.lang`, which is also Step 13's
  use case.

### 2.6 Out of scope: module semantics (Step 12.5)

FL-15 (constructor visibility should follow import edges) and FL-18
(`module_begin` should wrap only the entry module's own forms) are semantic
changes, not diagnostics, and can break existing programs. Recommendation:
a small, separate **Step 12.5 — Module semantics** after M2, with its own
before/after tests. Step 12 leaves their pinned tests
(`mlite_m3_ctor_visible_transitively_fl15`,
`mlite_m4_module_begin_reaches_imports_fl18`) untouched.

---

## 3. Milestones

| Milestone | Scope | Exit criterion |
|---|---|---|
| **M0** ✅ | `Diagnostic`, `DiagnosticSink`, text and LSP renderers; every diagnostic site (including a verbatim `report_analysis_errors` pass-through with `show_violation_output = false`, rendering through `show_violations` from IFA's internal `fa_internal.h`, plus `show_numeric_coercions` should langwb ever set `fruntime_errors`) emits into the sink; `SyntaxObj` end positions; `CaptureStderr` / `parse_diag_loc` removed; grammar-compilation errors no longer exit (`GrammarDef` validation + `d_fail` / `d_warn` handler hooks, §2.1). | All tests pass **unchanged** (text output byte-identical; no test pins DParser's `fail:` output). The LSP builds diagnostics without parsing text. A `Diagnostic`-level unit test covers each renderer. New tests: an unresolved non-terminal in a `.lang`, and in an LSP document, give a located error, and the process (and LSP server) keeps running. **Done:** `src/diag/` (`Diagnostic`, sinks, `Builder`); every diagnostic site emits into the current sink (CLI text byte-identical); IFA violations via `report_analysis_errors` + `show_violations_each` (`ifa/`); LSP from structured ranges (`CaptureStderr` / `parse_diag_loc` removed); `d_set_fail_handler` / `d_set_warn_handler` (`dparser/`) + located undefined-non-terminal errors; CST / `SyntaxObj` end positions; unit tests in `--smoke`. See §7. |
| **M1** ✅ | Structured IFA violations in `report_analysis_errors`: `WbAST::node`, `NOTYPE` deduplication, `called from` and macro provenance as notes. | B4 gone, with no other IFA message lost (every `expect_error` still matches its key text). Before/after table in §7. **Done:** `WbAST::node`; violations collected with guest node, call sites, and call chains; dedup by IF1 node / guest-node span / untyped inputs / call chain; call sites and macro expansions as notes (LSP `relatedInformation`), also for `print_origin_suffix` / `stx_error` diagnostics. See §7. |
| **M2** ✅ | Guest terms: `stx_describe`, template locations for `syntax_rule` / guest macros (FL-11), display names, neutral ADT wording, file names instead of `<input>`, new text format (`file:L:C: error:` + `note:`). MLite's macros describe their nodes. | B1–B3, B5–B6 fixed. No MLite error case in `tests/run_tests.sh` mentions `__wb_`, `match`, `if condition`, or a `<macro:...>` prefix. Each changed expectation listed in §7. **Done:** every diagnostic rendered from its fields (`ScopedMainFile`, `ScopedDefaultFile`); template nodes (`synthetic`) located at the call, provenance chain (`outer_origins`) as notes; value roles, operator names, `stx_describe`, display names, same-place merging in `report_analysis_errors`; `constructor` / `pattern` wording; MLite `not` / `and` / `or` describe their operands. See §7. |
| **M3** ✅ | FL-2 warning, FL-4 `reserve`, FL-10 `const`. | B9–B10 fixed. `mlite_m2_val_is_assignable_fl10` flips to an error test. MLite reserves its keywords, with tests for `val if = 1` and `fun then() = 0`. Minilang unchanged. **Done:** `core(..., const)` + resolver check; priority warnings in `GrammarDef::report_priority_warnings`; `reserve` with a DParser speculative-code callback and an unreserved re-parse to blame the word. See §7. |
| **M4** ✅ | LSP: binder hover, constructor definition, display names in hover, hover ranges, debounce, forked worker with timeout. | B7–B8 fixed. LSP tests cover a macro that loops forever and one that trips an assertion; both give one diagnostic and the server keeps answering. A multi-line hover range is tested. **Done:** per-document index (`DocIndex`) serialized by a forked worker; binder Syms recorded in lowering; `AdtVariant` declaration positions; display names in hover; hover ranges; timeout / crash diagnostics; cancellation and coalescing of edits. See §7. |

Recommended order: M0 → M1 → M2 → M3 → M4. M3 is independent of M1/M2 and
can move earlier if needed. M4's isolation can start once M0's document
index exists.

### 3.1 Test policy

- M0 changes no expectation.
- Every later expectation change is listed in §7 as before → after, with
  its test name.
- Each general change has a minilang-only test; MLite tests are the
  acceptance corpus.
- `drop_gc_cycle_warnings` (Step 11 FL-16) stays until FL-16 is fixed. If
  the M0 sink makes it easy, FL-16's Boehm warning gets routed through a GC
  warning proc instead, and the filter is removed.

---

## 4. Decisions Taken (from the Step 12 review of open questions)

| Question | Decision |
|---|---|
| Plan shape | This document, with milestones and exit criteria (like Step 11). |
| Structured IFA errors | Use the existing `report_analysis_errors` hook and `show_violation_output`; no `ifa/` change expected. Any `ifa/` change goes in its own commit, with an `ifa/`-only test, in upstreamable form. |
| Guest customisation | `stx_describe` descriptions first (§2.3); a `diagnostic` declaration only if needed after M2. |
| Pinned error text | Expected to change from M1 on; each change recorded as before/after in §7. |
| `<input>` | Replaced by the program's path for files; kept for `-e`, the REPL, and embedder strings. |
| Immutability (FL-10) | `const` flag on binding core forms; minilang `def` stays mutable. |
| LSP isolation | Forked worker with a timeout returning a serialized document index, plus debounce. |
| FL-15 / FL-18 | Moved to a separate Step 12.5 (module semantics). |

## 5. Open Questions

- **Stable diagnostic codes.** Is the `code` field (`type.primitive-arg`, ...)
  part of the public interface (for tests and editors), or internal? This
  proposal treats it as internal until M2, then documents the set.
- **Warnings policy.** Should `-Werror`-style promotion exist (e.g. for the
  FL-2 warning in CI)? Not planned.
- **`fork` vs. spawn.** Forking a single-threaded parent (no marker threads,
  no LLVM JIT, and `GC_atfork_prepare` / `parent` / `child` around `fork`) is
  safe unless external environment settings such as `GC_MARKERS > 1` force
  marker threads at startup. Spawning `langwb --lsp-worker` (`posix_spawn`)
  would not depend on that invariant, at the cost of process start-up and
  re-reading the document over a pipe. Start with `fork`, and switch if the
  invariant proves hard to keep.
- **Worker cost.** Forking per evaluation re-runs the whole document each
  time. Caching the loaded `#lang` in a pre-forked zygote may be needed for
  large `.lang` files. Measure in M4 before optimizing.

---

## 6. Acceptance Criteria for Step 12

1. **One diagnostic path.** No diagnostic is written with `fprintf(stderr)`
   outside the renderers, and the LSP does not parse diagnostic text.
2. **MLite in its own terms.** Every MLite error case in `tests/run_tests.sh`
   gives a position in the user's file, a message without internal names
   (`__wb_`, `__m__`, `match`, `if condition`), and macro provenance as
   notes.
3. **Baseline closed.** B1–B10 (§1.1) are each fixed, or explicitly
   re-targeted with a reason in §7.
4. **LSP robustness.** A looping or crashing `.lang` cannot hang or kill
   `langwb --lsp`. Hover and definition work on binders and constructors.
5. **Regression discipline.** Every general change has a minilang-only test.
   Every changed expectation is in §7.

**Status (end of M4).** 1: met (the remaining `fprintf(stderr)` are the
command-line status lines, the DEBUG monotonicity abort, the AOT runtime's
traps, and the LSP's DEBUG fork check; M2 notes). 2: met (no MLite error
expectation names `__wb_`, `__m__`, `match`, `if condition`, or `<macro:`;
an error inside a library module notes the program's call). 3: B1–B10 fixed.
4: met (`lsp_worker_*`, `lsp_newer_version_*`, `lsp_hover_binder_*`,
`mlite_m4_lsp_*`). 5: met. Left for later: IFA wording that is still IFA's
(`unresolved member ... of class`, `expression has no type`, `illegal call
argument type`); the worker's cold start (M4 notes); a `diagnostic`
declaration (§2.3.5, not needed by MLite).

---

## 7. Change Log (filled in per milestone)

| Milestone | Test | Before | After |
|---|---|---|---|
| M0 | (none changed) | All pinned command-line text | Byte-identical: every diagnostic still renders its pre-Step-12 text. |
| M0 | `grammar_undefined_nonterminal_located` (new) | `fail: unresolved identifier: 'undefined_nt'`, then `exit(1)` | `langwb: 2:41: grammar rule for 'zz_e' refers to undefined non-terminal 'undefined_nt'`; evaluation fails normally |
| M0 | `grammar_undefined_nonterminal_in_lang_located` (new) | same exit | `langwb: <path>/bad.lang:6:14: grammar rule for 'zz_e' refers to undefined non-terminal 'missing_thing'` |
| M0 | `grammar_empty_literal_reported` (new) | `fail: empty string after unescape ''`, then `exit(1)` | `langwb: grammar error: empty string after unescape ''` |
| M0 | `lsp_survives_grammar_error`, `repl_survives_grammar_errors` (new) | the LSP server / REPL process exited | located diagnostic; the process keeps answering |

| M1 | `type_error_cascade_reports_root_only` (new) | `def g = fn(a) => a + "s"; def h = fn(b) => g(b) * 2; h(1) + 1`: 6 errors, 5 of them `expression has no type` | 1 error: `<input>:1:19: error: illegal primitive argument type expression illegal: string` (`called from <input>:1`) |
| M1 | `type_error_standalone_notype_kept` (new) | `def f = fn(x) => f(x); f(1) + 2`: 3 × `expression has no type` | 1 × `expression has no type` (the root, at `f(x)`) |
| M1 | `lsp_related_information_*` (new) | call chain only inside the message text; no macro provenance | `relatedInformation`: `called from here` at each call site, `in expansion of 'getb'` at the macro call |
| M1 | (no pinned text changed) | MLite baseline programs (Step 11): `fun f(a) = a + "s"; f(x)` 3 errors, `q.y + 1` on `{ x: 1 }` 3 errors | 1 error each; `not 1 == 2`, `while 1 do ... end` unchanged (1 error) |

| M2 | (format) every located diagnostic: `top_level_error_*`, `mlite_m0_*`, `review_decl_body_error_pos`, `review2_syntax_rule_error_col`, `copilot_multiline_top_level_loc`, `mlite_m2_assign_to_fun`, `mlite_m5_multi_let_not_mutually_recursive`, ... | `langwb: L:C: msg`, `langwb: <file>:L:C: msg` | `<file>:L:C: error: msg` (`<input>` for `-e`); unlocated: `langwb: error: msg` (`grammar_empty_literal_reported`) |
| M2 | `review5_main_file_error_unprefixed`, `diag_main_file_named` (new), `diag_eval_string_is_input` (new), `mlite_m5_aot_build_runs_checks` (B5) | `<input>:3:12: error: ...` for a program read from a file | `bad.mlite:3:12: error: ...` (the path given) |
| M2 | `top_level_error_in_imported_module`, `using_syntax_body_syntax_error_position` | module / file syntax errors: `langwb: 2:11: syntax error` (no file) | `<module>:2:11: error: syntax error` |
| M2 | `neg_macro_origin_trace`, `macro_template_node_at_call_site_nested` (new) (FL-11) | `langwb: 1:13: unbound identifier 'missing_var' (expanded from 'bad_ref' at 1:52)` (template-relative position) | `<input>:1:52: error: unbound identifier 'missing_var'` + `<input>:1:52: note: in expansion of 'bad_ref'` (one note per nested step) |
| M2 | `ifa_diag_macro_provenance`, `using_syntax_body_error_position`, `lsp_related_information_macro_expansion` | `<macro:getb@3:0>:1:11: error: ...`, `<macro:zz_e@4:3>` | at the call site, `note: in expansion of 'getb'` / `'zz'` (form macros named by keyword) |
| M2 | `mlite_m2_diag_nested_expansion_b6` (new) (B6) | `<macro:fast_pow@1:0>:...: 'e' illegal: string` | `<input>:2:24: error: invalid operand in '**': string` + notes `in expansion of 'fast_pow'`, `in expansion of '**'` |
| M2 | `mlite_m2_not_binds_tighter_than_eq` (B1) | `<macro:not_e@2:89>:1:89: error: illegal primitive argument type 'if condition (expected bool)' illegal: int64` | `<input>:2:0: error: operand of 'not' must be bool, got int64` + `note: in expansion of 'not'` |
| M2 | `mlite_m2_while_int_condition`, `review6_*_literal_condition_named` | `illegal primitive argument type 'while condition (expected bool)' illegal: int64` | `condition of 'while' must be bool, got int64` (`diag_template_condition_names_macro`, `diag_stx_describe_names_role`: new) |
| M2 | `mlite_m1_non_adt_scrutinee`, `guest_macro_non_adt_scrutinee_at_call_site`, `review3_adt_match_non_adt_static_error`, `review5_*` | `illegal primitive argument type 'match scrutinee (expected Option)' illegal: int64` | `value matched by 'case' must be Option, got int64` (`'match'` / `'isv'` for those constructs) |
| M2 | `mlite_m2_diag_swapped_args_b2` (new) (B2) | 3 × `'match scrutinee (expected MList)' illegal: __wb_fn_10` / `int64` | 1 × `value matched by 'case' must be MList, got function at 4:9 or int64`, noting the program's call |
| M2 | `review4_eq_*`, `review7_eq_*`, `review4_repl_survives_type_violation`, `type_error_cascade_reports_root_only`, `lsp_related_information_call_chain` | `illegal primitive argument type expression illegal: string` | `invalid operand to '+': string` (`'=='` for `==`; `list` for `wb_list`) |
| M2 | `diag_same_place_types_merged`, `diag_function_display_names`, `diag_anonymous_function_display_name` (new) | (one error per contour; `__wb_deffn_g_1`, `__wb_fn_3`) | `invalid operand to '+': string or bool` with both call sites; `function 'g'`; `function at 1:4` |
| M2 | `step10_5_adt_*`, `review3_adt_pattern_arity_mismatch`, `review4_pattern_arity_multiline_location`, `review6_bare_ctor_pattern_hint`, `stx_copy_loc_variant_error_at_user_node`, `mlite_m1_arity`, `mlite_m1_unknown_variant`, `mlite_m5_lsp_unknown_variant` (B3) | `unknown variant 'X' in match pattern`, `...; variant 'Some' has 1`, `wrong number of arguments to variant` | `unknown constructor 'X' in pattern`, `...; constructor 'Some' has 1`, `wrong number of arguments to constructor` |
| M2 | `review_match_non_exhaustive_trap`, `mlite_m1_non_exhaustive` (runtime) | `langwb: non-exhaustive match` | `langwb: no pattern matched the value` |
| M2 | `lsp_survives_grammar_error`, `mlite_m5_lsp_*`, `lsp_related_information_call_chain` | LSP message = the command-line text, location prefix included | the message alone; a diagnostic in another file keeps a `<file>:L:C:` prefix and adds that location as `relatedInformation` (`review5_lsp_module_diag_not_misplaced`, `review9_lsp_digit_module_name` unchanged) |

M2 notes:

- **Format.** Every diagnostic is rendered from its fields: `<file>:L:C: <severity>: <message>`, then one `<file>:L:C: note: <message>` line per note (`langwb: ...` without a position). `<file>` is the module or `.lang` path, else the program's path when read from a file (`ScopedMainFile`), else `<input>`. Diagnostics raised while a module or `.lang` is read and that have only a position (parser syntax errors) are attributed to that file (`ScopedDefaultFile`). IFA's own candidate lists and ambiguity alternatives are notes.
- **Template locations (FL-11).** `parse_template` marks its nodes `synthetic`; the expansion that introduces them (origin still empty, not `located` by `stx_copy_loc` or C++) gives them the call's range. A node's provenance is its macro and call site plus `outer_origins`, the chain of the call's own provenance when the call is itself a template node, so `2 ** "a"` notes `fast_pow` then `**`. Expansion notes are attached to template nodes only. LSP hover and definition skip template nodes, which now sit at their call (they sat at template offsets before, possibly over unrelated user text).
- **Roles and wording.** `report_analysis_errors` words `PRIMITIVE_ARGUMENT` violations itself: a value with a role (literal conditions, matched values) as `<role> must be <type>, got <types>`; an operand as `invalid operand to '<op>': <types>` (`in '<construct>'` for a template's operation). The role is the node's `stx_describe` description, else `<what> in '<outermost macro>'` for a template node, else `<what> of '<keyword>'`. Other kinds keep IFA's wording with display names (`__wb_deffn_f_n` → `f`, `__wb_fn_n` → `function`).
- **Merging.** Violations at one position with the same message head are one diagnostic listing the offending types and all call sites. A violation located in an imported module also notes the program's first call on its chain (IFA's `print_call_depth` often stops short of it).
- **`stx_describe(h, text)`** (new Meta-ABI call) sets a node's description, which is not inherited by its children. `stx_copy_loc` now copies only the position (not the source node's provenance).
- **§6 criterion 1.** The only `fprintf(stderr)` outside `src/diag/` are the command-line status lines in `src/cli/main.cc` (usage, `evaluation failed`, `build failed`, `cannot open`), the DEBUG-only monotonicity abort in `prim_registry.cc`, and the AOT runtime's own traps (`src/runtime/langwb_rt.c`), none of which are compile-time diagnostics.
- **Still IFA's wording:** `unresolved member 'b' of class 'record { a }'`, `expression has no type`, `illegal call argument type ...`, unbound-variable kinds. Hover still shows internal names (B7, M4).

| M3 | `mlite_m2_val_is_assignable_fl10` → `mlite_m3_val_is_immutable_fl10`, `mlite_m3_let_val_is_immutable_fl10` (B9) | `val x = 1; x := 2; x` → `2` | `<input>:2:11: error: cannot assign to 'x': it is immutable` + `<input>:2:4: note: 'x' is bound here` |
| M3 | `core_const_binding_*`, `minilang_let_stays_assignable`, `core_const_only_on_bindings` (new) | — | `const` on `define` / `let` / `letrec` only; minilang bindings stay assignable |
| M3 | `fl2_warns_rule_level_left_without_effect`, `fl2_warns_mixed_rule_level_and_operator_priority` (new) (B10) | no diagnostic | `rule_prio.lang:9:11: warning: rule-level '$left 1' on 'pipe_e : expr '|>' expr' has no effect: ...`; `op_prio.lang:9:42: warning: operator production 'pipe_op' ... do not group correctly together ...` |
| M3 | `op_priority_subproduction_*`, `op_priority_mixed_rule_level_lhs_groups_wrongly` | stdout + stderr compared | stdout only (these grammars now warn); `op_priority_uniform_*` still compare both, so the uniform form gives no warning |
| M3 | `mlite_m3_reserved_*`, `reserve_*`, `minilang_reserves_nothing` (new) (B10, FL-4) | `val if = 1; if` → `1` | `<input>:2:4: error: syntax error: 'if' is a reserved keyword`; an incomplete form (`if 1 < 2 then 3 else`) keeps plain `syntax error` |

M3 notes:

- **`const` (FL-10).** `core(define|let|letrec, ..., const)` sets `CoreRuleMeta::is_const` (in the grammar cache key). The resolver records const binders and rejects `core(assign)` to one, with a note at the binder. MLite's `val` forms (statement, `let val`, multi-binding `let`) are const; `var` and `fun` are unchanged (`fun` keeps its `cannot assign to function definition` error).
- **FL-2 warnings** are checked when a grammar is compiled, only for rules written by guest `grammar` declarations (`RuleDef::origin`), at the rule. The first warning is limited to binary-looking rules `X op X` whose `X` is not the rule's own non-terminal: `$right 0` on keyword forms (`'when' expr ... expr`) is a common, meaningful idiom and is not reported. The second fires when an operator-priority production is used between operands whose non-terminal also has rule-level binary rules, so extending minilang (whose built-in operators are rule-level) with an operator-priority operator warns: that combination is the one FL-2 found misgrouping.
- **`reserve` (FL-4)** differs from §2.4's plan in one respect: the callback rejects a reserved word matched by *any* regex terminal of the reduced rule (looking through EBNF sub-rules, which have no code of their own), not only identifier / binder slots. That covers `params` lists and field names without per-slot metadata. A rejected word can still be a string's content or a longer identifier (`whenx`). The reserved set is reached through the parse's `D_Parser::initial_globals`, so one callback serves every grammar. The callback is installed only for grammars with reserved words, and MLite's run time is unchanged. The error is located by re-parsing with nothing reserved: if that parse succeeds (for a prefix parse, past the original failure), the first reserved word it read as a regex terminal is blamed.

| M4 | `lsp_hover_binder_and_ctor_definition`, `mlite_m4_lsp_binders_ctors_display_names` (new) (B7, B8) | hover on `val scale` / `fun area` → nothing; `fold_l : __wb_deffn_fold_l_1`; definition on `Circle(2)` / a `case` pattern → nothing | `scale : int64 = 41`, `area : function 'area'`, `fold_l : function 'fold_l'`; definition → the `type` declaration (`D 2:13`) |
| M4 | `lsp_hover_multiline_range` (new) | hover had no `range`; a position on a later line of a multi-line node found nothing | `range` included (`0:8-1:4`); later lines find the node |
| M4 | `lsp_worker_timeout_and_crash_isolated`, `lsp_newer_version_cancels_evaluation` (new) | a looping macro hung `langwb --lsp`; an aborting primitive killed it | `0:0 evaluation of this document timed out after N ms` / `... crashed (signal 6)`; the server keeps answering; a newer version cancels a running evaluation |
| M4 | `type_decl_error_absolute_position` (new) | `def a = 1;\n\n  type T = A \| A; a` → `<input>:1:13: error: duplicate variant ...` | `<input>:3:15: ...` |

M4 notes:

- **Document index.** Hover and definition are answered from a per-document index built once per evaluation: hover entries (children before parents; the narrowest node containing the position wins, ties to the first) and definition targets (parents before children; ties to the last), each with its node's start and end, plus the program's constructors and their declarations. These orders and tie rules are the ones the old tree walks used, so every pinned hover / definition answer is unchanged. Nodes are matched by their range (start to end), not only their first line.
- **Binders (B8).** The lowering records the Sym each `let` / `letrec` / sequence `def` / lambda-parameter binder binds, so binders get attributes like any expression. Boxed variables (assigned and captured) are skipped: their Sym is the box.
- **Constructors (B8).** `AdtVariant` records where its `type` declaration names it. Definition on a constructor token goes there; one the expanded tree no longer holds (a `case` pattern that MLite's macro turned into a template node) is found by the word at the cursor, so it is answered for any token spelled like a constructor.
- **Worker.** The server forks one worker at a time; queued edits are coalesced before a worker starts, a newer version of the document being evaluated kills its worker, and hover / definition for a document with a pending evaluation wait for it. The crash diagnostic does not name the phase (§2.5 proposed `in <phase>`): the server only sees the exit status.
- **Cost.** A worker starts cold: an MLite document takes ~2.5 s (mostly loading `mlite.lang`, see M0's findings), where the in-process server reused its per-process `#lang` snapshot after the first evaluation. The timeout default is therefore 10 s, not 5 s. A pre-forked zygote holding loaded `#lang` snapshots (§5) is the follow-up if this matters in an editor.

Other visible M0 changes (not pinned by text):

- **LSP placement and severity.** Diagnostics are placed from their structured range instead of re-parsing the text. Messages whose text had no parseable position but whose node is known now appear at that node instead of the top of the document: `form macro 'X' failed at L:C`, `macro 'X' failed at L:C`, `macro expansion exceeded max depth`. Diagnostic ranges end at the node's end when known (one character past the start before). Warnings are published with severity 2 (everything was 1). Each diagnostic is one LSP diagnostic: a multi-line rendering whose later lines were not indented (e.g. IFA's `note: candidates are:`) was split into several before.
- **Declarations in a `.lang`** are now located in the `.lang` file. Their errors were reported with positions relative to the start of each declaration, and without a file name.
- **Ranges.** `CstNode` / `SyntaxObj` carry `end_line` / `end_col` (used by `range_of`, the LSP, and later by M4 hover ranges).
- **Not converted:** command-line status messages in `src/cli/main.cc` (usage, `langwb: evaluation failed`, `langwb: build failed`), which are not diagnostics, and the DEBUG monotonicity assertion in `prim_registry.cc`, which aborts right after printing. §6 criterion 1 is checked against this list in M2.

M1 notes:

- The LSP message of a type error is its first line; the call chain moved to `relatedInformation` (it was part of the message text).
- Not "has no type", so not deduplicated: B2's swapped list-function arguments (`filter_l(p, xs)`) still give 3 `illegal primitive argument type 'match scrutinee (expected MList)'` errors at the same place in `std/list.mlite` (one with the function value, two with `int64` flowing on to other callers). Grouping same-node violations of one kind is a wording question for M2.

Done alongside M0 (outside the plan, prompted by test run time):

- `tests/run_tests.sh` runs its checks in parallel (`LANGWB_TEST_JOBS`, default `nproc`; `LANGWB_TEST_TIMING=1` lists the slowest), 293 s → 54 s wall on 32 cores.
- Each `#lang` is loaded once per process and shared by the main program and every `#lang` module (`lang_snapshots_`), with the language's macros re-scoped per module as before; `make test` ~295 s → ~42 s overall.
- Findings for later: a 500-term `1 + 1 + ...` chain takes ~50 s in DParser (priority disambiguation in `check_path_priorities_internal`; the large-diagnostics LSP test was rewritten to avoid it); loading `mlite.lang` still costs ~0.9 s per process, almost all in Stage-0 expansion of each Phase-1 body wrapped with every `for_syntax` helper (IFA analysis and JIT of those bodies total ~0.05 s).
