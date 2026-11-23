# langwb review — 2026-11-22, `ce2fa9b..bb6f4eb` + working tree

This review covers everything since `ce2fa9b`. That includes the four commits
on `main` and the uncommitted changes in the working tree:

| change | subject |
|---|---|
| `4595a57` | Address 2026-11-22 review (1f88a19..ce2fa9b): closures, eval_value, linear Amb, grammar-driven decls, match/quasiquote |
| `1c7a7ee` | Add the previous review |
| `238e924` | Step 8: AOT compiler driver (`langwb build`) and Language Server (`langwb --lsp`) |
| `c3ca4d0` / `bb6f4eb` | `ifa/` subtree pull `0fd69e4..8788b89` (explorer, exported functions, inliner fixes) |
| working tree | Native `.g`-style `grammar` declarations, unquoted `syntax_rule` templates, eager `syntax_ext` deltas, `src/runtime/langwb_rt.c` + `liblangwb_rt.a`, `execvp` for clang, LSP JSON parser |

The build has no warnings. `make test` passes: the smoke tests, all
`examples/minilang/*.ml`, and 121 cases in `tests/run_tests.sh`, in about 8 s.
Every finding below was reproduced against the current working-tree binary,
using the `-e` expression, file or LSP script quoted with it.

The top three:

1. **The LSP server deadlocks when a document produces more than about 64 KB
   of diagnostics (§2.4a).** `CaptureStderr` redirects `fd 2` into a pipe and
   reads it only after evaluation finishes, so stderr writes block forever once
   the pipe is full.
2. **Error positions inside declaration bodies are relative to the body
   (§2.2).** `macro m => 1 + * 3;` reports `1:4`, not `1:15`. A new test locks
   in the wrong column, and the LSP puts the squiggle on the `macro` keyword.
3. **The new `grammar` syntax has two silent footguns (§2.3).** The old quoted
   form `grammar expr += "foo";` still parses, but now means "the regex `foo`".
   Inside a `syntax_ext`, a `:` (replace) rule on an existing production is
   dropped without any message.

---

## 1. What the range delivered

Most of the previous review was addressed, and the fixes are correct:

| prior finding | status |
|---|---|
| §2.1a closure called from a closure or a record segfaults | **Fixed.** `g(3)` → `4`; `ops.t(1)` → `2` |
| §2.1b mutually recursive capturing `def`s | **Fixed.** A shared `group_caps` per `Seq`, lowered before siblings are rebound → `20` |
| §2.1c self-call from a nested lambda | **Fixed.** `rec_self_caps` now stores names and scopes, and is resolved at each call site → `7` |
| §2.1 `ifa/` edits not upstreamed, `i1` sign-extended, unsized fall-through | **Fixed.** `ZExt` for `i1`, `codegen_fail` when the struct is unsized, upstreamed in `56095e2` and pulled back in `c3ca4d0` |
| §2.2 `5 == 5` → `0`, lists print addresses | **Fixed** for `bool`, flat `int` lists and records (`Kind::Bool`/`List`/`Other`); see §2.5 for the rest |
| §2.3 registration failures counted as success | **Fixed.** Functions now return `bool` and the result is propagated |
| §2.3 error points at `;`, not at `*` | **Partly fixed:** see §2.2 |
| §2.4a Tier-3 exponential search with a depth-8 cap | **Fixed** for independent sites: 12 sites resolve, 30 sites take 1.2 s; see §2.6 for nested sites |
| §2.4b Tier 2 speculatively mutates the grammar | **Mitigated.** `GrammarDef` snapshot and rollback per alternative |
| §2.4c duplicate derivations become false ambiguities | **Fixed.** Alpha-equivalence signature dedup |
| §2.4d diagnostics show hygiene names | **Fixed.** `render_surface` |
| §2.5 `rewrite_rhs_nterms` rewrites inside `[...]` | **Fixed** |
| §2.6 non-exhaustive `match` → `0` | **Fixed.** `wb_match_fail()` traps; this is safe because the JIT runs in a forked child, so the REPL survives |
| §2.6 quasiquote breaks on string literals | **Fixed.** The string rule and lowering now handle `\"`, `\n` and `\t` |
| §2.7 `check` reserved globally | **Fixed.** Declaration detection uses `decl_grammar_` `parse_prefix` |
| §2.7 `check nosuch => …` silently accepted | **Still open.** `check nosuch => 0; 5` → `5` with no warning |
| §2.6 record patterns are static, `head([])` → `0` | **Open but documented** in the README |

