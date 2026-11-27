# `langwb` — Programming Language Workbench

`langwb` is a **C++23 programming language workbench** for experiments and investigations in programming languages, built on top of:

- **[DParser](dparser/)** — scannerless GLR parsing with in-memory grammar compilation and binary table linking (`mkdparse_from_string` → `write_binary_tables_to_string` → `read_binary_tables_from_string`).
- **[IFA](ifa/)** — whole-program iterative flow analysis, type-directed cloning/monomorphisation, optimization, and C / LLVM / ORCv2 LLJIT code generation.

For the full architectural overview and design decisions, see [docs/SKETCH.md](docs/SKETCH.md), for completed milestones and next steps (Steps 12–13), see [docs/ROADMAP.md](docs/ROADMAP.md), and for the Step 11 dogfood language (MLite) and its friction log, see [docs/STEP11_DOGFOOD_LANG.md](docs/STEP11_DOGFOOD_LANG.md).

---

## Features

- **Extensible In-Memory Grammars (`src/parse/`)**
  - Structured `GrammarDef` IR for composing grammar fragments (`add_rule`, `prepend_rule`, `extend`) and compiling them in memory with zero disk I/O or `dlopen`.
  - Content-addressed `GrammarCache` for compiled `DynamicGrammar` tables.
  - Parsing from any non-terminal (`start_state`) for embedded sublanguages and grammar-aware macro templates.
- **Hygienic Procedural Macros (`src/syntax/`, `src/macro/`)**
  - Racket-style **sets-of-scopes** hygiene (`SyntaxObj`, `ScopeSet`, `BindingEnv<T>`) with Flatt's flip-on-intro-scope algorithm.
  - Both **form macros** (keyed by dynamically added grammar non-terminals) and **call macros** (`ident(args...)`).
  - Grammar-aware quasiquoting (`MacroContext::parse_template`) and explicit opt-out escapes (`MacroContext::unhygienic_ident`) for anaphoric/unhygienic macros.
- **Frontend-Injected Primitives & Transfer Functions (`src/ifa_bridge/prim_registry.*`)**
  - `PrimRegistry` wraps IFA's `prim_reg` with C++23 `std::function` handlers (`transfer`, `check`, `emit_c`, `emit_llvm`) that persist across `ifa_reset()`.
  - Debug-mode monotonicity checker for custom abstract-interpretation transfer functions.
  - Inline LLVM IR emission via `RegisteredPrim::llvm_cgfn` + `llvm_emit_get_var` / `llvm_emit_put_result`.
- **Analysis-Driven Metaprogramming (`src/ifa_bridge/attrs.h`, `src/workbench/`)**
  - Staged pipeline: `Parse → Stage-0 Macros → IFA Analyze → AnalysisAttrs → Stage-1 Attribute-Driven Macros → Final IFA + LLVM LLJIT`.
  - Stage-1 macros can query IFA's inferred types, compile-time constants, and record field layouts (even through unannotated polymorphic functions) to specialize, unroll, or reflect over code.
- **In-Language Compile-Time Execution (`comptime(...)`) & Zero-C++ `#lang` Modules**
  - Evaluates guest-language expressions at compile time in-process via IFA + LLVM LLJIT.
  - Complete languages can be defined purely inside `.lang` modules (`#lang <name>`) with zero C++ using `grammar`, `for_syntax`, `syntax_rule` (declarative pattern macros with DParser syntax classes `$x:expr`/`$id:ident`/`$n:int`), `macro`/`form_macro`/`stage1_macro`, syntax properties (`stx_set_prop`/`stx_get_prop`/`stx_has_prop`), `stx_local_expand`, `prim ... c_call ... transfer => ...`, and Racket-style core interposition hooks (`hook module_begin`, `hook datum`, `hook app`, `hook field`, `hook record`).

---

## Building & Testing

### Prerequisites
- `clang++` with C++23 support
- Boehm GC (`bdw-gc`: `-lgc -lgccpp`)
- LLVM (with `llvm-config` in `PATH` or `/opt/homebrew/opt/llvm/bin/llvm-config`)
- GNU `make`

