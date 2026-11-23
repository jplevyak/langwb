# langwb review — 2026-11-23, `bb6f4eb..9d9fc47`

This is a delta review of everything since the previous review
(`2026-11-22-review-ce2fa9b..HEAD.md`, which covered up to `bb6f4eb` plus the
working tree at that time):

| change | subject |
|---|---|
| `455c599` | Native `.g` grammar syntax, shortcut cleanups, and fixes for the previous review (the working tree that review saw, plus the fixes) |
| `5720e75`, `f557568`, `3d3c06f` | ROADMAP / README updates |
| `227c236` | Step 9: one shared `langwb_rt` runtime, readable errors, capture fixpoint, `check` warnings |
| `268e00e` | Step 10: mutation and `while`, `print`/`println`, strings, tagged lists, `import`/`export` |
| `698a600` | Step 10.5: sum types (ADTs) with a `CreationSet` per variant and `match` narrowing |
| `9d9fc47` | README for Steps 9–10.5 |

The build has no warnings. `make test` passes: the smoke tests, all
`examples/minilang/*.ml`, and 165 cases in `tests/run_tests.sh`. Every
finding below was reproduced against `9d9fc47` with the quoted `-e`
expression or files.

The top four:

1. **Assigning to a captured variable inside a closure does nothing, with no
   error (§2.1a).** `let x = 1 in { def f = fn(u) => { x = x + 1 }; f(0); f(0); x }`
   → `1`. Closures copy captured values into hidden parameters, so `x = …`
   writes to the copy. Assignments made after a closure is created are not
   seen by the closure either.
2. **`print`/`println` of a record, ADT value or closure passes the object
   pointer to `fputs` as if it were a C string (§2.3a).** `println({ x: 1 })`
   prints raw heap bytes. This is an out-of-bounds read that can crash.
3. **Imports are resolved relative to the process's working directory, not
   the importing file (§2.4a).** `langwb sub/main.ml` fails to find
   `sub/a.ml`'s `import "b.ml"`. Running `cd sub && langwb main.ml` works.
4. **Several import cases are silently wrong (§2.4b–e).** A `type` declared
   in a module is not visible to the module's own code once it is imported.
   An exported macro that uses a private `def` of its module fails with
   `unbound identifier`. Import cycles produce empty modules instead of an
   error. Two modules exporting the same name: the last one wins with no
   warning.

---

## 1. Status of the previous review

Nearly everything was addressed, and the fixes hold up:

| prior finding | status |
|---|---|
| §2.1 `langwb_rt.c` untracked, explorer `.inc` not ignored | **Fixed** |
| §2.2 declaration-body error columns relative to the body | **Fixed.** `macro m => 1 + * 3; 5` → `1:15`. `syntax_rule` uses same-length placeholders and reports `1:41` (the `)`) |
| §2.3a legacy quoted `grammar` RHS silently reinterpreted | **Fixed.** Rejected with a migration hint |
| §2.3b `prod : …` clears a core production | **Fixed.** `:` on an existing production is an error. `:=` replaces |
| §2.3c `syntax_ext` deltas lose `:` | **Fixed.** Deltas now replay `GrammarOp`s (`clear`/`add`) |
| §2.3d declarations dropped inside `syntax_ext` | **Fixed.** Unsupported declarations are rejected with an error |
| §2.4a LSP deadlock on >64 KB of stderr | **Fixed.** Uses `tmpfile()` |
| §2.4b diagnostics parsed line by line | **Mostly fixed.** Continuation lines are folded and `<macro:…@L:C>` maps to the call site. The redundant `invalid declaration near '…'` line is still sent as a second error at `0:0` |
| §2.4c `positionEncoding` | **Fixed.** Debounce and multi-line hover are moved to ROADMAP Step 12 |
| §2.5 lists print addresses, `\n` not escaped, no `true`/`false` | **Fixed for printing** with tagged cons cells. **Still open for typing:** `head(["a"]) + 1` still type-checks (§2.3c) |
| §2.6 nested `Amb` | **Fixed.** `review2_nested_amb_resolves` |
| §2.7 two copies of the runtime, absolute library path, temp-file races, silent default output path | **Fixed.** The `-L/opt/homebrew/lib` flags are still hard-coded in the `Makefile` |
| §2.8 `$binary_op_left`, silently dropped captures, wasteful `group_caps`, `__wb_rec_N` | **Fixed.** The capture fixpoint (`known_issue_def_sibling_capture_fixpoint`) is a good change |
| §1 `check nosuch` silently accepted | **Fixed.** It now warns |

