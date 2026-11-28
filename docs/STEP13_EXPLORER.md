# Step 13 — Inspecting a Language Under Construction

> **Goal (from [ROADMAP.md](ROADMAP.md)):** `langwb explore <file>` shows
> each stage of a program in one place:
> - parsing, including `Amb` alternatives;
> - Stage-0 expansion with its hygiene scopes;
> - IFA's types, constants and contours;
> - Stage-1 rewrites and contour clones.
>
> It is the workbench's counterpart to Racket's macro stepper, and the tool
> for questions like issue 214's "why does this edge oscillate".
>
> **Status: plan.**

---

## 0. Approach and the Two Decisions

**Record, then render.** A compilation writes what it did and why into a
*compilation record*, a SQLite file. `langwb explore` serves pages that read
the record on demand. The alternative is the explorer's current design: one
HTML file with all its data embedded as JSON. That doesn't fit langwb, for
three reasons:

- **Many IFA runs per evaluation.** One evaluation runs IFA many times,
  measured with a breakpoint on `ifa_analyze`:

  | Program | IFA runs |
  |---|---|
  | `wordcount.mlite` | 11 |
  | `expr_eval.mlite` | 13 |
  | `comptime.ml` | 3 |

  The runs come from each macro's first JIT, the Stage-0 analysis, each
  contour-clone probe, the final analysis, and code generation. Each run
  calls `ifa_reset()`, and the explorer's recorder (`ifa/common/html.cc`)
  is a single global that holds one run.
- **langwb's stages have no record at all.** Today only the final expanded
  tree and its attributes survive an evaluation, in `InspectResult`. The CST,
  the `Amb` alternatives, each macro step and the scopes it added, the
  Stage-1 rewrites and the clone decisions are all thrown away.
- **Size.** The page embeds every recorded pass, under a 100 MB cap.
  `wordcount.mlite` reaches IFA's pass cap (101 passes, issue 214) in a
  single run.

A record can hold many runs and every stage. Pages load only what is being
looked at. Tests, agents and later the LSP can query it with SQL.

**Decision 1 — a compilation record in SQLite, rendered dynamically:**

- **One file per run:** `<file>.explore.db`, which `langwb explore` writes.
- **Why SQLite:** `libsqlite3` 3.45 is installed, it is one file, and the
  `sqlite3` CLI can read it.
- **New dependency:** SQLite becomes a link dependency of `langwb`
  (open question §6.1).
- **Serving:** a small built-in HTTP server on localhost serves the page
  assets and a JSON API over the record (`langwb explore <file> [--port N]`).
  `--export <dir>` writes a static snapshot of the pages plus JSON files, for
  CI artifacts and for sharing.

**Decision 2 — no scyth-style, database-driven compilation, for now.**
Scyth's database is the *source of truth* for code: content-addressed
definitions, incremental derivation in Salsa and Datalog, and facts served to
agents. Here the record is an *output* of an ordinary compilation. Four
reasons:

1. **The unit of meaning is different.** A scyth definition's hash fixes its
   meaning. A langwb module's meaning depends on its text plus its `#lang`
   (grammar and macros), its imports, and hygiene scopes that are allocated
   per run. Content-addressing expanded syntax would need a canonical
   numbering of scopes. `contour_clone.cc` already numbers ScopeIds by first
   appearance for one purpose, so this is possible, but it is research.
2. **The cost is in the wrong place for it to help.** IFA is whole-program,
   and it dominates: an MLite evaluation takes about 3 s (`wordcount` 3.05 s),
   most of it loading the language and running IFA. Incremental derivation
   would make the cheap part (re-expanding unchanged modules) cheaper and
   leave the expensive part alone.
3. **Correctness risk with no current need.** Step 12.5 just made module
   meaning depend only on a module's own text and explicit imports. A cache
   keyed on that is now possible, but nothing measured needs it yet.
4. **The record keeps the door open.** Each module row stores the key a
   module cache would use: a hash of the module's text, its language
   snapshot and its imports' keys. It is recorded, not used. The
   relational shape is the same as scyth's fact layer (rows per subject,
   read-only SQL), so moving to database-driven compilation later would
   change who writes the rows, not how they are read.

   Entry criterion (ROADMAP *Deferred*): a measurement showing LSP or
   evaluation latency on a multi-module program is dominated by re-expanding
   modules that did not change.

---

## 1. What Exists