### Build & Test Commands
```bash
make -j8            # builds dparser/, ifa/, and ./langwb
make test           # runs ./langwb --smoke, examples/minilang/*.ml, and tests/run_tests.sh
make clean          # removes langwb objects and binary
make distclean      # also cleans dparser/ and ifa/
```

### Subtree Sync Targets
`ifa/` and `dparser/` are managed as git subtrees (`ifa-remote`, `dparser-remote`):
```bash
make pull-ifa       # git subtree pull --prefix=ifa ifa-remote main --squash
make push-ifa       # git subtree push --prefix=ifa ifa-remote main
make pull-dparser   # git subtree pull --prefix=dparser dparser-remote main --squash
make push-dparser   # git subtree push --prefix=dparser dparser-remote main
```

---

## Usage

```bash
# Run the full Phases 0–4 + Zero-C++ smoke test suite (31 cases)
./langwb --smoke

# Evaluate an expression directly
./langwb -e "let x = 7 in clamp(x * 2, 1, 10) + clamp(3, 1, 10)"

# Run a minilang file
./langwb examples/minilang/reflect.ml

# Compile a file AOT to a standalone native executable (or emit LLVM IR)
./langwb build examples/minilang/step6_demo.ml -o step6_demo && ./step6_demo
./langwb build examples/minilang/hello.ml -o hello.ll --emit-llvm

# Start the Language Server (JSON-RPC 2.0 over stdio: diagnostics, hover, go-to-definition)
./langwb --lsp

# Inspect CST, expanded hygienic SyntaxObj, or IFA AnalysisAttrs
./langwb --dump-cst -e "1 + 2 * 3"
./langwb --dump-expanded examples/minilang/reflect.ml
./langwb --dump-attrs examples/minilang/reflect.ml

# Start the interactive REPL
./langwb --repl
```

### REPL Commands
- `<expr>` — evaluate expression through the staged pipeline and print `=> <value>`
- `:cst <expr>` — dump the DParser CST before evaluation
- `:expand <expr>` — dump the Stage-1 expanded `SyntaxObj` with scope sets
- `:attrs <expr>` — dump IFA's inferred `AnalysisAttrs` (types, constants, record fields)
- `:quit` / `:q` — exit

### Debug Environment Variables
- `LANGWB_TRACE=1` — print each smoke test case as it runs
- `LANGWB_DUMP_IF1=1` — dump generated IF1 and verbose IFA diagnostics

---

## Examples (`minilang`)

`langwb` ships with an expression-oriented experimental core language (`minilang`) featuring `let` bindings, `if/then/else`, lambdas/closures (`fn(a, b) => ...`), records (`{ x: 1, y: 2 }`) and field access (`p.x`), dynamic syntax extensions (`unless`, anaphoric `aif`), custom primitives (`clamp`, `runtime_val`), `comptime(...)`, and Stage-1 attribute-driven macros (`pow_opt`, `if_const`, `sum_fields`).

### 1. Custom Primitive with Transfer Function & Inline LLVM Emitter ([`examples/minilang/hello.ml`](examples/minilang/hello.ml))
```text
let x = 7 in
clamp(x * 2, 1, 10) + clamp(3, 1, 10)
```
```bash
$ ./langwb examples/minilang/hello.ml
13
```
`clamp` folds constant arguments inside IFA's `AType` lattice and emits inline LLVM `icmp`/`select` instructions with no runtime call.

### 2. IFA-Driven Compile-Time Record Reflection ([`examples/minilang/reflect.ml`](examples/minilang/reflect.ml))
```text
let id = fn(v) => v in
let p = id({ x: 10, y: 20, z: 12 }) in
sum_fields(p)
```
```bash
$ ./langwb examples/minilang/reflect.ml
42
```
IFA infers `p`'s concrete `Type_RECORD` fields `{x, y, z}` through the unannotated polymorphic function `id`. Stage-1 `sum_fields(p)` queries `AnalysisAttrs` and hygienically synthesizes `let r = p in r.x + r.y + r.z`.

### 3. In-Language `comptime(...)` Feeding Stage-1 Specialization ([`examples/minilang/comptime.ml`](examples/minilang/comptime.ml))
```text
let n = comptime(let sq = fn(x) => x * x in sq(3) - 6) in
pow_opt(2, n)
```
```bash
$ ./langwb examples/minilang/comptime.ml
8
```
`comptime(...)` compiles and runs `sq(3) - 6` at compile time via IFA + LLVM LLJIT (`3`), IFA propagates `n == 3`, and Stage-1 `pow_opt` unrolls `pow_opt(2, n)` into `let b = 2 in b * b * b`.