Steps 9 and 10.5 also make several structural improvements:

- `langwb_rt.c` is now the only runtime, and an emit sink selects where the
  JIT writes its result. This removes the drift risk the previous review
  raised.
- Lowering an ADT variant to its own `Type_RECORD` class and lowering `match`
  arms to `isinstance` lets IFA's existing SSU narrowing keep per-variant
  field types. `Ok(int)` and `Err(string)` in the same `Result` analyze
  correctly (`step10_5_adt_result_heterogeneous_narrowing`). This approach is
  the right fit for IFA.
- Errors now say `record { a }` instead of `__wb_rec_1`, and an arity
  mismatch is reported with the callee's name.

---

## 2. Findings

### 2.1 Mutation (`Assign`, `SetField`, `While`)

**(a) Assigning to a captured variable has no effect.** `lower_expr`
(`bridge.cc`, `CoreFormKind::Assign`) emits `if1_move(rhs, *bound)`. Inside a
closure, `*bound` is the hidden capture parameter, which is a copy:

```
$ ./langwb -e 'let x = 1 in { def f = fn(u) => { x = x + 1 }; f(0); f(0); x }'
1                                   # expected 3
$ ./langwb -e 'let x = 1 in let g = fn(u) => x in { x = 5; g(0) }'
1                                   # the closure captured the value 1
$ ./langwb -e 'let x = 1 in { def f = fn(u) => x; x = 5; f(0) + x }'
6
```

Mutating a record field through a closure works (`c.n = c.n + 1` → `2`),
because the record is shared by reference. Pick one of these fixes:

- Box assigned captured variables. A local that is both captured and the
  target of an `Assign` becomes a one-field record, created where it is
  bound, and every read or write goes through the record.
- Reject the program. In `resolve_node`, report an error when the target of
  an `Assign` resolves to a binder outside the enclosing lambda, and
  document that locals are captured by value.

The first fix matches what users expect. The second is a small change that
removes the silent wrong answer.

**(b) `SetField`'s object operand captures the whole expression to its
left.** The rule is `expr '.' ident '=' expr` at `$right 0`, so the object
`expr` can be any expression at all:

```
$ ./langwb -e 'let p = {x:1} in { 1 + p.x = 5; p.x }'
error: illegal primitive argument type 'p' …          # parsed as (1 + p).x = 5
$ ./langwb -e 'let p = {x:1} in { if true then p.x = 5 else p.x = 6; p.x }'
error: unresolved member 'x' of class 'int64'         # (if … else p).x = 6
$ ./langwb -e 'let c = {n:0} in { def inc = fn(u) => c.n = c.n + 1; … }'
error: unresolved member 'n' of class 'closure'       # (fn(u) => c).n = …
```

Restrict the object operand to a postfix or atom non-terminal, for example
`postfix '.' ident '=' expr`. Alternatively, give the rule a higher priority
and make `fn`, `if` and `let` bodies win. The `Assign` rule
`ident '=' expr` does not have this problem, because its left side is a
single token.

**(c) Assigning a new value to a function binding is ignored or misprinted.**

```
$ ./langwb -e '{ def f = fn(x) => x; f = fn(x) => x + 1; f(1) }'
1                                    # Pass 2b rebinds `f` to the lowered lambda, so the move is lost
$ ./langwb -e 'let f = fn(u) => u in { f = 3; f }'
<fn>                                 # `f` is {closure, int64}; the result printer chooses <fn>
```

The second case is correctly a type error for `f + 1`. The result printer
should report a union type instead of choosing one member. For a `def`
binder, either reject the assignment or stop rebinding.

**(d) Smaller issues:**

- `while 0 do 1` gives `<input>:1: error: illegal primitive argument type
  expression illegal: int64`. The message has no column and does not say
  that the condition must be a `bool`.
- `print(1); print(2); 3` prints `123`. Program output and the result share
  a line. The REPL should print a newline before the result when the program
  wrote output.
- `p.val = …` and `o._tag = 5` are accepted on ADT values, so variant
  fields, including the synthesized `_tag`, can be changed. That is
  defensible, but decide whether you want it and document it.

### 2.2 Sum types (ADTs)

**(a) Constructors are looked up by bare name across all types.**
`PrimRegistry::variant_map_` uses `v.name` as the key, so a second
declaration silently replaces the first type's variant:

