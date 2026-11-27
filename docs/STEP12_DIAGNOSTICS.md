# Step 12 — Diagnostics in the Language Author's Terms

> **Goal (from [ROADMAP.md](ROADMAP.md)):** a language built in `.lang` should
> report errors, and answer LSP queries, in *its own* terms: its keywords, its
> forms, its source positions. It should not leak minilang, IF1, or IFA. Step 11
> ([STEP11_DOGFOOD_LANG.md](STEP11_DOGFOOD_LANG.md)) built MLite and recorded
> where today's diagnostics fall short. Its friction log and the error texts
> pinned verbatim in `tests/run_tests.sh` are this step's baseline.
>
> **Status: proposal.**

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
4. **No new IFA policy in `ifa/`.** IFA already exposes structured violations
   (§1). If an `ifa/` change turns out to be needed, it is small and
   self-contained, gets its own commit with an `ifa/`-only test, and is written
   so it can go upstream.

---

## 1. Where Diagnostics Come From Today

| Source | Mechanism | Volume |
|---|---|---|
| Workbench, parser, expander, bridge, CLI | `std::fprintf(stderr, ...)` with hand-formatted `L:C:` / `<file>:L:C:` / `<macro:NAME@L:C>:` prefixes | ~115 call sites (`workbench.cc` 48, `bridge.cc` 31, `cli/main.cc` 16, `expander.cc` 8, `contour_clone.cc` 6, `meta_abi.cc` 4, `parse.cc` 2, `prim_registry.cc` 1) |
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
- **DParser can reserve words** for chosen non-terminals
  (`${declare tokenize nonterminal...}`, `dparser/docs/reference/terminals.md`),
  which gives FL-4 a mechanism.

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

- A `DiagnosticSink` lives on the `Workbench` (shared by clones, like
  `stats_`). Every current `fprintf(stderr, ...)` diagnostic site emits into
  it. Non-diagnostic output (`--dump-*`, REPL banners) stays as it is.
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
- **Ranges.** `SyntaxObj` gains `end_line` / `end_col`, taken from the CST
  node's end position in `from_cst` and kept through `clone` /
  `offset_syntax_loc` / `stx_copy_loc`. This fixes multi-line spans and lets
  the LSP measure nodes properly instead of using `text.size()`.

### 2.2 IFA violations as structured data

- `WbAST` gains `const syntax::SyntaxObj *node` (set by `make_wb_ast` /
  `attach_loc`). This is a langwb-side change only.
- `WbCallbacks::report_analysis_errors` converts each `ATypeViolation` into a
  `Diagnostic`. It uses the violation's `AVar` → `Var` → `Sym` / defining
  `Code` → `WbAST::node` to find the guest node, and the `send` / `pnode`
  chain for `called from` notes. langwb sets `fa->show_violation_output = false`.
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
   `stx_describe(h, "operand of 'not'")` stores a string on a node. The
   description is inherited by nodes a macro builds from it, and the message
   builder uses it in place of the IF1 label: `'not' operand must be a bool,
   got int64` instead of B1's `'if condition (expected bool)'`. MLite's
   `and` / `or` / `not` / `for` / `case` / `|>` macros set descriptions on
   the nodes they generate. Declarative `syntax_rule` templates get one
   automatically (`in expansion of 'not_e'`) unless they set their own.
2. **Locations of template nodes (FL-11, B1, B6).** When a `syntax_rule` or
   guest macro expands:
   - template-introduced nodes get the call site's range;
   - spliced pattern variables keep their own range;
   - the macro call becomes a `note` ("in expansion of `not` at 2:0")
     instead of a `<macro:...@L:C>:` prefix.

   This is what FL-7's `stx_copy_loc` did by hand, now done by default.
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
  inside `grammar { ... }`, lowered to DParser's
  `${declare tokenize ...}` for the language's identifier positions. A
  reserved word used as a name becomes a syntax error that names the
  keyword. Minilang reserves nothing, so behaviour is unchanged.
- **FL-10.** A `const` flag on binding core forms
  (`core(define, bind=1, rhs=3, const)`, `core(let, ..., const)`), enforced
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
- **Debounce.** `didChange` notifications are coalesced. The server
  evaluates a document only when no newer change for it is already queued,
  and it answers hover/definition from the last completed evaluation.
- **Isolation.** Each document evaluation runs in a forked worker with a
  wall-clock timeout (default 5 s, configurable). The worker sends back a
  serialized *document index*: diagnostics plus a per-range table of hover
  texts and definition targets. The server never holds the worker's
  `SyntaxObj` tree. A worker that crashes or times out produces one
  diagnostic at the top of the document ("evaluation of this document
  crashed / timed out in `<phase>`"), and the server keeps running. This
  matters most while writing a `.lang`, which is also Step 13's use case.

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
| **M0** | `Diagnostic`, `DiagnosticSink`, text and LSP renderers; every diagnostic site emits into the sink; `SyntaxObj` end positions; `CaptureStderr` / `parse_diag_loc` removed. | All tests pass **unchanged** (text output byte-identical); the LSP builds diagnostics without parsing text; a `Diagnostic`-level unit test covers each renderer. |
| **M1** | IFA violations through `report_analysis_errors`; `WbAST::node`; `NOTYPE` deduplication; `called from` and macro provenance as notes. | B4 gone, with no other IFA message lost (every `expect_error` still matches its key text). Before/after table in §7. |
| **M2** | Guest terms: `stx_describe`, template locations for `syntax_rule` / guest macros (FL-11), display names, neutral ADT wording, file names instead of `<input>`, new text format (`file:L:C: error:` + `note:`). MLite's macros describe their nodes. | B1–B3, B5–B6 fixed. No MLite error case in `tests/run_tests.sh` mentions `__wb_`, `match`, `if condition`, or a `<macro:...>` prefix. Each changed expectation listed in §7. |
| **M3** | FL-2 warning, FL-4 `reserve`, FL-10 `const`. | B9–B10 fixed. `mlite_m2_val_is_assignable_fl10` flips to an error test. MLite reserves its keywords, with tests for `val if = 1` and `fun then() = 0`. Minilang unchanged. |
| **M4** | LSP: binder hover, constructor definition, display names in hover, hover ranges, debounce, forked worker with timeout. | B7–B8 fixed. LSP tests cover a macro that loops forever and one that trips an assertion; both give one diagnostic and the server keeps answering. A multi-line hover range is tested. |

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

---

## 7. Change Log (filled in per milestone)

| Milestone | Test | Before | After |
|---|---|---|---|
| — | — | — | — |