| Piece | Where | What it gives |
|---|---|---|
| IFA explorer | `ifa/common/html.cc` (2.6k lines) and `ifa/common/explorer/` (`explorer.js`, 3.7k lines) | Six panels: source, CFG, IFA passes (entry-set and creation-set splits, constant demands, violations), cloning, optimizations, codegen. It records each FA pass through `html_record_*` hooks in `fa.cc`, using global state. `dump_html()` reads the source and codegen files from disk and writes `<fn>.html`, and `<fn>.json` when `IFA_DUMP_JSON` is set. |
| langwb's IFA entry points | `ifa_bridge/bridge.cc` | `analyze_syntax` (Stage-0 attributes), the JIT, persistent macro modules and AOT. Each one runs `ifa_analyze` and then `ifa_reset`. |
| Contour cloning | `workbench/contour_clone.cc` | Probes candidate clones with `analyze_syntax` and records demands (`begin_demand_recording`). The decisions are not kept. |
| Syntax provenance | `syntax/syntax.h` | `node_id` is a per-process counter. `origin_macro` and `with_origin` record which macro a node came from, but not which node it was expanded from. |
| LSP index | `workbench/lsp.cc` | Builds hovers and definitions from `InspectResult` in a worker process, then serializes them. That is a small record of its own (§5). |

---

## 2. Refactoring Needed

Each item is a separate commit. Items R1 and R2 change no output.

- **R1. Make the pipeline's products explicit.**
  - **Problem:** `eval_impl` (in a 3.5k-line `workbench.cc`) threads its
    stages through locals. Per-module CSTs, expansion results, analysis runs
    and clone decisions are temporaries.
  - **Change:** a `Compilation` object owns them for the length of one
    evaluation: modules with their CSTs and expanded trees, analysis runs
    with their purpose and attributes, the Stage-1 output, and the clone
    decisions. `InspectResult` becomes a view of it.
  - **Acceptance:** all tests pass byte for byte, as with Step 12's M0.
- **R2. Add a recorder interface.**
  - **Change:** a `CompileRecorder` pointer on the `Compilation`, null by
    default, and called at stage boundaries:
    - modules and sources;
    - parses, with their `Amb` nodes;
    - each macro step (macro, call node, result, scopes added);
    - diagnostics;
    - analysis runs (purpose, module, IFA data);
    - Stage-1 rewrites;
    - clone decisions.
  - **When recording is off:** the only cost is a pointer check.
  - **When recording is on:** results must not change. A differential test
    compares the output with and without recording on every example.
- **R3. Expansion provenance.**
  - **Problem:** a macro stepper needs, for each node, the node it was
    expanded from, and for each scope, the step that added it.
  - **Change:** `SyntaxObj` gains `derived_from`, the id of the call node or
    template node, set where `with_origin` already runs. `ScopeAllocator`
    reports each scope it allocates to the recorder.
  - **Cost:** one field on every node, only written while recording.
- **R4. An IFA run as a value (an `ifa/` change, upstreamable).**
  - **Change:** split `dump_html()` into two parts:
    - `ifa_explorer_json(source_lines, codegen_lines) -> std::string`
      returns the explorer data for the recorded run, and the caller passes
      in what `dump_html()` used to read from disk;
    - writing the file stays as it is for pyc and scyth.
  - **Recorder reset:** the recorder resets after each call, so langwb can
    capture each IFA run before `ifa_reset()`.
  - **Constraints:** no explorer policy changes. It gets its own commit, a
    test in `ifa-test`, and is pushed with `git subtree push`.
- **R5. Label each analysis run.**
  - **Change:** each `ifa_analyze` call site in `bridge.cc` and
    `contour_clone.cc` passes a purpose through the `Compilation`: `macro-jit
    <name>`, `stage0`, `clone-probe <candidate>`, `final`, `aot`.
  - **Size limit:** per-pass IFA recording is on for `final` by default.
    The other runs record only their final state, and
    `--ifa-passes all` records every pass of every run.

---

## 3. The Record (schema v0)

Tables are keyed by run-local ids. `node.id` is `SyntaxObj::node_id`.

