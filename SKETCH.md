# `langwb` — Programming Language Workbench Sketch

A project sketch for **`langwb`**, a programming language workbench for experiments and investigations in programming languages, built in **C++23** on top of **[DParser](../dparser)** (scannerless GLR front end) and **[IFA](../pyc/ifa)** (iterative flow analysis, monomorphisation, and C/LLVM/JIT back end, as used in **[pyc](../pyc)**).

---

## 1. Vision & Goals

`langwb` is designed as a laboratory for language design where syntax, static analysis, metaprogramming, and code generation are **composable and mutually informing**, rather than rigid, one-way compiler passes:

1. **Extensible, scannerless GLR grammars** — languages and embedded DSLs defined and extended dynamically using DParser's in-memory grammar compilation and table linking.
2. **Syntax-directed systems & hygienic procedural macros** — composable syntax transformations with hygiene preserved across dynamically extended grammars.
3. **Whole-program flow analysis as a first-class service** — IFA infers concrete types, constants, call graphs, contours, liveness, and definite assignment without requiring pervasive type annotations.
4. **Frontend-injected primitives & transfer functions** — a language definition can inject new operations, custom abstract-interpretation transfer functions, type checks, and C/LLVM codegen emitters directly into IFA.
5. **Analysis-driven metaprogramming** — attributes produced by IFA (types, constants, call targets, contours, liveness, purity, execution frequency) can control compile-time metaprogramming, code specialization, and AST/IF1 rewrites.
6. **Multiple execution tiers** — C codegen, LLVM AOT, and LLVM LLJIT (`llvm_jit_execute`) for fast iteration and in-process compile-time execution.

---

## 2. Foundation: C++23 + IFA + DParser