Step 8 is in good shape for a first cut:

- `langwb build` reuses `eval_impl` and the staged pipeline. `--emit-llvm`
  works, and binaries print `int`, `bool`, `string`, flat-list, record and
  `<fn>` results the same way the JIT does. A non-exhaustive `match` exits
  with status 1. The working tree replaces the `std::system` command string
  with `fork`/`execvp`, which has a test for an output path containing a
  space. It also moves the runtime into a real `src/runtime/langwb_rt.c`
  archive instead of a C string written to a temp file. Both are the right
  changes.
- `langwb --lsp` handshakes correctly. Hover on `abc` in `def abc = 41;\nabc + 1`
  gives `abc : int64 = 41`, and go-to-definition lands on the binder. The
  `<input>:L:C` IFA diagnostics map to the right line and column. The working
  tree replaces the ad-hoc key search with a real JSON parser, with a test
  for document text containing `"method"` and `"id"` keys.
- The working-tree `grammar` syntax (`expr += a core(passthrough) | b;`,
  `foo : 'x' expr;`, `grammar { … }`) reads much better than the
  quote-inside-quote form, and all the tests were migrated to it.
- Unquoted `syntax_rule` templates (`=> let t = $x in t + t;`) are checked
  eagerly against the guest grammar. `syntax_ext` bodies are now parsed once,
  at declaration, into a `SyntaxExtDelta`. They are no longer re-parsed as
  text on every `using syntax`.

---

## 2. Findings

### 2.1 Uncommitted work

`src/runtime/langwb_rt.c` is untracked, but the `Makefile` in the working tree
needs it to build `liblangwb_rt.a`. Committing the `Makefile` without it
breaks the build. `ifa/common/explorer/*.inc` are generated by
`ifa/Makefile:246` and should be in `.gitignore`. The root `.gitignore` covers
`*.o` and `*.a` but not these files.

### 2.2 Declaration-body error positions are relative to the body

```
$ ./langwb -e 'macro m => 1 + * 3; 5'
langwb: 1:4: syntax error            # '*' is at column 15
$ ./langwb -e 'syntax_rule form foo(_ $x:expr) => ($x + ); 5'
langwb: 1:16: syntax error           # column in the rewritten "(__sr_chk_0__ + )" text
```

The re-parse added in `4595a57` (`workbench.cc`, `parse_and_apply_decl_prefix`)
calls `guest_gram->parse(body_sv.substr(0, semi), …)` without the body's line
and column offset. The new `syntax_rule` head path has the same problem, and
also reports columns in the `__sr_chk_N__`-substituted string, not in the
source. The test `review_decl_body_error_pos` expects `"1:4: syntax error"`,
so it codifies the bug. Pass the body's base line and column into `parse()`,
as `eval_single_expr` already does with `line_offset`/`col_offset`. Map
substituted columns back, or substitute metavariables with same-length
placeholders. Then change the test to expect `1:15`.

Each such error also prints a second line,
`1:0: invalid declaration near 'macro m => …'`. That line quotes up to 60
characters of raw source, including newlines (see §2.4b).

### 2.3 Native `grammar` declarations (working tree)

**(a) The old quoted syntax is silently reinterpreted.** `str` is a valid
`gram_atom`, so `grammar expr += "when_x";` still parses. It now means "a
regex terminal matching `when_x`", not the non-terminal `when_x`. The failure
appears later, at a use site, as a bare `syntax error`:

```
$ ./langwb -e 'grammar expr += "when_x" core(passthrough); grammar when_x += "'\''wx'\'' expr"; …; wx 41'
langwb: 1:3: syntax error
```

Every existing user `.lang` file breaks this way. Reject a `gram_rhs` that is a
single `str` whose contents contain a quote or an identifier that names a
production, or at least warn. Also add a short migration note to the README.

**(b) `prod : …` clears the whole production, including core rules.**
`grammar expr : 'zz'; 1` → `syntax error`, because every `expr` rule, `+`,
literals and the rest are gone. That matches `.g` semantics for a new
production, but for an existing core production it is almost certainly a
mistake. Refuse `:` on a production that already had rules from the base
grammar or a `#lang`, or require an explicit `grammar expr := …` to replace.

**(c) `syntax_ext` deltas lose `:` replacements.**
`apply_decl_cst` (`decl_syntax_ext`) computes the delta as "rules past the
base count, per production". A `:` inside the extension clears the
production first, so the rule count can drop to or below the base count, and
the delta comes out empty:

```
$ ./langwb -e 'syntax_ext e { grammar expr : '\''zz'\'' core(datum); }; using syntax e in { zz }'
langwb: 1:0: unbound identifier 'zz'
```

The same applies to anything that uses `prepend_rule`. Either record the
extension's declarations as a list of operations (`add`, `clear`, `prepend`)
and replay them, or diff whole productions instead of counting rules.

**(d) `SyntaxExtDelta` captures only the grammar and `stage0_` macros and
hooks.** `stage1_macro`, `check`, `prim … transfer` and `for_syntax`
declarations inside a `syntax_ext` are applied to the temporary `ext_wb` and
then dropped. Previously they were re-applied on every `using syntax`. Carry
them in the delta, or reject them inside `syntax_ext` with a clear error.

**(e)** `gram_alt` uses `$right 1`/`$right 2` priorities and then
post-filters `Amb` nodes by "has `core_body`". That works, but it means the
declaration grammar itself is ambiguous. A rule like
`gram_alt: gram_rhs core_ann?` with `core_ann` excluded from `gram_elem` would
remove the ambiguity.

### 2.4 Language server (`src/workbench/lsp.cc`)

**(a) Deadlock on large diagnostics.** `CaptureStderr` points `fd 2` at a
pipe. Only the read end is non-blocking, and the pipe is drained only in
`finish()`. Once more than the pipe buffer (64 KB on macOS) is written, the
next `fprintf(stderr, …)` blocks forever:

```
# 700 unresolved-member errors ≈ 113 KB of stderr (`-e` finishes in 12 s)
$ python3 lsp.py "$(python3 -c "print('let x = { a: 1 } in ' + ' + '.join(['x.b%d'%i for i in range(700)]))")" | ./langwb --lsp
# still running after 40 s; `sample` shows the main thread in __swrite
```

Drain the pipe from a thread while evaluating, or better, route diagnostics
through `EvalOptions::diag_fp` (`open_memstream`) and stop redirecting the
process-wide `fd 2`. The second option also fixes the thread-safety concern
from the previous review about `QuietStderr`.

**(b) Diagnostics are parsed from text, line by line.**
`format_diagnostics_json` treats each stderr line as one diagnostic and takes
the first two `:`-separated integers as its position. This goes wrong in
several ways:

- A message that contains a newline becomes extra diagnostics. For
  `macro m => 1 + * 3;\n5`, the `invalid declaration near '…'` message turns
  into a second error whose whole text is `5'`, at line 0.
- `<macro:op_inc@1:0>:1:5: …` parses `atoi("op_inc@…")` as `0`, so every
  diagnostic from macro-expanded code goes to line 0, column 0.
- Non-error lines, such as `alternative 1: …` under an ambiguity error and the
  trailing `langwb: evaluation failed`, become separate severity-1 errors.