| Table | Columns (main ones) |
|---|---|
| `meta` | `schema_version`, `langwb_version`, `argv`, `started`, `result` |
| `module` | `id`, `path`, `lang`, `scope_id`, `cache_key` (§0, recorded only), `parent_module` (importer) |
| `source` | `module_id`, `text` |
| `parse` | `id`, `module_id`, `form_index`, `cst` (JSON), `amb_count` |
| `node` | `id`, `stage` (`parsed` / `stage0` / `stage1` / `final`), `module_id`, `kind`, `symbol`, `text`, `file`, `line`, `col`, `end_line`, `end_col`, `parent`, `child_index`, `derived_from`, `origin_macro` |
| `node_scope` | `node_id`, `scope_id` |
| `scope` | `id`, `kind` (module / intro / use-site / def-site), `step_id` |
| `expansion_step` | `id`, `phase` (0 or 1), `macro`, `call_node`, `result_node`, `module_id`, `seq` |
| `analysis_run` | `id`, `purpose`, `module_id`, `passes`, `ifa_json` (blob, IFA's explorer data as is) |
| `attr` | `run_id`, `node_id`, `types`, `constant`, `binder` |
| `clone_decision` | `id`, `function`, `candidate`, `outcome`, `reason`, `probe_run_id` |
| `diagnostic` | `severity`, `code`, `file`, `line`, `col`, `message`, `notes` (JSON) |

**IFA data stays in IFA's own JSON (`analysis_run.ifa_json`), not
normalized into tables.** IFA's explorer already renders it. Normalizing it
would duplicate IFA's model in langwb and would need changing whenever IFA
records more. It can be done later if queries need it.

**Querying the record:**
- **Tests** use `langwb explore --query '<SQL>' <db>`, read-only, so tests
  don't depend on the `sqlite3` CLI.
- **Agents** use the same read-only SQL, as scyth's `query_facts` does.

---

## 4. Milestones

Each milestone is in the style of Step 12:
- baselines first;
- every visible change is recorded in §7 as a before/after pair;
- MLite is the acceptance corpus;
- each general change gets a minilang-only test.

| M | Content | Acceptance |
|---|---|---|
| **M0** | Baselines and measurements: IFA runs per evaluation for every example (the numbers in §0), the size of IFA's explorer JSON per run, and recording overhead. Write the goldens for R1. | A §7 table of the measurements. |
| **M1** | R1 + R2 (`Compilation`, `CompileRecorder`, null by default). | All tests byte for byte; no measurable slowdown on the suite. |
| **M2** | SQLite recorder for langwb's own stages: modules, sources, parses with `Amb`, Stage-0 nodes, scopes and expansion steps (R3), diagnostics. `langwb explore --record <db>` and `--query`. | The differential test (output with and without recording) passes on every example. Query tests pin a minilang macro's steps and scopes, and MLite `case` expansion. |
| **M3** | IFA and Stage 1: R4 (`ifa/` commit), R5, per-run `analysis_run`, `attr`, Stage-1 nodes and rewrites, `clone_decision`. | `wordcount.mlite` records 11 labelled runs. A query lists the clone decisions for `fold_l` and shows the edge from issue 214. |
| **M4** | The viewer: `langwb explore <file>` (record and serve) and `--export <dir>`. Pages: |  |
|  | • **Source.** Modules as tabs; a node at the cursor shows its stage history. |  |
|  | • **Macro stepper.** Step forward and back through `expansion_step`. Before and after trees, scopes as coloured sets, `derived_from` links. |  |
|  | • **Parse.** CST and `Amb` alternatives. |  |
|  | • **Analysis runs.** A list of runs; each opens IFA's `explorer.js` on that run's `ifa_json`. |  |
|  | • **Stage 1 and clones.** Rewrites and decisions, each linked to its probe run. |  |
|  |  | Export snapshots of a minilang program and an MLite one are checked for structure (pages and JSON files present, a few pinned queries). The pages are checked by hand and in a browser. |
| **M5** | Close: update ROADMAP, decide on the open questions, record what is deferred. | Final suite. |

---

## 5. Out of Scope, and What It Enables Later

- **The other two Step 13 bullets in ROADMAP.** These are the remaining
  analysis attributes for the Meta-ABI (liveness, purity, call targets,
  frequency) and Step 11's friction-log items (list builders, non-`int64`
  constants, contour-insensitive checks). They are language features, not
  inspection, and don't depend on the explorer, although the explorer will
  make them easier to debug. **Proposal:** move them to a new Step 14
  (open question §6.2).
- **Database-driven or incremental compilation.** Moves to ROADMAP
  *Deferred* with the entry criterion in §0.
- **The LSP reading the record.** The LSP's worker could write a record and
  the server could query it, replacing the hand-written index serialization
  in `lsp.cc`. That is worth doing only once the record covers what the
  index needs. It goes in *Deferred*.
- **Normalizing IFA's data into tables, and diffs between two records**
  (compare two versions of a `.lang`). Both are deferred until there is a
  concrete need.

---

## 6. Open Questions

1. **SQLite dependency.** Should it be required, or optional behind a build
   flag that disables `explore`? Recommendation: required. It is small,
   installed here, and common everywhere.
2. **Move the Meta-ABI and friction-log items to Step 14?** Recommendation:
   yes.
3. **Default `explore` mode: serve or export?** Recommendation: serve
   (record, start the server, print the URL), with `--export` for CI and
   sharing.

---

## 7. Change Log (filled in per milestone)
