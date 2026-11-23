# langwb review — 2026-11-23, uncommitted changes on `9d9fc47`

This review covers the uncommitted working tree on top of `9d9fc47`: 23
files, +1010/−221. The changes respond to the previous review
(`2026-11-23-review-bb6f4eb..9d9fc47.md`).

The build has no warnings. `make test` passes: the smoke tests, all
`examples/minilang/*.ml`, and 191 cases in `tests/run_tests.sh` (26 of them
are new `review3_*` cases). Every finding below was reproduced against the
working tree with the quoted `-e` expression or files.

The top four:

1. **The two new IFA `check` hooks call `fail()`, which calls `exit(1)`
   (§2.1a).** `{x:1} == {x:1}`, `None == None`, or a constructor pattern
   matched against a non-ADT value ends the whole process. In `--repl` the
   session ends. In `--lsp`, opening a document that contains one of these
   kills the server without a diagnostic.
2. **ADT values that are unions print `<union>` (§2.2a).**
   `if c then Some(1) else None` is the common case for an ADT, and both the
   result printer and `println` give `<union>`. A record field holding a
   closure prints `<uninit>`.
3. **The REPL newline fix does nothing (§2.3).** `print` runs in the forked
   JIT child, so `g_wb_stdout_dirty` is set in the child and the parent never
   sees it. `print(1)` in the REPL still shows `1=> 0`. The `-e` and file
   paths were not changed (`{ print("a"); 5 }` → `a5`).
4. **Module ADTs are merged by type name (§2.4a).** If the importer, or an
   earlier module, already has a type with the same name, the module's
   type is skipped without a message, and the module's own constructors
   become `unbound identifier`.

---

## 1. Status of the previous review

| prior finding | status |
|---|---|
| §2.1a assignment to a captured variable | **Fixed** by boxing variables that are both assigned and captured. Correct for `let`, `def`, lambda parameters, nested closures, and recursive defs that capture (`f(3)` → `4`) |
| §2.1b `SetField` object captures everything to its left | **Fixed** with a `field_lhs` non-terminal (ident, paren, field chain, call) |
| §2.1c assigning to a `def` fn / `<fn>` for a union | **Fixed.** `def` fns are rejected. A `let` fn that is assigned is no longer rebound |
| §2.1d `while 0` message, `print` sharing a line with the result | **Open.** The message is unchanged. The newline fix does not work (§2.3) |
| §2.2a duplicate constructors and fields, REPL re-registration | **Fixed.** `register_adt` replaces by type name |
| §2.2b ADTs print as records | **Fixed for single variants.** Unions print `<union>` (§2.2a) |
| §2.2c pattern arity and non-ADT scrutinee | **Fixed, but** the arity error has the wrong location (§2.5) and the non-ADT check calls `fail()` (§2.1a) |
| §2.2d `__wb_*` helpers shadow user bindings | **Fixed.** `env.resolve` is checked first |
| §2.2e `cg_isinstance_tag_targets` `id` loop | **Removed**, and replaced by `m == cls_type \|\| fam == cls_type`. Still an unupstreamed `ifa/` edit (§2.6) |
| §2.3a `print` of a non-string pointer | **Fixed.** `emit_llvm_wb_value` dispatches on the IFA type and is shared with the result printer |
| §2.3b `==` on strings | **Fixed** with the `wb_eq`/`wb_ne` prims. New problems in §2.1a and §2.2b |
| §2.3c tags not checked | **Fixed.** The `head*` accessors trap on a mismatch, and `cons` tags by IFA type |
| §2.4a imports resolved against the CWD | **Fixed** for the CLI. In the LSP, URIs are not percent-decoded (§2.4b) |
| §2.4b module `type`s lost | **Fixed when the type names differ** (§2.4a) |
| §2.4c exported macros not hygienic | **Fixed** with `flip_scope` in `copy_macro_from`. `let secret = 100 in addsec(secret)` → `141`, which is correct |
| §2.4d import cycles | **Fixed.** `import cycle: c1.ml -> c2.ml -> c1.ml` |
| §2.4e module errors have no file name | **Open.** `2:19: unbound identifier 'nope'` does not say which file |
| §2.4f ambiguous imports, `export nosuch` | **Fixed** (warnings) |
| §2.5 `check core` names, `check call`, `seen_ids` | **Fixed.** There is one list (`is_core_form_name`), `CheckKind` is stored with each check, and only identifiers and binders are compared |
| ROADMAP "int64-only list enforcement" | **Fixed** |
| `-L/opt/homebrew/lib` in the `Makefile` | **Open** |