Have the workbench collect a structured `std::vector<Diagnostic>` with file,
line, column, severity and message, and use the macro provenance as
`relatedInformation`. Until then, at least escape newlines in messages and
fold continuation lines into the previous diagnostic.

**(c) Smaller issues:**

- Every `didChange` runs the full staged pipeline with no debounce, and guest
  macros are JIT-run on the server's thread. A looping compile-time macro
  hangs the server.
- `find_annotated_at` and `find_node_at` assume a node lies on one line
  (`node->line == line`, end column = `col + text.size()`). Hover misses
  multi-line forms, and nodes created by macro templates carry template
  positions.
- Columns are byte offsets, but LSP defaults to UTF-16 code units. Advertise
  `positionEncoding: "utf-8"` in the `initialize` response.
- `\u` escapes with surrogate pairs are decoded as two 3-byte sequences.

### 2.5 `eval_value` and lists

The flat cases are fixed, but the list representation is still an untyped
`i64`. `head` is typed `int64` no matter what the elements are, and
`_CG_wb_emit_list` prints every element with `%lld`:

```
$ ./langwb -e '["a", "b"]'        # [4380557336, 4380557320]
$ ./langwb -e '[[1,2],[3]]'       # [4439906368, 4439906336]
$ ./langwb -e 'head(["a"])'       # 4377624584
```

`head(["a"]) + 1` also type-checks. Either make `wb_list` carry an element
type, which needs a creation-set-based list in IFA, or reject non-`int64`
element types in `list_lit` until it does. Also update the README: it now
says lists print as `[1, 2, 3]` without the int-only qualifier.

`_CG_wb_emit_str`, in both the JIT version and `langwb_rt.c`, re-escapes `"`
and `\` but not newline. `"a\nb"` therefore prints a raw newline inside the
quotes, so the output is not re-readable. There is also no `true`/`false`
literal: bools print, but `[true]` is `unbound identifier 'true'`.

### 2.6 Tier-3 resolution of nested `Amb` nodes

`collect_all_ambs` stops at the outermost `Amb`, and each outer alternative is
probed with any inner `Amb` still in place. `analyze_syntax` on a tree that
contains an `Amb` always fails, so nested ambiguity can never resolve. The old
recursive resolver handled this case by recursing into each candidate.

```
# at_rec: '@' e → e.x ; at_int: '@' e → (e + 10)
$ ./langwb -e '… let p = { x: { x: 1 } } in (@(@p))'
error: unresolved syntax ambiguity for '@(@p)' (0 of 2 alternatives valid):
  alternative 1: paren `(((p+10)p.x)+10)`
  alternative 2: field `((p+10)).x`
