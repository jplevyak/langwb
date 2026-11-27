# Step 12.5 — Module Semantics

> **Goal (from [ROADMAP.md](ROADMAP.md)):** what a module can see, and what a
> language's hooks reach, should follow the program's import edges. Two
> Step 11 friction items ([STEP11_DOGFOOD_LANG.md](STEP11_DOGFOOD_LANG.md))
> show where it does not today: ADT constructors are visible to modules that
> never imported them (FL-15), and `hook module_begin` wraps imported
> modules' code as well as the entry module's (FL-18). Step 12 left both
> alone because they change what programs mean, not how errors read
> ([STEP12_DIAGNOSTICS.md](STEP12_DIAGNOSTICS.md) §2.6).
>
> **Status: planned.**

---

## 0. Approach

1. **Baseline first.** §1.3 records today's behavior for each case. Each
   case becomes a test before its milestone changes it, so every changed
   expectation is a before/after pair (§5).
2. **Each fix is general.** Each milestone gets a minilang-only test, and
   MLite only confirms the result. Nothing in the fix is MLite-specific.
3. **Breaking on purpose, but narrowly.** Both fixes reject programs that
   work today. The only such programs in the repository are the two pinned
   tests (`mlite_m3_ctor_visible_transitively_fl15`,
   `mlite_m4_module_begin_reaches_imports_fl18`); `examples/mlite/` and
   `std/` already import what they use. A program that stops working gets an
   error that says what to import or re-export.
4. **No `ifa/` or `dparser/` changes are expected.** Both fixes are in
   `src/workbench/` (module assembly) and `src/ifa_bridge/` (constructor
   resolution).

---

## 1. How Modules Work Today

### 1.1 Assembling a program

`Workbench::eval_impl` reads the entry file form by form. An `import`
calls `import_module_file`, which:

- evaluates the imported module in its own `Workbench` (`mod_wb`), started
  from its `#lang` snapshot or, for a header-less module, from the
  importer's language;
- gives the module a fresh scope (`mod_scope`) and adds it to each of the
  module's forms;
- appends the module's expanded statements to the importer's statement list
  (`out_stmts`), followed by one `import_alias` definition per exported
  `def` (`make_import_alias_def`), which rebinds the name with the
  importer's scope.

A module is evaluated once (`imported_cache`); a second import of it only
runs `apply_exports` again, which adds its aliases to the second importer.
Statement order is therefore: each import's statements and aliases, at the
position of its `import`, then the importer's own forms.

At the end, `eval_impl` puts every statement into one `module` `Seq` and
calls `apply_module_begin` on it **once**. Imported modules are expanded
with `run_module_begin=false`.

### 1.2 Constructors

An ADT declared in a module is registered in `PrimRegistry` and travels to
importers as `ImportedModule::adt_defs`. Each `AdtVariant` carries one
`private_scope`:

- `0` (visible everywhere) when the module exports the type or that
  constructor, or when the type is declared in the entry module or in a
  `.lang`;
- the defining module's `mod_scope` otherwise, so only that module's code
  (and macro templates registered there) can name it.

