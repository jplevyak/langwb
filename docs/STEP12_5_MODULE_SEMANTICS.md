# Step 12.5 — Module Semantics

> **Goal (from [ROADMAP.md](ROADMAP.md)):** what a module can see, and what a
> language's hooks reach, should follow the program's import edges: a
> module's meaning comes from its own text and what it explicitly imports.
> Two Step 11 friction items ([STEP11_DOGFOOD_LANG.md](STEP11_DOGFOOD_LANG.md))
> show where it does not today: ADT constructors are visible to modules that
> never imported them (FL-15), and `hook module_begin` wraps imported
> modules' code as well as the entry module's (FL-18). Step 12 left both
> alone because they change what programs mean, not how errors read
> ([STEP12_DIAGNOSTICS.md](STEP12_DIAGNOSTICS.md) §2.6). The plan review
> found a third: a module without a `#lang` header is expanded with its
> first importer's declarations (§4).
>
> **Status: in progress (M0–M1 done).**

---

## 0. Approach

1. **Baseline first.** §1.3 records today's behavior for each case. Each
   case becomes a test before its milestone changes it, so every changed
   expectation is a before/after pair (§7).
2. **Each fix is general.** Each milestone gets a minilang-only test, and
   MLite only confirms the result. Nothing in the fix is MLite-specific.
3. **Breaking on purpose, but narrowly.** The fixes reject programs that
   work today. Across the entire repository, the only affected programs are
   three pinned tests (`mlite_m3_ctor_visible_transitively_fl15` for FL-15,
   `mlite_m4_module_begin_reaches_imports_fl18` for FL-18, and
   `import_module_without_lang_header_inherits_importer_lang` for §4);
   `examples/mlite/`, `std/`, and all other module tests already declare or
   import what they use (for §4, checked by prototyping the change: 526 of
   527 tests pass). Outside the suite, §4 also breaks `--bare` programs that
   import a header-less module (H4). A program that stops working gets an
   error that says what to import, re-export, or declare.
4. **No `ifa/` or `dparser/` changes are expected.** The fixes are in
   `src/workbench/` (module assembly, module languages) and
   `src/ifa_bridge/` (constructor resolution).

---

## 1. How Modules Work Today

### 1.1 Assembling a program

`Workbench::eval_impl` reads the entry file form by form. An `import`
calls `import_module_file`, which:

- evaluates the imported module in its own `Workbench` (`mod_wb`), started
  from its `#lang` snapshot or, for a header-less module, as a copy of the
  importer (its language *and* everything the importer declared before the
  `import`; §4);
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