Boxing is done in the right place. `resolve_node` already visits every
binder and reference, so recording `binder_depth` and the assigned and
captured sets there, then boxing only their intersection, keeps the common
case unboxed. The `field_lhs` grammar is also a better fix than a priority
tweak. Moving the result printer from IF1-level field sends to
`emit_llvm_wb_value` removes the `record_fields` special case from both the
JIT and AOT paths. AOT `build` now prints nested records and ADTs
correctly.

---

## 2. Findings

### 2.1 Error reporting from prim `check` hooks

**(a) `fail()` ends the process.** `wb_eq`/`wb_ne` (`workbench.cc`,
`reg_eq`) and `wb_check_adt` (`bridge.cc`) report errors with IFA's
`fail()`. `fail()` (`ifa/common/misc.cc:31`) prints `fail: …` and calls
`exit(1)`:

```
$ printf '{x:1} == {x:1}\n1 + 1\n' | ./langwb --repl
langwb> fail: illegal primitive argument type for '=='
                                      # session gone; `1 + 1` never runs
$ ./langwb -e 'type T = A | B; match runtime_val(0) { A => 1, B => 2 }'
fail: cannot match pattern of type 'T' against incompatible value
```

The same check runs during analysis in `inspect_document`. With `--lsp`, a
`didOpen` of `{x:1} == {x:1}` gets no `publishDiagnostics` and no later
reply. The server has exited, and its stderr went into the `CaptureStderr`
tmpfile. The `-e` messages also have no `<input>:L:C` and do not use the
usual `error:` format.

Report these through `type_violation(ATypeViolation_primitive_argument, av,
…)`, as IFA's own primitive argument checks do. That gives the normal located
`illegal primitive argument type` message, and analysis continues to report
the other errors. Add a REPL test that evaluates a bad `==` and then a good
expression, so a process exit is caught.

**(b) What `==` accepts.** `is_num_or_bool` treats `int64`, `bool` and
`wb_list` as one class:

```
$ ./langwb -e '1 == true'      # true
$ ./langwb -e '[1] == [1]'     # false: compares cons pointers
```

`1 == true` used to be a type error, and allowing it now looks unintended.
List `==` is identity equality, which users will not expect. Either compare
only same-kind operands and reject lists, or document identity semantics.
`None == None` is rejected (it is a record). Equality on nullary
constructors is the next thing users will try, and `_tag` already makes it
cheap.

### 2.2 `emit_llvm_wb_value`

**(a) Unions print `<union>`.** `Type_SUM` returns `<union>` before
anything else is tried:

```
$ ./langwb -e 'type Option = None | Some(val); if runtime_val(1) == 1 then Some(1) else None'
<union>
$ ./langwb -e 'let q = {f: fn(x) => x} in q'
{ f: <uninit> }
```

Most ADT values are this union of variants of one type, so this undoes much
of §2.2b for real programs. For a sum whose members are all `Type_RECORD`s
that carry `_tag` (or all are variants of one `AdtDef`), load `_tag` and
`switch` to a per-variant `emit_llvm_wb_value`. The variant classes already
have known field layouts. The `<uninit>` for a closure field happens because
`llvm_emit_record_field` returns null for the erased member. Check the field's
`Sym` type for `is_fun` before the load, and print `<fn>`.

**(b) Smaller issues.**

- The depth limit of 8 stops a cyclic record (`r.n = r`) from looping
  forever at compile time, but the output is eight nested levels and then
  `{ ... }`. Something like `<cycle>` or a lower limit would be easier to
  read.
- `compile_and_jit_value` classifies the result as `Kind::Record` with
  `out_str.front() == '{'`. That works today only because strings are
  quoted. Pass the kind from the IFA type of the root instead.

### 2.3 `print` and the REPL newline