The check `private_scope == 0 || id->scopes.contains(private_scope)` is
written out in four places in `src/ifa_bridge/bridge.cc` (constructor
references, nullary constructors, constructor calls, patterns), and
`private_variant_note` explains a hidden one ("is not exported by module
...").

Definitions are different: each import edge gets its own `import_alias`, so
`result.mlite` importing `option.mlite` does not make `unwrap_or` visible to
`result`'s importers. Constructors follow the first rule and functions the
second; that is FL-15.

### 1.3 Baseline

Checked against the current binary (minilang unless noted; the files are
as described in each row).

| # | Case | Today |
|---|---|---|
| A1 | MLite: `mv.mlite` (`#lang mlite`) is `fun mv() = mlite_version; export mv;`; the entry imports it and calls `mv()` | `4` (`mlite_m4_module_begin_reaches_imports_fl18`) |
| A2 | Header-less `lib.ml`: `def lib_v = fn(u) => magic + u;`. The entry declares a `module_begin` that binds `magic = 32` unhygienically, imports `lib.ml`, and evaluates `lib_v(10)` | `42` |
| A3 | Same hook, `def k = magic; import "lib.ml"; k` (import after an own form) | `32` |
| C1 | MLite: `import "std/result.mlite"; case Some(3) of ...` (`result` imports `option` and does not export `Option`) | `3` (`mlite_m3_ctor_visible_transitively_fl15`) |
| C2 | `a.ml` exports `Opt = Non \| Som(v)` and `mk`; `b.ml` imports `a.ml`, exports only `wrap`; the entry imports `b.ml` and matches `Som(1)` | `1` |
| C3 | As C2, but `b2.ml` also has `export ..., Opt;` | `2` (the `export Opt` is accepted with no warning but has no effect, since `Som` is already visible) |
| C4 | The entry imports `b.ml` and then `a.ml` (diamond) | `3` |
| C5 | `p1.ml` and `p2.ml` each declare a different type with an unexported constructor `Leaf`; the entry imports both | error: `variant 'Leaf' imported from 'p2.ml' conflicts with existing variant of type 'P'` |
| C6 | `b3.ml` imports `a.ml` and has `export mk;` | warning: `'mk' exported from 'b3.ml' is not defined` |

A1–A3 and C1–C4 change in this step; C5 and C6 do not (§4).

---

## 2. FL-18: `module_begin` Wraps Only the Entry Module's Own Forms

### 2.1 Design

`eval_impl` keeps two lists instead of one:

- `imported_stmts`: what `import_module_file` produces (imported modules'
  statements and the entry's `import_alias` definitions), in import order;
- `own_stmts`: the entry module's own expanded forms.

The program is `Seq(imported_stmts..., module_begin(Seq(own_stmts...)))`.
The hook's binding (`mlite_version`, `magic`) then scopes over the entry's
own forms only. Imported code cannot see it, and the entry still sees every
import, because the aliases come before the wrapper. This holds even for an
imported module in the same language: a `#lang mlite` library cannot use
`mlite_version` (A1). What a language's hook provides is a property of the
program, not of each module. A program with no
`module_begin` hook gets the same two-part `Seq`.

**Consequence: imports are hoisted.** An `import` after some of the entry's
own forms (A3) now runs before them. Imported modules cannot refer to the
entry's definitions, so only top-level side effects in an imported module
(a `print` at a module's top level) can observe the change. This is
documented, not diagnosed.

The single-expression fast path (`top_stmts.empty()` → `eval_single_expr`
with `run_module_begin=true`) is unchanged: it only runs when there are no
imports.

### 2.2 Why not run each module's own `module_begin`?

Racket runs each module's `#%module-begin` on that module. Here the hook's
output is a `let ... in <Seq> end` around the module. Definitions inside that
`Seq` are local to it: the bridge collects `def`s per `Seq`
(`bridge.cc`, Pass 2a/2b). An importer's `import_alias`, outside the `let`,
could not reach them. Per-module hooks would therefore need definitions that
escape a wrapper, which is a larger change with no current use case. It is
listed under §4 (out of scope), and the entry-only rule above is what FL-18
proposed.

### 2.3 Tests

- `mlite_m4_module_begin_reaches_imports_fl18` → error test
  `mlite_m4_module_begin_entry_only_fl18`: `unbound identifier
  'mlite_version'`, located in `mv.mlite`.
- New (minilang): A2 as an error test (`unbound identifier 'magic'` in
  `lib.ml`); a hook-wrapped entry that uses `magic` and an imported function
  together (`magic + lib_w(1)` where `lib_w` does not use `magic`) still
  works; A3 still gives `32`; an imported module with a top-level
  side effect placed after an own form runs first (pins the hoisting).
- Unchanged: every existing `module_begin` test (`for_syntax_hook_under_replaced_grammar`, the MLite M4 cases) and `expect_file` of every MLite example.

---

## 3. FL-15: Constructor Visibility Follows Import Edges

### 3.1 Design

**Visibility is a set of scopes.** `AdtVariant::private_scope` becomes
`std::vector<uint32_t> visible_in`, with a helper
`bool AdtVariant::visible_to(const syntax::ScopeSet &)` that replaces the
four inline checks. An empty set means public; that still applies to types
declared in the entry module or in a `.lang`. For a type declared in an
imported module, the set holds:

1. the defining module's `mod_scope` (always, as today); and
2. the importer's scope, for each import edge whose module exports the
   constructor: it defines and exports the type or that constructor, or it
   **re-exports** it (below).

`apply_exports` adds the importer's scope (2). It must do so on every path,
including the `same_module_type` path that today skips a type that is
already registered (C4, the diamond). When a type reaches an importer by
two edges, the sets are merged.

**Re-export.** A module that imports a type and lists the type (or one of
its constructors) in `export` passes it on: its importers get the
constructors as if they had imported the defining module. This reuses the
existing `export Name;` syntax. Today that `export` is accepted and has no
effect (C3), so no program changes meaning. `ImportedModule` records the
re-exported constructor names next to `adt_defs`.

**Hygiene needs nothing new.** Macro templates registered in a module carry
that module's scope (`decl_scope_`), so an exported macro whose template
names `Som` keeps working in an importer that never imported `a.ml`, as long
as the macro's module can see `Som`. This is the property that makes
exported macros with private helpers work today (`pick_ok`).

**Message.** A constructor that exists but is not visible gets one of two
explanations (`private_variant_note`):

- not exported by its module (unchanged): `constructor 'Tk' of type 'Tok' is
  not exported by module 'priv.mlite' (add 'export Tk;' or 'export Tok;'
  there)`;
- exported, but not imported here (new): `constructor 'Some' of type
  'Option' is not imported here (module 'option.mlite' exports it; import it
  or re-export 'Option' from the module you import)`.

To tell these apart, each variant also records whether its defining module
exports it.

### 3.2 What else reads constructor visibility

- `bridge.cc`: the four checks (identifier, nullary constructor, call,
  pattern) and `private_variant_note`.
- `workbench.cc` `import_module_file`: computing `visible_in` instead of
  `private_scope`, and the pass-through of imported ADTs (the comment at
  "ADTs this module imported are passed through unchanged" becomes
  "passed through with their sets").
- LSP: constructor hover and definition (`DocIndex` ctors,
  `constructor_word_at`) read `AdtVariant` positions, not visibility. A test
  confirms definition still works on a constructor imported directly, and
  that an invisible one gives the error diagnostic, not a crash.

### 3.3 Tests

- `mlite_m3_ctor_visible_transitively_fl15` → error test
  `mlite_m3_ctor_not_visible_transitively_fl15` with the new message.
- New (minilang): C2 as an error test (constructor and pattern positions,
  both with the new message); C3 now `2` *because of* the re-export, and
  the same file without `export Opt` fails; C4 still `3` (diamond: the
  scope added on the cached path); an exported macro from `b.ml` whose
  template builds `Som(x)` works in an entry that imports only `b.ml`;
  `b.ml`'s own code still uses `Som` (defining-edge visibility).
- Unchanged: the private-constructor tests
  (`mlite_m3_private_ctor_hidden`, `..._pattern_hidden`,
  `review6_private_ctor_in_pattern_explained`),
  `review6_qualified_adt_prints_ctor_name`, `mlite_m3_expr_eval`,
  `mlite_m3_wordcount`, `mlite_m4_aot_cli`.

---

## 4. Out of Scope

Each item below was considered and left for a concrete need. Each is
recorded in [ROADMAP.md](ROADMAP.md) *Deferred* when the step closes.

- **Per-module `module_begin`** (§2.2). This needs definitions that escape
  the hook's wrapper.
- **Same-named private constructors in two modules** (C5). Variants are
  looked up by name (`PrimRegistry::find_variant`, a dozen call sites), so two
  modules cannot both declare `Leaf`, even privately. With import-edge
  visibility the conflict could be limited to constructors visible in the
  same scope, but that means resolving constructors by binding, not by name.
- **Re-exporting functions** (C6). `export f;` of an imported `f` warns
  "not defined". Supporting it means forwarding the module's own
  `import_alias` to its importers. That is symmetric with §3's type
  re-export, but nothing needs it yet.
- **Entry-module and `.lang` ADTs stay public.** An imported module could in
  principle name a constructor the entry declares (lowering sees one
  registry). No program does, and modules have no way to import the entry.

---

## 5. Milestones

| Milestone | Scope | Exit criterion |
|---|---|---|
| **M0** | Baseline tests for A1–A3 and C1–C4 as they behave today, in `tests/run_tests.sh`. | All tests pass; §1.3 matches the pinned output. |
| **M1** | FL-18 (§2): two statement lists in `eval_impl`, `module_begin` on the entry's own forms only. | §2.3: A1/A2 become errors, A3 and all other tests unchanged except the import-hoisting test. Change log in §6. |
| **M2** | FL-15 (§3): `visible_in`, `visible_to`, re-export, new message. | §3.3: C1/C2 become errors, C3 works because of the re-export, C4 unchanged. Every MLite example and stdlib file unchanged. Change log in §6. |
| **M3** | Close the step: §4 items into ROADMAP *Deferred*, FL-15 / FL-18 marked fixed in STEP11_DOGFOOD_LANG.md, ROADMAP Step 12.5 marked done. | Docs updated. |

M1 and M2 are independent; M1 first because it is smaller and touches only
`eval_impl`. Each fix is one commit with its minilang test.

---

## 6. Change Log (filled in per milestone)

*Empty until M0.*