```
$ ./langwb -e 'type A = X(v); type B = X(w); match X(1) { X(q) => q }'
1                                    # A::X is no longer reachable; no diagnostic
$ ./langwb -e 'type T = A | B | A; 1'      # accepted
$ ./langwb -e 'type T = A(x, x); A(1, 2)'  # { x: 2 }: duplicate field names accepted
```

Reject duplicate constructor names within a type and across types, as ML
does, unless qualified names `T::V` are planned. Also reject duplicate field
names. `register_adt` is also never cleared, so in a REPL session a
redefined `type` adds a second `AdtDef`. `init_adt_types` then creates a
`Sym` for every stale variant as well.

**(b) Values print as anonymous records.** `Some(3)` prints `{ val: 3 }` and
`None` prints `{ _tag: 0 }`, because the record branch of the result printer
runs first. The new `"::"` branch in `ensure_wb_result_registered` is
unreachable in these cases. Inside lists, ADT values print as `<val>`. The
README says ADT values print. Print `Some(3)` and `None`, and keep `_tag` out
of the output.

**(c) Pattern errors and pattern limits.**

- `match Some(1) { Some(a, b) => a }` reports `<input>:1:12: error: invalid
  field index 1 for variant 'Some'`. The location is the `match` macro's
  template position, and the message describes an internal detail. Check the
  pattern's arity in the `match` expander, where the pattern syntax is
  available: "pattern `Some(a, b)` has 2 fields; `Some` has 1".
- `match runtime_val(0) { A => 1, B => 2 }` with `type T = A | B` compiles,
  and fails only at run time with `non-exhaustive match`. A capitalized
  pattern on a scrutinee that is not of an ADT type, or a constructor that
  cannot match, should be a static error. IFA already knows the scrutinee's
  type.
- Nested patterns (`Some(Some(v))`) are a syntax error. That is consistent
  with the documented limits, but the README should mention constructor
  patterns specifically.
- Whether a pattern is a constructor or a variable depends on whether its
  first letter is uppercase (`tok[0] >= 'A'`). An unknown uppercase name such
  as `Foo` is therefore reported as an unknown variant, never bound as a
  variable. That is a reasonable rule, but document it.

**(d) `__wb_is_variant` / `__wb_variant_field` are matched by text before the
environment is consulted** (`lower_expr`, `CoreFormKind::App`). A user
binding with either name is ignored. Give these hygienic names, or check
`env.resolve` first, as the constructor path does.

**(e) `ifa/` edit.** `698a600` changes
`ifa/codegen/codegen_common.cc:cg_isinstance_tag_targets` directly in the
subtree. This is the same pattern the review before last flagged. Two
problems:

- The new `impl->id == mid` loop is equivalent to the pointer check that
  comes just before it, because `Sym::id` values are unique. Either it is
  dead code, or it hides a case where two `Sym` objects share an id, which
  would be a bug in its own right. Find out which case `fam->type` covers
  and keep only that check.
- `concrete.add(m)` for tagged members changes the result for every IFA
  front end, pyc included.

Upstream it with a pyc regression run, as was done for `56095e2`.

### 2.3 I/O, strings and tagged lists

**(a) `print`/`println` on a pointer that is not a string.** The `reg_print`
emitter (`workbench.cc`, `init_builtin_prims`) sends anything that is
`isPointerTy()` to `_CG_wb_print_str`:

```
$ ./langwb -e '{ println({ x: 1 }); 0 }'
h@6                                  # bytes of the record header
$ ./langwb -e 'type O = N | S(v); { println(S(65)); 0 }'
<invalid UTF-8>
$ ./langwb -e '{ println(fn(x) => x); 0 }'
<empty line>
```

This reads memory until it finds a NUL byte. Dispatch on `arg_var->type`
only: `sym_string` prints as a string, records and ADTs go through the same
printer as the result printer (which also fixes §2.2b), and anything else
prints `<fn>`/`<val>` or is a compile-time error. Do the same in the `cons`
emitter: its `isPointerTy()` → `WB_TAG_OBJ` fallback is safe only because
the list printer never dereferences an `OBJ` element.

**(b) `==` on strings is a type error.** `"ab" == str_concat("a", "b")`
gives `illegal primitive argument type … string`. That is acceptable, with
`str_eq` as the workaround, but users will write `==` first. Give a message
that names `str_eq`, or lower `==` on two strings to `str_eq`.