Building `langwb` in **C++23** (matching `../pyc/ifa`'s `-std=c++23` toolchain) eliminates any FFI or GC boundary between the workbench, IFA, LLVM, and DParser:

### 2.1 Direct Integration with IFA (`../pyc/ifa`)
- **Native customization hooks:** Directly subclass `IFACallbacks`, `IFASymbol`, and `IFAAST` (`ifa/ifa.h`) with zero marshaling overhead.
- **Direct access to IFA & LLVM internals:** Custom transfer functions (`prim_reg`), `check_fn` validators, analysis-attribute queries, and `llvm_cgfn` emitters can directly read `FA`, `AType`, `CreationSet`, `EntrySet`, `AVar`, `PNode`, `Sym`, `Fun`, and emit into IFA's `llvm::IRBuilder<>` / `llvm::Module`.
- **Unified Boehm GC memory model:** IFA has a hard dependency on Boehm GC (`-DUSE_GC -lgc -lgccpp`). Workbench IR nodes (`SyntaxObj`, CST/AST nodes, custom `IFAAST`/`IFASymbol` subclasses) can inherit from `gc` (just like `pyc`), avoiding manual rooting or handle-table indirection while keeping modern C++23 value types (`std::string_view`, `std::span`, `std::expected`, `std::variant`) for non-GC logic.
- **Process-wide globals & `ifa_reset()`:**
  - IFA currently keeps process-wide state (`if1`, `pdb`, `fa`, canonicalization tables in `fa.cc`, LLVM `TheContext`/`TheModule`/`Builder`, and per-`Fun` `f->llvm`).
  - `ifa_reset()` (`ifa/ifa.h:205`) tears down all process-wide IFA state so a single process can run multiple isolated analyses **sequentially** (e.g., compile/JIT a macro stage → `ifa_reset()` → analyze target program → rewrite → `ifa_reset()` → re-analyze).
  - True *nested* or *concurrent* IFA sessions would require either a worker subprocess or refactoring IFA's globals into an explicit `IFAContext`.
- **Clean library boundary:** `ifa/Makefile` currently compiles `ifa/frontend/` (the legacy V frontend) into `libifa_gc.a` and links `-ldparse_gc`. Splitting a `libifa_core.a` target (without `ifa/frontend/`) or linking both `ifa` and `langwb` against the same `libdparse_gc` keeps the link clean and avoids duplicate symbols.

### 2.2 In-Memory Dynamic Grammars with DParser (`../dparser`)
DParser already provides everything needed for dynamic, in-memory grammar construction without disk I/O or `dlopen`:

1. `mkdparse_from_string(Grammar *g, char *str)` (`mkdparse.h`) parses a grammar specification from memory and builds the GLR state machine (`parse_grammar` + `build_grammar`).
2. `write_binary_tables_to_string(Grammar *g, unsigned char **buf, unsigned int *len)` (`write_tables.h`) serializes `D_ParserTables` into an in-memory relocatable byte buffer.
3. `read_binary_tables_from_string(buf, len, spec_code, final_code)` (`read_binary.h`) relocates the tables in memory and binds unified speculative/final reduction callbacks that dispatch on `D_Reduction::action_index` into C++ handlers.
4. Full GLR feature set out of the box: state-specific symbol tables (`D_Scope` / `dsymtab.h`), speculative actions, partial parses, `start_state` (parsing starting at any non-terminal for embedded DSLs and quasiquotes), whitespace subgrammars, ambiguity callbacks, and error recovery.
5. Linking `libdparse_gc.a` (built with `D_USE_GC=1`) aligns DParser's memory management with IFA's Boehm GC.

---

## 3. Proposed System Architecture

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                         langwb (C++23 Host)                             │
│                                                                         │
│  ┌─────────────────────┐    ┌────────────────────────────────────────┐  │
│  │  Grammar Registry   │───▶│  parse/ (DParser in-memory)            │  │
│  │  • Base grammars    │    │  • mkdparse_from_string → binary buf   │  │
│  │  • Syntax extensions│    │  • read_binary_tables_from_string      │  │
│  │  • Table cache      │    │  • CST / Parse Forest + D_Scope        │  │
│  └─────────────────────┘    └──────────────────┬─────────────────────┘  │
│                                                ▼                        │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Syntax & Metaprogramming Engine (syntax/ + macro/)               │  │
│  │  • SyntaxObj (CST/AST + SourceSpan + ScopeSet for hygiene)        │  │
│  │  • Syntax-directed passes / attribute evaluators                  │  │
│  │  • Hygienic procedural macros (C++ plugins OR IFA+LLVM LLJIT)     │  │
│  └──────────────────────┬──────────────────────────────▲─────────────┘  │
│                         │ lower to IF1                 │ query attrs    │
│                         │ + register prims/tfns        │ (types, consts,│
│                         ▼                              │  contours, ...)│
│  ┌─────────────────────────────────────────────────────┴─────────────┐  │
│  │  ifa_bridge/ (Workbench ↔ IFA Integration)                        │  │
│  │  • WbCallbacks : IFACallbacks, WbSymbol : IFASymbol, WbAST : IFAAST│ │
│  │  • Dynamic primitive & transfer-function registry (prim_reg)      │  │
│  │  • Analysis-attribute query API over FA / AType / CreationSet     │  │
│  └──────────────────────┬────────────────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────────────────┘
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    IFA C++23 Library (libifa_core.a)                    │
│  IF1 IR → CFG/SSU → Iterative Flow Analysis (FA) → Cloning → Optimize   │
│  Backends: C codegen (cg.cc) | LLVM AOT (llvm.cc) | LLVM LLJIT          │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 4. Key Subsystems & Design Details

### 4.1 Extensible Grammars & Syntax-Directed Front End (`src/parse/`)

- **Structured Grammar IR:** Represent grammars in C++ (`GrammarDef`: productions, rules, operator precedence/associativity, whitespace rules, token/regex terminals, and action IDs) rather than ad-hoc string concatenation.
- **Composable syntax extensions:** A language module or macro can extend a `GrammarDef` (adding statements, expressions, custom literals, or operator precedences), render it to DParser grammar syntax, compile it in memory via `mkdparse_from_string` → `write_binary_tables_to_string`, and cache the resulting `BinaryTables*` by content hash.
- **Scoped DSLs & context-sensitive parsing:**
  - Use DParser's `start_state` and `partial_parses` to parse an embedded sublanguage inside a delimited region without rebuilding the host grammar, or compile a merged grammar table in memory when a module imports a syntax extension.
  - Use speculative actions + `D_Scope` when disambiguation depends on lexical bindings (e.g., type names vs. value names, user-defined operator fixity).
- **Syntax-directed translation:** Preserve the parse forest / CST (with source spans and ambiguity nodes when desired) and support multi-pass attribute evaluation (synthesized & inherited attributes) over the tree.

### 4.2 Hygienic Procedural Macros & Compile-Time Metaprogramming (`src/syntax/`, `src/macro/`)

- **Hygiene via Scope Sets:**
  - Represent identifiers and syntax trees as `SyntaxObj`s carrying `(kind, span, scopes: ScopeSet, children)` (Racket's "sets of scopes" model).
  - Every lexical binding site adds a fresh scope; every macro invocation adds a fresh macro-introduction / use-site scope.
  - Name resolution matches a reference to the binding whose scope set is the maximal subset of the reference's scope set. This handles hygienic procedural macros, macro-generating macros, and bindings introduced by dynamic syntax extensions uniformly.
- **Grammar-aware quasiquoting:**
  - Because DParser can parse starting at any non-terminal (`start_state`), quasiquote templates can be parsed with the *active* grammar at the target non-terminal (e.g., `quote(expr) { $x + 1 }`), giving syntax-checked, hygienic AST construction for arbitrary user-defined grammars.
- **Execution engine for compile-time code (two-tier path):**
  1. **Host C++ plugins / builtins (Phase 1):** Procedural macros and syntax-directed passes written directly in C++23 — fast iteration, full access to `SyntaxObj`, `GrammarDef`, and IFA internals.
  2. **In-language compile-time code via IFA + LLVM LLJIT (Phase 2+):** Compile compile-time functions written in the guest language itself through IFA → `llvm_jit_execute`, exposing a small host API for syntax manipulation and attribute queries. This makes the workbench self-extensible.

### 4.3 Injecting Primitives & Transfer Functions into IFA (`src/ifa_bridge/`)

IFA already supports runtime-registered primitives via `prim_reg` (`ifa/if1/prim.h`):

```cpp
RegisteredPrim *prim_reg(cchar *name,
                         PrimitiveTransferFunctionPtr tfn,
                         PrimitiveCGPtr cgfn = 0,
                         PrimitiveLLVMCGPtr llvm_cgfn = 0);
```

along with `RegisteredPrim::check_fn` (custom argument-type violation checks), `is_functional` (DCE eligibility), and `is_visible` (side-effect/root preservation).

- **Stateful / closure-backed primitive handlers:** Since `prim_reg` takes bare C function pointers without a `void*` context, `langwb` registers a single static trampoline (`wb_prim_tfn`, `wb_prim_check`, `wb_prim_cg`, `wb_prim_llvm_cg`) that dispatches on `pn->rvals[1]->sym->name` into a `PrimRegistry` of `std::function` / `PrimitiveHandler` objects:
  ```cpp
  struct PrimitiveHandler {
    std::function<void(PNode*, EntrySet*)> transfer;
    std::function<void(PNode*, EntrySet*)> check;
    std::function<void(FILE*, PNode*, Fun*)> emit_c;
    std::function<void(PNode*, Fun*)> emit_llvm;
    bool is_functional = true;
    bool is_visible = false;
  };
  ```
- **Monotonicity & stability contract:** As noted in `ifa/ifa.h:173-177`, transfer functions participate in IFA's fixed-point worklist (`update_gen` unions rather than replaces), so custom transfer functions must be monotonic with respect to the `AType` lattice. A debug-mode wrapper in `wb_prim_tfn` can assert monotonicity on experimental transfer functions to catch bugs immediately.
- **Declarative / meta-language transfer functions (stretch goal):** Support registering simple transfer functions declaratively (signature rules or guest-language functions over `AType`s/constants) in addition to C++ handlers.

### 4.4 Analysis-Driven Attributes Controlling Metaprogramming

IFA computes whole-program facts that `langwb` surfaces as **queryable attributes** on `WbAST` / `PNode` / `Sym`:

- **Inferred types & type sets:** Monomorphic (`|AType| == 1`) vs. polymorphic, concrete `CreationSet` members.
- **Constants & partial evaluation:** Compile-time known values (`AType::constants()`).
- **Control flow & contours:** Possible callee `Fun`s / `EntrySet`s at a call site, specialization count.
- **Liveness, definite assignment, & frequency:** `Var::live`, definite-assignment status, loop-nested `PNode::execution_frequency`.
- **Custom domain attributes:** Attached by frontend-injected transfer functions or post-analysis CFG/SSU passes.

**Two complementary feedback mechanisms:**

1. **In-loop synthesis (during IFA convergence):**
   - Use `IFACallbacks` hooks (`instantiate_generic`, `promote`, `coerce`, `signature_wrapper`, `reanalyze`, `retract_derived_state`) so the frontend synthesizes new `Fun`s / `Sym`s on demand as IFA discovers types, and retracts speculative decisions if converged types no longer support them.
2. **Staged / iterative macro re-expansion (around IFA passes):**
   ```text
   Parse → Stage-0 Macros (syntax-only) → Lower to IF1 → IFA Analyze
                                                              │
        ┌─── Re-lower & ifa_reset() ◀─── Stage-1 Macros ◀─────┘
        │                                (type/attr-driven rewrites)
        ▼
   IFA Analyze (final) → Clone → Optimize → C / LLVM / JIT
   ```
   - Enables type-directed specialization, DSL lowering/fusion (linear algebra, queries, parser combinators), compile-time reflection (`@fields_of(x)`), automatic differentiation, and check elision.
   - Stratifying into explicit stages (or bounding iterations with a decreasing measure) keeps analysis-driven rewriting terminating and debuggable.

---

## 5. Proposed Repository Layout

```text
langwb/
├── Makefile
├── SKETCH.md                  # This document
├── dparser/                   # git subtree (git@github.com:jplevyak/dparser.git)
├── ifa/                       # git subtree (git@github.com:jplevyak/ifa.git)
├── src/
│   ├── parse/                 # DParser in-memory wrapper, GrammarDef IR, table cache, CST
│   ├── syntax/                # SyntaxObj, ScopeSet hygiene, SourceSpan, quasiquote
│   ├── macro/                 # Macro expander, attribute-driven rewrite driver, LLJIT bridge
│   ├── ifa_bridge/            # WbCallbacks, WbAST, WbSymbol, PrimRegistry, FA attribute queries
│   └── cli/                   # CLI driver, REPL, IR/GraphViz/HTML inspection
├── examples/
│   ├── minilang/              # Minimal dynamically-typed/inferred core language
│   ├── ext_syntax/            # Language with dynamically added grammar rules + hygienic macros
│   └── attr_meta/             # Example where IFA type/constant attrs drive macro expansion
└── tests/                     # Unit & end-to-end language tests
```

---

## 6. Suggested Phased Roadmap

1. **Phase 0 — Vertical Slice ("Hello, IFA + DParser in-memory")**
   - Link `../dparser` (`libmkdparse.a` + `libdparse_gc.a`) and `../pyc/ifa` (`libifa_core.a` / `libifa_gc.a` + LLVM).
   - Compile a tiny grammar *from an in-memory string* via `mkdparse_from_string` → `write_binary_tables_to_string` → `read_binary_tables_from_string`, parse input to a CST, lower to IF1, run `ifa_analyze` + `ifa_optimize`, and execute via C or LLVM LLJIT.
   - Verify `ifa_reset()` works cleanly across multiple sequential runs in one process.
2. **Phase 1 — Primitive & Transfer Function Injection**
   - Build `PrimRegistry` over `prim_reg` with a debug monotonicity checker.
   - Build a small example language that injects custom primitives with custom transfer functions, `check_fn`s, and C/LLVM emitters.
3. **Phase 2 — Core Workbench Language + Extensible Grammars & Hygienic Macros**
   - Implement `SyntaxObj` with `ScopeSet` hygiene and multi-pass syntax-directed lowering.
   - Add dynamic grammar extension (modules/macros extending `GrammarDef` and recompiling tables in memory).
   - Add hygienic procedural macros and grammar-aware quasiquoting.
4. **Phase 3 — Analysis-Driven Metaprogramming**
   - Expose IFA analysis results (`AType`, constants, contours, liveness, call targets) as queryable attributes on `WbAST`/`Sym`.
   - Implement the staged Analyze → Attribute-Driven Macro Rewrite → `ifa_reset()` → Re-analyze loop.
5. **Phase 4 — Self-Hosted Compile-Time Execution**
   - Compile and execute guest-language macros and transfer functions in-process via IFA's LLVM LLJIT (`llvm_jit_execute`).

---

## 7. Resolved Design Decisions

1. **Build system & subtree policy:**
   - **Makefile** (matching `../pyc`), with `ifa/` and `dparser/` managed as git subtrees (`make pull-ifa` / `push-ifa` / `pull-dparser` / `push-dparser`).
   - Small, clean, upstream-friendly additions to `ifa/` and `dparser/` are allowed (e.g., `void* user_data` on `RegisteredPrim`, `f->llvm` reset hygiene, read-only `FA` query helpers, `libifa_core.a` without `frontend/`) and can be pushed upstream.
2. **Primary backend:**
   - **LLVM (AOT + LLJIT)** is the primary backend (powering both native execution and in-process compile-time metaprogramming), with C codegen kept working as a secondary/debugging backend.
3. **Compile-time metaprogramming & transfer functions (two-tier):**
   - **Phases 1–3:** Author procedural macros and custom IFA transfer functions as **C++23 host handlers** for rapid iteration and direct access to IFA/LLVM internals.
   - **Phase 4:** Add **in-language compile-time execution** via IFA + LLVM LLJIT (`llvm_jit_execute`).
4. **Hygiene model:**
   - **Racket-style sets-of-scopes hygiene** (`SyntaxObj` + `ScopeSet`) as the default across dynamically extended grammars, with explicit opt-out escapes (`datum->syntax`-style) for unhygienic/anaphoric macros.
5. **First experimental language (`examples/minilang`):**
   - A **minimal expression-oriented core** (`let`, lambdas/closures, records, `if`/`match`, primitives) with extensible grammar rules.