`_CG_wb_print_end` sets `g_wb_stdout_dirty` in the process that runs the
JIT code. In the REPL, `llvm_jit_execute` forks (`bridge.cc:18`), so
`_CG_wb_consume_stdout_dirty()` in `run_repl` always reads 0 in the parent:

```
$ printf 'print(1)\n' | ./langwb --repl
langwb> 1=> 0
```

Send the flag back over the result pipe that the fork already uses, or have
the parent check whether the child wrote to stdout. The `-e` and file paths
(`{ print("a"); 5 }` → `a5`) do not call it at all. There is no test for
this, so add one.

`_CG_wb_print_begin`/`_end` save and restore the emit sink by setting a
global to NULL. That is correct as long as nothing nests (no `print` inside
a value printer), which is true today. Note it in a comment.

### 2.4 Imports

**(a) Module ADTs are merged by type name.** `apply_exports` skips any
`adt_defs` entry whose `name` is already in the importer's `prims_.adts()`:

```
$ cat t.ml    # type Option = None | Some(val); …
$ cat t2.ml   # type Option = Nada | Algo(v); def mk2 = fn(x) => Algo(x); export mk2;
$ ./langwb -e 'import "t.ml"; import "t2.ml"; match mk2(4) { Algo(v) => v, Nada => 0 }'
langwb: 1:49: unbound identifier 'Algo'
$ ./langwb -e 'type Option = None | Some(val); import "t2.ml"; mk2(1)'
langwb: 1:49: unbound identifier 'Algo'
```

The skip is right only for the diamond case, where the *same* module is
reached twice (it is cached by path). Key the test on the module path and
type name, or on identity. When two different modules use the same type name,
the type needs a module-qualified IFA class name (`t2::Option::Algo`). The
`variant '…' is already defined` check should also run when a module's
variants are merged, not only when a `type` is declared. Also,
`base_adt_count` captures only types appended after the copy. A module that
redefines a type it inherited from the importer replaces it in place and
is missed.

Every constructor of a module is now visible to the importer even when it
is not exported (`import "t.ml"; Some(9)` works though only `mk` is
exported). The module's own code needs the classes, but the importer should
see only exported constructor names. Either decide that types are always
public and document it, or filter `variant_map_` lookups by scope.

**(b) LSP document paths are not decoded.** `run_lsp_server` uses
`uri.substr(7)`, so `file:///…/my%20dir/main.ml` searches the literal
directory `my%20dir`, and `import "q.ml"` gives `cannot find imported
module`. Percent-decode the path (at least `%XX`). Windows drive letters
(`file:///C:/…`) will need the same work later.

**(c) Smaller issues.**

- The cycle and ambiguity messages mix file names (`c1.ml -> c2.ml`) with
  full absolute paths (`already imported from '/private/…/m1.ml'`). Use
  paths relative to the importing file, or file names, in both.
- Every early `return false` in `import_module_file` now has
  `import_stack.pop_back()` before it, 11 times in all. A small RAII guard
  would avoid missing one later. The `in_progress` entry also stays in
  `imported_cache` on failure. That is harmless now, because a failure
  aborts the eval, but not if import errors ever become recoverable.
- `flip_scope` wrapping is applied in `copy_macro_from` only. Macros that
  reach the importer through `syntax_ext_exports` or `export grammar`
  deltas are not wrapped, so they probably have the hygiene bug the previous
  review fixed for plain exports. This was not reproduced. Add a test with a
  `syntax_ext` that refers to a private `def`.

### 2.5 Pattern arity check

The error location is wrong. `cur->line/col = pat->line/col` in the `match`
expander copies the pattern's position, but the pattern's position is
relative to the macro's template, not the source:

```
$ ./langwb -e 'type Option = None | Some(val); match Some(1) {
  Some(a, b) => a }'
<input>:1:3: error: pattern 'Some' has 2 field(s); variant 'Some' has 1
```

The pattern is at `2:3`, and the same `1:3` is reported wherever the
pattern is. Use the position of the arm's ctor token. The message would
also be clearer as `pattern 'Some(a, b)' …`.

The arity is now required. `Some => 1` is rejected with `pattern 'Some' has
0 field(s); variant 'Some' has 1`. That is a reasonable ML-like rule, but it
is a behavior change. Document it, and accept `Some(_)` if `_` is not yet a
wildcard.