**(c) Tagged lists are tagged only for printing.** `head` is still typed
`int64` whatever the element is, so `head(["a"]) + 1` compiles and returns an
address plus one. `match ["a", "b"] { [h | t] => h }` returns an address,
because the list pattern expands to `head`. `head_str([1, 2])` returns `""`,
and `head_bool([5])` returns `true` (it ignores the tag). `head([{x: 1}])`
returns the record pointer as an integer. The tags exist at run time, so:

- `head_str`, `head_bool` and `head_list` should trap on a tag mismatch, as
  `wb_match_fail` does.
- `head` should check for `WB_TAG_INT`, and the list pattern should use the
  tag.
- In the longer term, let IFA give a `cons` cell an element-type
  `CreationSet`, the same mechanism the ADTs now use.

The ROADMAP entry for `455c599` still says "int64-only list enforcement",
which Step 10 removed. Update it.

**(d) Minor:** `str_to_int("12abc")` → `12` and `str_to_int("x")` → `0`, with
no way to detect the error. `str_slice` clamps silently. Both are fine for
now, but document them.

### 2.4 `import` / `export`

**(a) Paths are resolved against the CWD.** `import_module_file` tries
`spec`, `spec.ml` and `spec.lang` relative to the process's working
directory, then `lang_search_paths_`. The directory of the importing file is
never tried:

```
$ cat sub/main.ml      # import "a.ml"; fa(20)
$ cat sub/a.ml         # import "b.ml"; def fa = fn(x) => fb(x) + 1; export fa;
$ ./langwb sub/main.ml
langwb: cannot find imported module 'a.ml'
$ (cd sub && ../langwb main.ml)
41
```

The tests pass only because they use absolute paths. Resolve relative to
the importing file's directory: thread its path through `eval_impl`, or use
the URI's directory for the LSP. Fall back to the search paths after that.
Without this, the LSP cannot resolve any relative import, because the
server's CWD is unrelated to the document.

**(b) An ADT declared in a module is lost.** `Workbench mod_wb(*this, …)`
takes a copy of `prims_`. `type` registers into that copy, but
`init_adt_types` lowers the combined program with the importer's registry:

```
$ cat t.ml     # type Option = None | Some(val); def mk = fn(x) => Some(x); export mk;
$ ./langwb -e 'import "t.ml"; match mk(4) { Some(v) => v, None => 0 }'
langwb: 2:18: unbound identifier 'Some'
```

Module types must reach the importer's `prims_` even when they are not
exported, because the module's own code needs them. Export controls only
which constructor names the importer can see.

**(c) Exported macros are not hygienic with respect to their module.** The
template identifiers of a `syntax_rule` do not carry `mod_scope`, so they
resolve at the use site:

```
$ cat priv.ml  # def secret = 41; syntax_rule addsec($x:expr) => ($x + secret); export addsec;
$ ./langwb -e 'import "priv.ml"; addsec(1)'
langwb: 1:14: unbound identifier 'secret' (expanded from 'addsec' at 1:18)
```

This is the main point of hygienic modules, and the test
`step10_import_export_hygiene_and_diamond` misses it, because `add10` uses
no module bindings. Add the module's scope to the macro's template when the
macro is defined, so it is captured with the definition, and add a test.
Private `def`s that macros refer to must then be emitted with `mod_scope`,
which they already are.

**(d) Import cycles succeed and yield empty modules.** The placeholder
`ImportedModule{.mod_scope}` added "to guard against circular imports" is
then returned as a cache hit with no exports:

```
$ cat c1.ml    # import "c2.ml"; def f1 = …; export f1;
$ cat c2.ml    # import "c1.ml"; def f2 = fn(x) => f1(x) + 1; export f2;
$ ./langwb -e 'import "c1.ml"; f2(1)'
langwb: 2:18: unbound identifier 'f1'
```

Keep an "in progress" flag and report `import cycle: c1.ml → c2.ml → c1.ml`.

**(e) Errors in module code have no file name.** The `2:18` in (b) and (d)
is a line in `t.ml` or `c2.ml`. The message does not say which file, and the
LSP shows it at line 2 of the importing document. Declaration errors include
`found_path`, but resolution and IFA errors do not. `SyntaxObj` needs a
file, or a source id, next to its line and column. This belongs with the
structured diagnostics planned in ROADMAP Step 12, but the gap is visible as
soon as imports exist.

**(f) Smaller issues:**

- When two modules export the same name, the later `import_alias` silently
  shadows the earlier one (`import "m1.ml"; import "m2.ml"; g(0)` → `2`).
  Report an ambiguous import, or require qualification.