The check `private_scope == 0 || scopes.contains(private_scope)` is written
out in three places in `resolve_node` in `src/ifa_bridge/bridge.cc`
(`CoreFormKind::Ident` constructor/missing-arg references, `__wb_is_variant`
patterns, and `CoreFormKind::App` constructor calls; `lower_expr`'s two
constructor lookups at `Ident` and `App` rely on `resolve_node` having run,
which leaves a gap if an invisible constructor shares a name with a `prim`),
and `private_variant_note` explains a hidden one ("is not exported by module
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
| A2 | Header-less `lib.ml`: `def lib_v = fn(u) => magic + u; export lib_v;`. The entry declares a `module_begin` that binds `magic = 32` unhygienically, imports `lib.ml`, and evaluates `lib_v(10)` | `42` |
| A3 | Same hook, `lib_ok.ml` (`def lib_w = fn(u) => u + 10; export lib_w;`, not referencing `magic`): `def k = magic; import "lib_ok.ml"; lib_w(k)` (import after an own form) | `42` |
| A4 | `se.ml` is `print("lib"); def q = 1; export q;`; the entry is `print("own"); import "se.ml"; q` | prints `ownlib`, then `1` |
| A5 | `def lib_w = fn(u) => u; import "lib_ok.ml"; lib_w(1)` (an entry `def` and an imported name collide; the import comes later) | `11` (the alias wins); with the `import` first, `1` |
| A6 | `def k = lib_w(1); import "lib_ok.ml"; k` (an imported name used before its `import`) | errors: `'lib_w' has no type`, `'lib_w' is used before assignment on every path`, `illegal call argument type` |
| A7 | `import "lib_ok.ml";` (imports only, no own form) | `<fn>` (the value of the last `import_alias`) |
| A8 | `tw.ml` is `def twice = fn(u) => u * 2; def use_t = fn(u) => twice(u); export use_t;`. The entry declares the A2 hook, then `import "tw.ml"; syntax_rule twice($x:expr) => "($x + 1000)"; use_t(1)` | `1001` (the entry's macro rewrites `tw.ml`'s call); without the hook, `2` |
| H1 | `tw.ml` as in A8. Header-less `m.ml` is `syntax_rule twice($x:expr) => "($x + 1000)"; import "tw.ml"; def via_m = fn(u) => use_t(u); export via_m;`. The entry is `import "m.ml"; import "tw.ml"; use_t(1)`, or the same with the two imports swapped | `1001` with `m.ml` first, `2` with `tw.ml` first |
| H2 | The entry declares `syntax_rule twice(...)` (as in A8) and then `import "tw.ml"; use_t(1)`, no hook | `1001` |
| H3 | `#lang op_prio_uniform.lang` entry imports header-less `inherit_lang_mod.ml` (`def j = 2 * 10 \|> 3; export j;`, using the importer's `\|>`) | `17` (`import_module_without_lang_header_inherits_importer_lang`) |
| H4 | `langwb --bare` on an entry `#lang prelude; import "inc.ml"; inc(1)`, where `inc.ml` is header-less (`def inc = fn(x) => x + 1; export inc;`) | `2` (no test today) |
| C1 | MLite: `import "std/result.mlite"; case Some(3) of ...` (`result` imports `option` and does not export `Option`) | `3` (`mlite_m3_ctor_visible_transitively_fl15`) |
| C2 | `a.ml` exports `Opt = Non \| Som(v)` and `mk`; `b.ml` imports `a.ml`, exports only `wrap`; the entry imports `b.ml` and matches `Som(1)` | `1` |
| C3 | As C2, but `b2.ml` also has `export ..., Opt;` | `2` (the `export Opt` is accepted with no warning but has no effect, since `Som` is already visible) |
| C4 | The entry imports `b.ml` and then `a.ml` (diamond) | `3` |
| C4s | Siblings: `c.ml` imports `a.ml` and its own `cget` matches `Som`; the entry imports `b.ml` and `c.ml` and evaluates `cget(wrap(1))` | `101` |
| C4q | Diamond under a type-name collision: `other.ml` declares an unrelated `type Opt = Zip \| Zap(v)`; the entry imports `other.ml`, `b.ml`, then `a.ml` | error: `variant 'Non' imported from 'a.ml' conflicts with existing variant of type 'a::Opt'` (the first edge renamed `a.ml`'s `Opt` to `a::Opt`, so the second edge's `existing.name == raw_adt.name` no longer finds it) |
| C5 | `p1.ml` and `p2.ml` each declare a different type with an unexported constructor `Leaf`; the entry imports both | error: `variant 'Leaf' imported from 'p2.ml' conflicts with existing variant of type 'P'` |
| C6 | `b3.ml` imports `a.ml` and has `export mk;` | warning: `'mk' exported from 'b3.ml' is not defined` |

A1, A2 and A4–A8 change in M1 (A3 keeps its value `42`). H1–H4
change in M3. C1–C3 and C4q
change in M2; C4 and C4s keep their values but come to depend on the new
merging. C5 and C6 do not change (§5). C4q is a bug independent of FL-15,
fixed by its own commit (§6).

---

## 2. FL-18: `module_begin` Wraps Only the Entry Module's Own Forms

### 2.1 Design

`eval_impl` keeps two lists instead of one:

- `imported_stmts`: what `import_module_file` produces (imported modules'
  statements and the entry's `import_alias` definitions), in import order;
- `own_stmts`: the entry module's own expanded forms.

When `imported_stmts` is non-empty, the program is
`Seq(imported_stmts..., module_begin(Seq(own_stmts...)))` (when
`imported_stmts` is empty, `module_begin(Seq(own_stmts...))` directly, as
today). If `apply_module_begin` rewrites `Seq(own_stmts...)`, only that
wrapped subtree is Stage-0-expanded (`imported_stmts` were already expanded in
their own modules and must not be re-expanded under the entry module's
`stage0_`). The empty-program check (`top_stmts.empty()` today) becomes
`own_stmts.empty()` (`"expected expression after declarations"`), so
`Seq(own_stmts...)` is never empty. This rejects an import-only program
(A7), which today evaluates to its last alias's value; it now behaves like a
file of declarations alone.

Expanding only the wrapped subtree also fixes a leak: today, when the hook
rewrites the program, the whole `Seq` is expanded again by the entry's
`stage0_`, so the entry's macros reach imported modules' already-expanded
code (A8).

The hook's binding (`mlite_version`, `magic`) then scopes over the entry's
own forms only. Imported code cannot see it, and the entry still sees every
import, because the aliases come before the wrapper. This holds even for an
imported module in the same language: a `#lang mlite` library cannot use
`mlite_version` (A1). What a language's hook provides is a property of the
program, not of each module. A program with imports and no `module_begin`
hook gets the same two-part `Seq`.

**Consequence: imports are hoisted.** An `import` after some of the entry's
own forms (A3) now runs before them (and an entry `def` in the inner `Seq`
consistently shadows a same-named `import_alias` in the outer `Seq` regardless
of whether `import` appeared before or after `def`). Imported modules cannot
refer to the entry's definitions, so for non-colliding names only top-level
side effects in an imported module (a `print` at a module's top level, A4)
can observe the change. This is documented, not diagnosed. Two more cases
change with it:

- A colliding name (A5) now always resolves to the entry's `def`; today the
  later of the `def` and the `import` wins.
- An imported name used before its `import` (A6) now works; today it fails
  with three errors that do not mention the order.

The single-expression fast path (`imported_stmts.empty() && own_stmts.empty()`
→ `eval_single_expr` with `run_module_begin=true`) is unchanged: it must check
both lists so a single-expression entry file preceded by `import` does not
discard `imported_stmts`.

### 2.2 Why not run each module's own `module_begin`?

Racket runs each module's `#%module-begin` on that module. Here the hook's
output is a `let ... in <Seq> end` around the module. Definitions inside that
`Seq` are local to it: the bridge collects `def`s per `Seq`
(`bridge.cc`, Pass 2a/2b). An importer's `import_alias`, outside the `let`,
could not reach them. Per-module hooks would therefore need definitions that
escape a wrapper, which is a larger change with no current use case. It is
listed under §5 (out of scope), and the entry-only rule above is what FL-18
proposed.

### 2.3 Tests

- `mlite_m4_module_begin_reaches_imports_fl18` → error test
  `mlite_m4_module_begin_entry_only_fl18`: `unbound identifier
  'mlite_version'`, located in `mv.mlite`.
- New (minilang): A2 as an error test (`unbound identifier 'magic'` in
  `lib.ml`, even when `lib_v` is uncalled); A3 (`def k = magic; import
  "lib_ok.ml"; lib_w(k)`) still gives `42`; A4 prints `libown` (pins the
  hoisting); A5 gives `1` in both orders; A6 gives `11`; A7 is the error
  `expected expression after declarations`; A8 gives `2` with the hook as
  without it.
- Unchanged: every existing `module_begin` test (`for_syntax_hook_under_replaced_grammar`, the MLite M4 cases) and `expect_file` of every MLite example.

---

## 3. FL-15: Constructor Visibility Follows Import Edges

### 3.1 Design

**Visibility is a set of scopes.** `AdtVariant::private_scope` becomes
`std::vector<uint32_t> visible_in`, with a helper
`bool AdtVariant::visible_to(const syntax::ScopeSet &)` that replaces the
inline visibility checks. An empty set means public; that still applies to
types declared in the entry module or in a `.lang`. For a type declared in an
imported module, the set holds:

1. the defining module's `mod_scope` (always, as today); and
2. the importer's scope, for each import edge whose module exports the
   constructor: it defines and exports the type or that constructor, or it
   **re-exports** it (below).

**Merging across import edges (`apply_exports` & `PrimRegistry`).** Because
every imported module's expanded statements are lowered together against the
session's single `PrimRegistry`, `apply_exports` must merge visibility on
every path — including `same_module_type`:

- On `same_module_type`, `apply_exports` **always** unions each incoming
  variant's existing `visible_in` scopes into `PrimRegistry` (even when `mod`
  does *not* export/re-export the type to the current importer, so two sibling
  modules `b.ml` and `c.ml` that both privately import `a.ml` both keep their
  own `mod_scope`s in `session.prims_`), and additionally adds
  `importer_scope` for constructors that `mod` exports or re-exports (C4).
- `PrimRegistry` stores `AdtVariant` copies in both `adts_` and
  `variant_map_`; merging `visible_in` must update **both**.
- `same_module_type` matches by `(origin_module, unqualified_name)` rather
  than `existing.name == raw_adt.name`, because an earlier `name_collision`
  renames `existing.name` in `prims_` to `stem::Name`. This is the C4q fix;
  it is a bug today, independent of visibility, and lands first as its own
  commit with its own test.

**Re-export.** A module that imports a type and lists the type (its
unqualified name) or one of its constructors in `export` passes on **the
constructors of that type that are visible to the module itself**
(`defined_here || v.visible_to({mod_scope})`): its importers get those
constructors as if they had imported the defining module, including across
multi-hop chains (`a → b → c → entry`). A module cannot launder another
module's unexported constructor by naming the type or constructor in `export`
(and `export Priv;` of an invisible constructor warns `"is not defined"`).
That needs a change: today `mod_adt_names` (`import_module_file`) records
every type and constructor in the module's registry, private imported ones
included, so it must be limited to the ones visible in `mod_scope`. This reuses the existing `export Name;` syntax. Today
that `export` is accepted and has no effect (C3), so no working program
breaks. `ImportedModule` records the exported/re-exported constructor names
next to `adt_defs`.

**Hygiene needs nothing new.** Macro templates registered in a module carry
that module's scope (`decl_scope_`), so an exported macro whose template
names `Som` keeps working in an importer that never imported `a.ml`, as long
as the macro's module can see `Som`. This is the property that makes
exported macros with private helpers work today (`pick_ok`).

**Message.** A constructor that exists in `PrimRegistry` but fails
`!v.visible_to(use_scopes)` gets one of two explanations
(`private_variant_note`, which now takes `use_scopes`):

- not exported by its defining module (unchanged): `constructor 'Tk' of type
  'Tok' is not exported by module 'priv.mlite' (add 'export Tk;' or 'export
  Tok;' there)`;
- exported by its defining module, but not imported here (new): `constructor
  'Some' of type 'Option' is not imported here (module 'option.mlite' exports
  it; import it or re-export 'Option' from the module you import)` (naming the
  constructor instead of the unqualified type if the defining module exported
  only that constructor).

To tell these apart, each variant also records whether its defining module
exports it (and `AdtDef` keeps its unqualified name alongside any
collision-qualified `name`).

### 3.2 What else reads constructor visibility

- `bridge.cc`: `resolve_node`'s three checks (`CoreFormKind::Ident`,
  `__wb_is_variant` patterns, `CoreFormKind::App`), `lower_expr`'s two
  constructor lookups (`CoreFormKind::Ident` and `CoreFormKind::App`, so an
  invisible constructor cannot shadow a `prim` of the same name), and
  `private_variant_note(prims, name, use_scopes)`.
- `workbench.cc` `import_module_file`: computing `visible_in` and
  `exported_ctors` instead of `private_scope`, merging `visible_in` in
  `apply_exports`, forwarding imported ADTs with their scope sets, and
  limiting `mod_adt_names` to visible names.
- LSP (`contour_clone.cc` / `lsp.cc`): `InspectResult::variants` filters to
  constructors visible in the entry module's scope (`{mod_scope}`, or public
  when `file_scope == 0`) so `DocIndex::ctors` (`constructor_word_at`) does
  not jump from an unrelated entry binder (e.g. `val Some = 42`) to an
  unimported transitive constructor. A test confirms definition still works on
  a constructor imported directly, and that using an invisible one gives the
  error diagnostic, not a crash.

### 3.3 Tests

- `mlite_m3_ctor_visible_transitively_fl15` → error test
  `mlite_m3_ctor_not_visible_transitively_fl15` with the new message.
- New (minilang): C2 as an error test (constructor and pattern positions,
  both with the new message); C3 now `2` *because of* the re-export
  (plus multi-hop `a → b → c → entry` and single-constructor re-export, and a
  check that `export Tok;` in `b` does not expose `a`'s private constructor),
  while the same file without `export Opt` fails; C4 still `3` and C4s
  still `101` (sibling modules that both privately import `a`); C4q gives
  `3` (its own commit, before the visibility change); an exported macro from
  `b.ml` whose template builds `Som(x)` works in an entry that imports only
  `b.ml`; `b.ml`'s own code still uses `Som` (defining-edge visibility).
- Unchanged: the private-constructor tests
  (`mlite_m3_private_ctor_hidden`, `..._pattern_hidden`,
  `review6_private_ctor_in_pattern_explained`),
  `review6_qualified_adt_prints_ctor_name`, `mlite_m3_expr_eval`,
  `mlite_m3_wordcount`, `mlite_m4_aot_cli`.

---

## 4. Header-less Modules Are in the Base Language

### 4.1 Problem

`import_module_file` starts a module that has no `#lang` header as a copy of
its importer (`new Workbench(*this)`, then `clear_adts`). The copy carries
the importer's language and also everything the importer declared before
the `import`: macros, grammar changes, `for_syntax` helpers, per-form hooks,
`check`s. Each module is expanded once and cached (`imported_cache`), so it
keeps the declarations of whichever importer reaches it first. In H1 the
entry's own `use_t(1)` gives `1001` or `2` depending on the order of two
`import`s, because `m.ml`'s `twice` macro rewrites `tw.ml` only when `m.ml`
imports it first. In H2 an entry macro rewrites a library's private call.
The LSP cannot analyze such a module by itself, since its meaning depends on
who imports it. This also contradicts §2: the entry's hook no longer reaches
imported code, but the entry's macros would.

### 4.2 Design

Exports are already explicit, and §3 makes constructors follow them. The
same rule applied in the other direction is that a module's `#lang` header,
or its absence, is its explicit import of a language. A header-less module
is in the **base language**: the root workbench before any `#lang` or
declaration (`lang_base_`, which `#lang` modules already start from, FL-12),
whoever imports it. In `import_module_file`, `mod_wb` starts from
`lang_base_ ? *lang_base_ : *this` instead of `*this` (preserving
`own_lang_base_` ownership and `mod_wb.prims_.clear_adts()`, so ADTs declared
on a root REPL workbench are not treated as defined by the imported module). A
session clone's `lang_base_` is the root, so this is the state before the
entry's declarations. Under `langwb --bare`, `lang_base_` has no grammar
rules, so every module imported under `--bare` must carry `#lang prelude` (or
its own `#lang`), matching `--bare` entry files. This breaks H4, which works
today, and the prototype's error is unlocated and does not name the module:
`langwb: error: grammar error: no productions in grammar '-'`. M3 therefore
adds a check in `import_module_file`: a header-less module whose base
language has no grammar gets an error located at the module (line 1)
saying so, e.g. `inc.ml:1:0: error: module has no '#lang' header and the
base language has no grammar (add '#lang prelude')`.

This is Racket's rule (every module names its language). It makes a
module's expansion independent of its importers, so `imported_cache` is
correct by construction, and the LSP can evaluate any module file alone.

The cost: a fragment written in a non-base language must carry `#lang <name>`.
All four `examples/mlite/std/*.mlite` files already say `#lang mlite`. Across
all 520+ tests in `tests/run_tests.sh`, the only test whose header-less module
relies on its importer's `#lang` or declarations is
`import_module_without_lang_header_inherits_importer_lang` (H3);
every other header-less module in the suite is base minilang imported from
base minilang. No fallback to `lang_snapshot` is needed.

A root workbench changed in place (`--repl`, embedders) keeps its current
base: `load_lang` snapshots it before the first `#lang` (`own_lang_base_`),
and REPL declarations made on the root itself before any `#lang` are not
separated out here.

### 4.3 Tests

- H1 → `2` in both import orders; H2 → `2`.
- `import_module_without_lang_header_inherits_importer_lang` (H3) → error
  test `import_module_without_lang_header_uses_base_lang` (syntax error on
  `|>`, located in `inherit_lang_mod.ml`). The comment above it in
  `tests/run_tests.sh` ("Header-less modules still inherit the importer's
  language") is rewritten to say they are in the base language.
- H4 → error test with the located message above; the same program with
  `#lang prelude` in `inc.ml` gives `2`.
- New (minilang): a header-less module using a macro its importer declares
  before the `import` now fails with the module's own error (`unbound
  identifier` or a syntax error, located in the module); the same module
  with the macro declared inside it works.
- A header-less module imported from a `#lang mlite` entry is parsed and
  Stage-0-expanded as minilang (a pure minilang helper `def inc = fn(x) =>
  x + 1; export inc;` works when imported into `#lang mlite`, while an
  MLite-only form in a header-less module is a syntax error located in that
  module).
- Unchanged: every `#lang` module test (FL-12), the MLite examples and
  `std/`.

---

## 5. Out of Scope

Each item below was considered and left for a concrete need. Each is
recorded in [ROADMAP.md](ROADMAP.md) *Deferred* when the step closes.

- **Per-module `module_begin`** (§2.2). This needs definitions that escape
  the hook's wrapper.
- **Module-local and cross-`#lang` `prim`, `stage1_macro`, and `check`
  declarations.** Stage-1 expansion, static checks, and `PrimRegistry`
  handlers run once on the entry `session` and are not forwarded by
  `apply_exports` (`import_module_file` forwards Stage-0 macros, `syntax_ext`,
  grammar ops, ADTs, and `def` aliases). Consequently, a `prim`,
  `stage1_macro`, or `check` declared inside an imported module — or by an
  imported module's `#lang` when that `#lang` differs from the entry's (e.g.
  importing a `#lang mlite` module that calls `abs_i` or `fast_pow` from a
  minilang entry) — is not present in `session`. Supporting this means
  merging/scoping Stage-1 macros, checks, and foreign primitives across
  modules; no current program needs it.
- **Same-named private constructors in two modules** (C5). Variants are
  looked up by name (`PrimRegistry::find_variant`, a dozen call sites), so two
  modules cannot both declare `Leaf`, even privately. With import-edge
  visibility the conflict could be limited to constructors visible in the
  same scope, but that means resolving constructors by binding, not by name.
- **Re-exporting functions** (C6). `export f;` of an imported `f` warns
  "not defined". Supporting it means forwarding the module's own
  `import_alias` to its importers. That is symmetric with §3's type
  re-export, but nothing needs it yet.
- **Selective and qualified imports** (`import "m" (f, g);`, `m.f`).
  `import` stays open: it brings every exported name, unqualified, with the
  existing ambiguity warning. A selective form helps readers and protects an
  importer from a module that adds an export, but grammar exports are all or
  nothing and constructors are needed in patterns, so the design needs a
  use case. Racket's `only-in` / `prefix-in` and SML's structure
  qualification are the models.
- **Entry-module and `.lang` ADTs stay public.** An imported module could in
  principle name a constructor the entry declares (lowering sees one
  registry). No program does, and modules have no way to import the entry.

---

## 6. Milestones

| Milestone | Scope | Exit criterion |
|---|---|---|
| **M0** ✅ | Baseline tests for A1–A8, H1–H2, H4, C2–C4, C4s and C4q as they behave today, in `tests/run_tests.sh` (A1, H3 and C1 are already pinned). | All tests pass; §1.3 matches the pinned output. |
| **M1** ✅ | FL-18 (§2): two statement lists in `eval_impl`, `module_begin` on the entry's own forms only. | §2.3: A1, A2 and A7 become errors; A4–A6 and A8 change as described in §2.1; A3 and all other tests unchanged. Change log in §7. |
| **M2** | First the C4q fix (its own commit), then FL-15 (§3): `visible_in`, `visible_to`, re-export, new message. | §3.3: C4q gives `3`; C1/C2 become errors, C3 works because of the re-export, C4 and C4s unchanged. Every MLite example and stdlib file unchanged. Change log in §7. |
| **M3** | Header-less modules in the base language (§4): `mod_wb` from `lang_base_`; H3 (`import_module_without_lang_header_inherits_importer_lang`) flipped to an error test (and its comment updated); the located `--bare` diagnostic. | §4.3: H1 and H2 give `2`; H3 becomes a located syntax error; H4 becomes the located `#lang` error; all other existing tests unchanged. Change log in §7. |
| **M4** | Close the step: §5 items into ROADMAP *Deferred*, FL-15 / FL-18 marked fixed in STEP11_DOGFOOD_LANG.md, ROADMAP Step 12.5 marked done. | Docs updated. |

M1, M2 and M3 are independent; M1 first because it is smaller and touches
only `eval_impl`, and M3 last because it may touch the most tests. Each fix is one commit with its minilang test.

---

## 7. Change Log (filled in per milestone)

### M0 (baselines)

18 tests added to `tests/run_tests.sh` (section "Step 12.5 M0"), pinning
§1.3 as it behaves today; files under `$AOT_TMP_DIR/s125`. Suite: 527 → 545,
all passing. No expectation changed.

| Row | Test | Pinned |
|---|---|---|
| A2 | `s125_m0_a2_hook_reaches_headerless_import` | `42` |
| A3 | `s125_m0_a3_hook_import_after_own_form` | `42` |
| A4 | `s125_m0_a4_import_side_effect_in_place` | `ownlib`, `1` |
| A5 | `s125_m0_a5_later_import_shadows_def` / `s125_m0_a5_later_def_shadows_import` | `11` / `1` |
| A6 | `s125_m0_a6_use_before_import` | error: `'lib_w' is used before assignment on every path` |
| A7 | `s125_m0_a7_import_only_program` | `<fn>` |
| A8 | `s125_m0_a8_hook_reexpands_imports` / `s125_m0_a8_no_hook` | `1001` / `2` |
| H1 | `s125_m0_h1_first_importer_macro_wins` / `s125_m0_h1_other_order` | `1001` / `2` |
| H2 | `s125_m0_h2_entry_macro_reaches_headerless` | `1001` |
| H4 | `s125_m0_h4_bare_headerless_import` | `2` |
| C2 | `s125_m0_c2_ctor_visible_transitively` | `1` |
| C3 | `s125_m0_c3_reexport_accepted` | `2` |
| C4 | `s125_m0_c4_diamond` | `3` |
| C4s | `s125_m0_c4s_siblings` | `101` |
| C4q | `s125_m0_c4q_collision_diamond` | error: `variant 'Non' imported from 'a.ml' conflicts with existing variant of type 'a::Opt'` |

A1, C1 and H3 were already pinned. C5 and C6 (unchanged by this step) are
not pinned.

### M1 (FL-18: `module_begin` wraps only the entry's own forms)

`eval_impl` keeps `imported_stmts` and `own_stmts`. The program is
`program`-`Seq(imported_stmts..., module_begin(module-Seq(own_stmts...)))`,
or the wrapped module alone when nothing is imported; only the wrapped part
is re-expanded when the hook rewrites it. The fast path needs both lists
empty, and the empty-program check is on `own_stmts`. Suite: 546 passed.

| Test | Before | After |
|---|---|---|
| `mlite_m4_module_begin_reaches_imports_fl18` → `mlite_m4_module_begin_entry_only_fl18` | `4` | error: `unbound identifier 'mlite_version'` (in `mv.mlite`) |
| `s125_m0_a2_hook_reaches_headerless_import` → `s125_m1_a2_hook_does_not_reach_import` | `42` | error: `lib.ml:1:21: error: unbound identifier 'magic'` |
| new: `s125_m1_a2_uncalled_import_still_resolved` | — | the same error when `lib_v` is never called |
| `s125_m0_a4_import_side_effect_in_place` → `s125_m1_a4_imports_run_first` | `ownlib`, `1` | `libown`, `1` |
| `s125_m0_a5_later_import_shadows_def` → `s125_m1_a5_def_wins_over_later_import` | `11` | `1` |
| `s125_m0_a6_use_before_import` → `s125_m1_a6_use_before_import` | error: `'lib_w' is used before assignment on every path` | `11` |
| `s125_m0_a7_import_only_program` → `s125_m1_a7_import_only_program` | `<fn>` | error: `expected expression after declarations` |
| `s125_m0_a8_hook_reexpands_imports` → `s125_m1_a8_hook_does_not_reexpand_imports` | `1001` | `2` |

Unchanged: A3 (`42`), `s125_m0_a5_later_def_shadows_import` (`1`),
`s125_m0_a8_no_hook` (`2`), every other `module_begin` test, and every
MLite example.

Found along the way: positions in an imported `#lang` module are one line
early (the header line is not counted), e.g. the `mlite_version` error above
is reported at `mv.mlite:1:11` for line 2. Fixed separately (next entry).

### Fix: positions in imported `#lang` modules

`import_module_file` started counting lines after the `#lang` header as
line 1; it now advances past the header as `eval_impl` does for the entry.
Test `import_lang_module_positions_count_header` (a module in a `.lang` that
adds nothing: `hdr.ml:2:17` before, `hdr.ml:3:17` after). Suite: 547 passed.

| Test | Before | After |
|---|---|---|
| `mlite_m2_diag_swapped_args_b2` | `list.mlite:7:2` error, notes at `list.mlite:19:2`, `12:16` | `list.mlite:8:2`, `20:2`, `13:16` (the `case` is on line 8) |
| `mlite_m5_lsp_hover_definition` (definition of `fold_l`) | `D 5:4 list.mlite` (0-based: the blank line 6) | `D 6:4 list.mlite` (line 7, `fun fold_l`) |
| `mlite_m4_module_begin_entry_only_fl18` | `mv.mlite:1:11` | `mv.mlite:2:11` |