### 4. Hygienic & Anaphoric Macros
```bash
# Hygienic macro: macro-introduced `let tmp = 7` does NOT capture caller's `tmp = 100`
$ ./langwb -e "let tmp = 100 in with_tmp(7, tmp + 1)"
108

# Dynamically extended syntax form: `unless`
$ ./langwb -e "unless 1 > 2 then 42 else 99"
42

# Explicit hygiene opt-out (anaphoric `aif` binds `it` in caller scope)
$ ./langwb -e "aif 10 + 5 then it * 2 else 0"
30
```

### 5. Zero-C++ `#lang` Modules & Interposition Hooks ([`examples/langs/mydsl.lang`](examples/langs/mydsl.lang), [`examples/minilang/use_mydsl.ml`](examples/minilang/use_mydsl.ml), [`examples/minilang/use_scaled.ml`](examples/minilang/use_scaled.ml))
```text
#lang mydsl
when my_abs(0 - 5) > 2 do
  pick_const(magic + 1, triple(magic + 4), 0)
otherwise
  999
```
```bash
$ ./langwb examples/minilang/use_mydsl.ml
42

$ ./langwb examples/minilang/use_scaled.ml   # `#lang scaled` interposes on `#%datum` (x10)
50

$ ./langwb examples/minilang/use_racket_demo.ml  # `for_syntax`, `syntax_rule`, `stx_local_expand`, syntax properties
42
```
`mydsl.lang` defines its own grammar rule (`when..do..otherwise`), hygienic form/call macros (`when_expr`, `triple`), IFA-driven Stage-1 macro (`pick_const`), foreign C primitive + custom IFA transfer function (`my_abs` → libc `labs`), and `#%module-begin` prelude hook (`magic = 10`) — all in `langwb` with zero C++. `racket_demo.lang` demonstrates `for_syntax` shared Phase-1 helpers, declarative `syntax_rule` pattern macros with DParser syntax classes (`$x:expr`), and Turnstile-style cooperative macros using `stx_local_expand` + `stx_set_prop`/`stx_get_prop`/`stx_has_prop`.

`#lang <name>` is looked up as `<name>.lang` in the source file's directory (and its `langs/`), the working directory (`.`, `langs/`, `examples/langs/`), each `LANGWB_LANG_PATH` entry (`:`-separated), and `langs/` / `examples/langs/` next to the `langwb` executable. `import "path";` resolves relative to the importing file first.

### 6. MLite: a small ML written entirely in `.lang` ([`examples/langs/mlite.lang`](examples/langs/mlite.lang), [`examples/mlite/`](examples/mlite/))
```text
#lang mlite
import "std/result.mlite";

type Expr = Lit(n) | Add(lhs, rhs) | Div(lhs, rhs);

fun eval_expr(e) =
  case e of
    Lit(n)    => Ok(n)
  | Add(l, r) => eval_expr(l) |> and_then(fn (lv) => eval_expr(r) |> map_ok(fn (rv) => lv + rv))
  | Div(l, r) => eval_expr(r) |> and_then(fn (rv) => eval_expr(l) |> and_then(fn (lv) => safe_div(lv, rv)))
  end;

Add(Lit(40), Div(Lit(6), Lit(3))) |> eval_expr |> unwrap_ok_or(0)
```
```bash
$ ./langwb examples/mlite/expr_eval.mlite                         # JIT
$ ./langwb build examples/mlite/aot_cli.mlite -o aot_cli && ./aot_cli   # standalone binary
```
MLite (the Step 11 dogfood language, [docs/STEP11_DOGFOOD_LANG.md](docs/STEP11_DOGFOOD_LANG.md)) replaces minilang's grammar and adds `val`/`var`/`fun`/`fn`, `let ... in ... end`, `case ... of ... end`, `while`/`for`/`do`/`unless`, `|>`, `and`/`or`/`not`, a Stage-1 `fast_pow`, `check call safe_div`/`assert`, C prims, a `module_begin` hook, and `using syntax power in { a ** b }`, with a multi-file stdlib under `examples/mlite/std/`.