- `export nosuch;` is accepted silently. Warn when an exported name is not
  defined at the module's top level.
- Top-level expressions in a module, such as `println("loaded")`, run once
  per program, which is correct, but where they run is not specified. If
  the importer has no final expression, the module's last expression becomes
  the program's result.
- `import` is recognized only as a top-level form. `{ import "x"; … }` is a
  syntax error. That is a reasonable restriction, but say so in the error.
- `imported_cache` is created per `eval_impl`, so every REPL line re-reads
  and re-expands every import. That is fine for now.

### 2.5 `check core` / `check call`

The list of core form names is hard-coded twice: in `workbench.cc` (`5c`) and
in `contour_clone.cc` (the new warning). Both copies already lack `while`,
`assign` and `set_field`:

```
$ ./langwb -e 'check core while => 0; 5'
langwb: unknown core form 'while' in 'check core' declaration
```

Build the set from `parse::core_form_name` over the `CoreFormKind`
enumerators instead.

`check call f` parses, but `call` is never read, so it behaves exactly like
`check f`. `check call binop` matches the `binop` core form, not a function
named `binop`. Either store the kind (core or call) with the check and match
only that kind, or remove `call` from the grammar.

The warning for an unmatched target treats any terminal text in the program
as a match (`seen_ids`), so a string literal or keyword that happens to equal
the target name silences the warning. Compare against identifier nodes only.

### 2.6 Arity checking

`fn_arity` is propagated through `let`, `def` and `import_alias` moves, but
not through function parameters, record fields or `if` results. A function
reached through one of those still gets IFA's generic error. That is fine as
a first step. Note that a non-empty `per_def_caps` adds hidden parameters to
the IF1 function, while `fn_arity` records only the user's parameter count.
That is correct today, because the hidden arguments are appended at the call
site after the check. Keep the two counts separate, and comment why.

---

## 3. Recommended order

1. Make `print`/`println` dispatch on IFA types only (§2.3a). This is a
   memory-safety bug and a small change.
2. Report an error for, or box, assignments to captured variables (§2.1a),
   and restrict `SetField`'s object operand (§2.1b).
3. Resolve imports relative to the importing file, and propagate module
   `type`s (§2.4a–b).
4. Add the module's scope to exported macro templates, and report import
   cycles (§2.4c–d).
5. Reject duplicate constructor names and fields, and print ADT values as
   constructors (§2.2a–b).
6. Derive the `check core` name set from `CoreFormKind`, and implement
   `check call` or remove it (§2.5).
7. Upstream the `cg_isinstance_tag_targets` change, without the `id` loop
   (§2.2e).
8. Make the tagged-list accessors check their tags (§2.3c).

---

## 4. Reproductions

```
./langwb -e 'let x = 1 in { def f = fn(u) => { x = x + 1 }; f(0); f(0); x }'   # §2.1a → 1
./langwb -e 'let x = 1 in let g = fn(u) => x in { x = 5; g(0) }'               # §2.1a → 1
./langwb -e 'let p = {x:1} in { 1 + p.x = 5; p.x }'                            # §2.1b
./langwb -e 'let c = {n:0} in { def inc = fn(u) => c.n = c.n + 1; inc(0); c.n }'  # §2.1b
./langwb -e '{ def f = fn(x) => x; f = fn(x) => x + 1; f(1) }'                  # §2.1c → 1
./langwb -e 'let f = fn(u) => u in { f = 3; f }'                                # §2.1c → <fn>
./langwb -e 'type A = X(v); type B = X(w); match X(1) { X(q) => q }'            # §2.2a
./langwb -e 'type T = A(x, x); A(1, 2)'                                         # §2.2a
./langwb -e 'type Option = None | Some(val); Some(3)'                           # §2.2b → { val: 3 }
./langwb -e 'type Option = None | Some(val); match Some(1) { Some(a, b) => a }' # §2.2c
./langwb -e 'type T = A | B; match runtime_val(0) { A => 1, B => 2 }'          # §2.2c → runtime trap
./langwb -e '{ println({ x: 1 }); 0 }'                                          # §2.3a → heap bytes
./langwb -e 'head(["a"]) + 1'                                                   # §2.3c → address + 1
./langwb -e 'match ["a", "b"] { [h | t] => h }'                                 # §2.3c → address
./langwb -e 'check core while => 0; 5'                                          # §2.5
# §2.4: see the module files quoted in §2.4a–d (relative import, type in a module,
#       private def in an exported macro, c1 ↔ c2 cycle)
```