```

The expected result is `1` (`at_rec ∘ at_rec`). The summaries are also
garbled, because `render_surface` concatenates both children of the inner
`Amb`. Fixes:

- Resolve innermost-first, either by collecting the leaves of the `Amb` tree
  or by recursing into each alternative before probing it.
- Make `render_surface` render only the first child of an `Amb`, marked
  `…|…`.

The other ambiguity sites are replaced with a `0` datum during probing
(`make_neutral_datum`). That biases toward `int`-typed contexts, and because
resolution is greedy within a round, the result can depend on the order of
the sites. Say this in a comment. Also add a test where the neutral `0`
makes the only valid alternative fail, so the limitation is visible.

The Tier-2 rollback restores only `GrammarDef`. Losing alternatives still run
guest macros, JIT-compile them, and can register prims or modify
`stats_`. That is acceptable, but ROADMAP 5.1's "never run guest macros
speculatively" is still not true. Reword it.

### 2.7 AOT driver

- The runtime now exists twice: the `extern "C" _CG_wb_*` definitions in
  `workbench.cc`/`bridge.cc` for the JIT, and `src/runtime/langwb_rt.c` for
  AOT. They already differ: the JIT versions write into the result buffer,
  the AOT versions to `stdout`. Build `langwb_rt.c` into the `langwb` binary
  too, and keep only the emit sink separate, so new prims cannot drift
  between the two.
- `LANGWB_RT_LIB` is an absolute path baked in at compile time, and
  `-L/opt/homebrew/lib -L/usr/local/lib` are hard-coded. Look up the archive
  relative to the executable (`/proc/self/exe` or `_NSGetExecutablePath`)
  and take library flags from `pkg-config bdw-gc` or the Makefile.
- With `-o foo`, the temporary `foo.tmp.ll` is written next to the output.
  If two builds run in parallel with the same `-o`, they overwrite each
  other's temp file. Use `mkstemps` in `$TMPDIR`.
- The default output path is `a.out` in the current directory. That is fine,
  but print it, since `langwb build x.ml` gives no other indication of where
  the binary went.

### 2.8 Minor

- In `tests/run_tests.sh`, `copilot_invalid_binop_role` still uses
  `$binary_op_left 3` (the previous review's §2.8). `grammar_def.h` was
  updated, but the test was not.
- In `collect_free_captures`, the new transitive capture for `rec_self_caps`
  callees silently drops a capture when `outer_env.resolve` fails, and so
  does the call-site loop in `lower_expr`. Either one produces an arity
  mismatch that IFA reports far from the cause. Turn both into hard errors
  with the callee's name.
- `group_caps` is the union of the captures of every `def` in a block, so a
  `def` that captures nothing still receives every sibling's captures. This
  is correct but wasteful, and it prevents those functions from being
  emitted as plain functions. Computing a fixpoint over the call graph within
  the group would capture only what is needed.
- An IFA error for a field that is not in a record still names `__wb_rec_1`
  (the previous review's §2.7), and the error now also reaches LSP users.
- The `ifa/` pull adds 2,800 lines to `ifa/common/html.cc` and 3,700 lines of
  `explorer.js`. These were reviewed upstream (PR #6) and are not examined
  here, apart from the `.gitignore` gap in §2.1.

---

## 3. Recommended order

1. Commit `src/runtime/langwb_rt.c` with the `Makefile` change, and ignore the
   generated `.inc` files (§2.1).
2. Fix the LSP stderr deadlock by moving diagnostics to `diag_fp` or a
   structured list. That also fixes the line-0 and split-message problems
   (§2.4a–b).
3. Pass source offsets into declaration-body and `syntax_rule` re-parses, and
   fix the `review_decl_body_error_pos` expectation (§2.2).
4. Reject or warn on the legacy quoted `grammar` form. Guard `:` on core
   productions, and make `syntax_ext` deltas replay operations (§2.3a–d).
5. Resolve nested `Amb` nodes innermost-first (§2.6).
6. Type list elements, or restrict lists to `int64` (§2.5).

---

## 4. Reproductions

```
./langwb -e 'macro m => 1 + * 3; 5'                                         # §2.2 → 1:4 (should be 1:15)
./langwb -e 'syntax_rule form foo(_ $x:expr) => ($x + ); 5'                  # §2.2 → 1:16 in rewritten text
./langwb -e 'grammar expr : '\''zz'\''; 1'                                    # §2.3b → syntax error
./langwb -e 'syntax_ext e { grammar expr : '\''zz'\'' core(datum); }; using syntax e in { zz }'   # §2.3c
./langwb -e '["a", "b"]'                                                    # §2.5 → addresses
./langwb -e '[[1,2],[3]]'                                                   # §2.5 → addresses
./langwb -e '"a\nb"'                                                        # §2.5 raw newline
./langwb -e 'check nosuch => 0; 5'                                          # §1 → 5, no warning
# §2.6, with the at_rec/at_int grammar from tests `review_tier3_linear_12_ambs`:
#   let p = { x: { x: 1 } } in (@(@p))      → 0 of 2 alternatives valid
# §2.4a: open a document producing >64 KB of diagnostics in `langwb --lsp` → hang
```