---

## Status & Limitations

`langwb` is an active research prototype. Current limitations being addressed on the roadmap (see [docs/ROADMAP.md](docs/ROADMAP.md) and [reviews/2026-11-22-review.md](reviews/2026-11-22-review.md)):
- **Guest language:** Top-level evaluation (`eval_value`) and AOT binaries (sharing the unified `src/runtime/langwb_rt.c` runtime) print `int64`, `bool` (`true`/`false`), `string` (with `\n`/`\r`/`\t`/`\"`/`\\` escaping), runtime-tagged heterogeneous/nested `list` (`[1, "two", true, [3, 4]]`), `record`, ADT variant, and `<fn>` values. Supported features include `true`/`false` literals, `rec` recursion, `{ def ...; ... }` blocks with mutual recursion, lexical closure capture (including nested/curried closures, record-stored closures, and per-`def` call-graph capture fixpoints), local assignment (`x = e`), mutable record fields (`p.x = e`), `while cond do body` loops, `print`/`println` I/O, GC-allocated strings (`str_concat`, `str_len`, `str_eq`, `str_slice`, `int_to_str`, `str_to_int`), runtime-tagged lists (`head`, `head_str`, `head_bool`, `head_list`, `head_tag`), algebraic data types (`type Option = None | Some(val);` with per-variant IFA `CreationSet`s and branch-narrowing `match`), multi-file `import`/`export` modules (per-module hygiene scopes, explicit exports, single instantiation), grammar-checked quasiquote (`` `[expr| ... ,(x) ... ] ``), `attr_field_name`, `stx_is_core`, `stx_binding_eq`, and per-contour `attr_contour_*` queries. Known limits: `match` does not yet support nested patterns, negative/string literals, or dynamic record-shape testing (record patterns must match statically), and `head([])` returns `0`.
- **Declaration parsing & scoped syntax:** `grammar` declarations use native DParser `.g` syntax (`+=` extend, `:` define new, `:=` replace, `|` alternatives, and `grammar { ... }` blocks — legacy outer-quoted `"..."` RHS strings are rejected). Declaration bodies (`macro`, `hook`, `for_syntax`, `check [core|call]`, `prim ... transfer`, and unquoted `syntax_rule` templates) are parsed eagerly at declaration time with the active guest grammar (locking syntax/precedence and reporting syntax errors at source `L:C`), while expansion and LLJIT compilation remain lazy. Mid-file declarations, hygienic helper non-terminals (`grammar_add_rule`), eagerly compiled lexically scoped syntax regions (`syntax_ext` / `using syntax <ext> in { ... }`), and GLR `Amb` nodes (including nested ambiguities) with linear 3-tier disambiguation are implemented.
- **Higher-order contour cloning & static checks:** Decision-keyed syntactic cloning specializes `let`, `def`-block, and record-stored functions across mixed-arity record contours, ADT variants, and bounded re-analyzing constant splits; `.lang` modules can declare custom Stage-1 static checks (`check [core|call] <target> => ...;` + `stx_error`), which run on the Stage-0 expanded tree under `opts.staged`.
- **Rules to know:**
  - **Constructor patterns list every field.** A `match` arm on a variant must have one sub-pattern per field. `Some => 1` is rejected with `pattern 'Some' has 0 field(s); variant 'Some' has 1`. Write `Some(_) => 1` to ignore the payload. Nullary variants are written bare (`None => 0`).
  - **Constructors are module-private unless exported.** An importer sees a module's constructors only if the module exports the type name (`export Option;`, which exports all its variants) or the constructor names (`export Some, None;`). Otherwise the importer cannot call them or match on them (`constructor 'Some' of type 'Option' is not exported by module 'lib.ml'`). The module's own code and its exported functions still use them, and their values still print. When two modules define types with the same name, the later one is renamed `<module>::<Type>` inside IFA, so the variants of both stay distinct; values still print by constructor name (`Algo(2)`).
  - **`export grammar` exports the module's syntax.** It merges the module's `grammar` rules into the importer, together with the form macros (`syntax_rule form`, `macro form`) that expand those productions. Diagnostics from an imported module are prefixed with its path (`lib.ml:3:2: unbound identifier 'nope'`); the LSP shows them at the top of the importing document. Macros declared in guest code (`syntax_rule`, `macro`, `stage1_macro`, and those inside a `syntax_ext`) are hygienic: identifiers in their templates refer to the top-level `def`s of the file that declares them, whether the macro is used in that file or exported, even when the use site shadows the same names.
  - **Patterns.** In a pattern, a name starting with an uppercase letter is always a constructor (an unknown one is an error, never a new binding); lowercase names bind. Patterns are one level deep: `Some(v)` and `Some(_)` work, nested patterns such as `Some(Some(v))` are a syntax error (match the inner value in the arm body).
  - **`==` on constructors.** Two values of the same ADT can be compared when at least one side can only be a nullary constructor: `x == None`, `c == Red`, `None == None`. That compares variants. Comparing two values that may both carry fields (`Some(1) == Some(1)`) is rejected; use `match`. `==` on other types requires both sides to be `int64`, both `bool`, or both `string`.
  - **ADT values are mutable records.** Variant fields can be assigned (`o.val = 5` turns `Some(1)` into `Some(5)`). The synthesized `_tag` field cannot (`cannot assign to '_tag'`).
  - **`import` is a top-level declaration.** It is written `import "path";` at the top level of a file; elsewhere (for example inside `{ ... }`) it is a syntax error with that explanation. A module's top-level expressions run once, when it is first imported. A program whose last form is an `import` evaluates to that module's last expression.
  - **String conversions do not report errors.** `str_to_int` parses a leading integer and ignores the rest (`str_to_int("12abc")` is `12`, `str_to_int("x")` is `0`), and `str_slice` clamps its bounds to the string.
  - **Printing recursive values.** Records and ADT values print to any depth. A value that contains itself prints `<cycle>` where it recurs (`Cons(1, <cycle>)`), a shared but acyclic value prints in full at each use, and nesting deeper than 1000 levels ends in `...`. A field that holds both an `int64` and a record (`r.n = r` after `{n: 0}`) has no runtime tag and prints `<union>`.
  - **Field assignment goes through `field_lhs`.** `obj.f = e` is parsed by the core non-terminal `field_lhs`, which covers identifiers, `( expr )`, field chains (`a.b.c`) and calls (`f(x).y`). A grammar that adds a new postfix form to `expr`, such as indexing `a[i]`, must also add it to `field_lhs` (`grammar field_lhs += ...`) before `a[i].f = e` parses.

---

## Repository Layout

```text
langwb/
├── Makefile
├── README.md
├── docs/
│   ├── SKETCH.md              # Architecture & resolved design decisions
│   ├── ROADMAP.md             # Completed milestones & next steps (Steps 12–13)
│   ├── STEP11_DOGFOOD_LANG.md # Step 11 dogfood language (MLite) & friction log
│   └── STEP12_DIAGNOSTICS.md  # Step 12 plan: diagnostics in the language author's terms
├── dparser/                   # git subtree (scannerless GLR parser)
├── ifa/                       # git subtree (iterative flow analysis + C/LLVM/LLJIT)
├── src/
│   ├── parse/                 # DynamicGrammar, CstNode, GrammarDef IR, GrammarCache
│   ├── syntax/                # SyntaxObj, ScopeSet, ScopeAllocator, BindingEnv, syntax properties
│   ├── macro/                 # MacroContext, hygienic MacroExpander, local_expand, interposition hooks
│   ├── ifa_bridge/            # WbCallbacks, PrimRegistry, ADTs, Meta-ABI, AnalysisAttrs, IF1 lowering & LLJIT
│   ├── runtime/               # Unified C runtime (langwb_rt.c -> liblangwb_rt.a) for JIT & AOT
│   ├── workbench/             # Workbench runtime (minilang, #lang/import loader, comptime, contour cloning, LSP)
│   └── cli/                   # CLI driver, AOT build, file runner, REPL, LSP, smoke test suite
├── examples/
│   ├── langs/                 # Zero-C++ #lang language modules (prelude, mydsl, scaled, racket_demo, mlite)
│   ├── minilang/              # Runnable minilang & #lang example programs
│   └── mlite/                 # MLite target programs (+ .expected) and stdlib (std/)
└── tests/
```