The arity is passed as a third `__wb_is_variant` argument and parsed back
with `strtoull` in `resolve_node`, while `lower_expr` accepts either 2 or 3
arguments. Pass it only one way. The template always passes three now.

### 2.6 `ifa/` changes

This working tree changes `ifa/` in five places:

- `clone_reset()` clears `sum_intern_table` and `cs_member_classes` between
  runs. This is a real bug fix for any front end that resets IFA, and it
  should go upstream on its own.
- `RegisteredPrim::reads_fields` and the `mark_record_fields_live`
  recursion in `dead.cc`.
- The `P_prim_setter` liveness change in `dead.cc`
  (`p->tvals.v[0]->live || …`), which affects every front end.
- `cg_isinstance_tag_targets` now has `m == cls_type || fam == cls_type`.
- `llvm_emit_record_field` in `cg_emit_llvm.cc`.

The setter and `isinstance` changes change pyc's output. As with
`56095e2`, upstream them with a pyc regression run. The other three only add
code and are lower risk.

### 2.7 Smaller issues

- `is_core_form_name` keeps its own `kAll[]` list next to `core_form_name`.
  That is better than two copies, but it can still drift from
  `CoreFormKind`. Add a sentinel enumerator (`kCount`) and iterate up to it,
  or add a `static_assert` on the array size.
- `field_lhs` is a fixed core non-terminal. A user grammar that adds a
  postfix form to `expr`, such as indexing, cannot use that form as a
  `SetField` object unless it also extends `field_lhs`. Document this with
  the core grammar.

---

## 3. Recommended order

1. Replace `fail()` in `wb_eq`/`wb_ne`/`wb_check_adt` with
   `type_violation` (§2.1a), and add a REPL and LSP survival test.
2. Print `Type_SUM` values made of ADT variants by `switch`ing on `_tag`,
   and print closure fields as `<fn>` (§2.2a).
3. Make the REPL newline work across the fork, and decide what `-e` does
   (§2.3).
4. Key module ADT merging by module, and reject variant collisions
   on import (§2.4a). Percent-decode LSP URIs (§2.4b).
5. Fix the pattern arity error location (§2.5), and tighten what `==`
   accepts (§2.1b).
6. Upstream `clone_reset` alone, then the `dead.cc` and `isinstance` changes
   with a pyc regression run (§2.6).

---

## 4. Reproductions

```
printf '{x:1} == {x:1}\n1 + 1\n' | ./langwb --repl                          # §2.1a → REPL exits
./langwb -e 'type T = A | B; match runtime_val(0) { A => 1, B => 2 }'       # §2.1a → fail:, exit 1
./langwb -e '1 == true'                                                     # §2.1b → true
./langwb -e '[1] == [1]'                                                    # §2.1b → false
./langwb -e 'type Option = None | Some(val); if runtime_val(1) == 1 then Some(1) else None'  # §2.2a → <union>
./langwb -e 'let q = {f: fn(x) => x} in q'                                  # §2.2a → <uninit>
printf 'print(1)\n' | ./langwb --repl                                       # §2.3 → 1=> 0
./langwb -e '{ print("a"); 5 }'                                             # §2.3 → a5
./langwb -e 'import "t.ml"; import "t2.ml"; mk2(4)'                         # §2.4a → unbound 'Algo'
./langwb -e 'type Option = None | Some(val); import "t2.ml"; mk2(1)'        # §2.4a
./langwb -e 'import "t.ml"; Some(9)'                                        # §2.4a → unexported ctor visible
./langwb -e 'type Option = None | Some(val); match Some(1) {
  Some(a, b) => a }'                                                        # §2.5 → reported at 1:3
# §2.1a LSP: didOpen with text `{x:1} == {x:1}`, then hover → no response
# §2.4b LSP: didOpen file:///…/my%20dir/main.ml containing `import "q.ml"; q`
# t.ml:  type Option = None | Some(val); def mk = fn(x) => Some(x); export mk;
# t2.ml: type Option = Nada | Algo(v); def mk2 = fn(x) => Algo(x); export mk2;
```
