#define EXTERN
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include "gc/gc.h"
#include "parse/grammar_def.h"
#include "ifa_bridge/bridge.h"
#include "workbench/workbench.h"

extern "C" int _CG_wb_consume_stdout_dirty(void);

namespace {

struct SmokeCase {
  std::string_view expr;
  int64_t expected;
  bool staged = false;
  bool expect_const_inferred = false;
};

// Step 12 M0: structured diagnostics (renderers, sinks, ranges).
int run_diag_unit_tests(langwb::Workbench &wb) {
  namespace diag = langwb::diag;
  int failures = 0;
  auto check = [&](bool ok, const char *what) {
    if (!ok) {
      std::fprintf(stderr, "smoke: diagnostics: %s\n", what);
      ++failures;
    }
  };

  diag::Diagnostic d;
  d.severity = diag::Severity::Error;
  d.range = diag::SourceRange{nullptr, 3, 4};
  d.message = "boom";
  diag::Diagnostic note;
  note.severity = diag::Severity::Note;
  note.range = diag::SourceRange{"lib.ml", 1, 0};
  note.message = "from here";
  d.notes.push_back(note);
  check(diag::render_text(d) == "<input>:3:4: error: boom\nlib.ml:1:0: note: from here\n",
        "render_text from structured fields");
  {
    diag::ScopedMainFile main_file("prog.ml");
    check(diag::render_text(d) == "prog.ml:3:4: error: boom\nlib.ml:1:0: note: from here\n",
          "render_text names the main program's file");
  }
  diag::Diagnostic unlocated;
  unlocated.severity = diag::Severity::Warning;
  unlocated.message = "careful";
  check(diag::render_text(unlocated) == "langwb: warning: careful\n",
        "render_text without a position");
  {
    diag::CollectingSink sink;
    diag::ScopedSink ss(sink);
    {
      diag::ScopedDefaultFile in_mod("mod.ml");
      diag::emitf(diag::Severity::Error, "t", diag::SourceRange{nullptr, 1, 2}, "x");
      diag::emitf(diag::Severity::Error, "t", diag::SourceRange{}, "y");
    }
    check(sink.diags.size() == 2 && sink.diags[0].range.file &&
              std::string(sink.diags[0].range.file) == "mod.ml" && !sink.diags[1].range.file,
          "ScopedDefaultFile attributes positioned diagnostics to its file");
  }

  int l = -1, c = -1, el = -1, ec = -1;
  diag::lsp_position(d, &l, &c);
  check(l == 2 && c == 4, "lsp_position in the document");
  diag::Diagnostic in_module = d;
  in_module.range.file = "lib.ml";
  diag::lsp_position(in_module, &l, &c);
  check(l == 0 && c == 0, "lsp_position of a module diagnostic is the top of the document");
  diag::Diagnostic unpositioned;
  diag::lsp_position(unpositioned, &l, &c);
  check(l == 0 && c == 0, "lsp_position without a position");
  diag::lsp_range(d, &l, &c, &el, &ec);
  check(l == 2 && c == 4 && el == 2 && ec == 5, "lsp_range without an end");
  diag::Diagnostic ranged = d;
  ranged.range.end_line = 4;
  ranged.range.end_col = 2;
  diag::lsp_range(ranged, &l, &c, &el, &ec);
  check(l == 2 && c == 4 && el == 3 && ec == 2, "lsp_range with a multi-line end");

  {
    diag::CollectingSink outer, inner;
    diag::ScopedSink so(outer);
    diag::emitf(diag::Severity::Warning, "t", diag::SourceRange{}, "w%d\n", 1);
    {
      diag::ScopedSink si(inner);
      diag::Builder b(diag::Severity::Error, "t", diag::SourceRange{nullptr, 2, 1});
      b.printf("part %s", "one");
      b.printf(", part two\n");
    }
    diag::emit_message(diag::Severity::Error, "t", diag::SourceRange{}, "after\n");
    check(outer.diags.size() == 2 && inner.diags.size() == 1, "nested sinks receive their diagnostics");
    check(inner.diags.size() == 1 && inner.diags[0].message == "part one, part two" && inner.diags[0].range.line == 2,
          "Builder emits one diagnostic with its accumulated message");
    check(outer.diags.size() == 2 && outer.diags[0].severity == diag::Severity::Warning &&
              outer.diags[0].message == "w1",
          "emitf formats text and message");
  }

  // Reserved words survive grammar composition (review 2026-11-27 §2.1).
  {
    langwb::parse::GrammarDef base, ext;
    ext.add_reserved("kw");
    base.extend(ext);
    check(base.reserved().size() == 1 && base.reserved()[0] == "kw",
          "GrammarDef::extend keeps reserved words");
  }

  // End positions: a parsed multi-line expression ends where its text ends.
  if (auto gram = wb.compiled_grammar()) {
    langwb::parse::CstNode *cst = gram->parse("let x =\n  41 in x", "expr", /*quiet=*/true);
    check(cst != nullptr, "parse a multi-line expression");
    if (cst) {
      check(cst->line == 1 && cst->col == 0 && cst->end_line == 2 && cst->end_col == 9,
            "CST end position of a multi-line node");
      langwb::syntax::SyntaxObj *stx = langwb::syntax::SyntaxObj::from_cst(cst);
      langwb::syntax::SyntaxObj *copy = stx ? stx->clone() : nullptr;
      check(copy && copy->end_line == 2 && copy->end_col == 9, "SyntaxObj keeps end positions");
      diag::SourceRange r = langwb::syntax::range_of(copy);
      check(r.end_line == 2 && r.end_col == 9, "range_of includes the end");
    }
  } else {
    check(false, "compile the default grammar");
  }
  return failures;
}

int run_smoke_test() {
  langwb::Workbench wb;
  if (run_diag_unit_tests(wb) != 0) return 1;

  auto g1 = wb.compiled_grammar();
  auto g2 = wb.compiled_grammar();
  if (!g1 || g1 != g2 || wb.grammar_cache_size() != 1) {
    std::fprintf(stderr, "smoke: GrammarCache failed to compile/cache extended grammar\n");
    return 1;
  }

  if (g1->parse_check("1 + * 3")) {
    std::fprintf(stderr, "smoke: unexpectedly accepted invalid input\n");
    return 1;
  }

  bool trace = std::getenv("LANGWB_TRACE") != nullptr;

  // Step 1 (Zero-C++ roadmap): register procedural macros written purely in
  // minilang and executed in-process via IFA + LLVM LLJIT + Phase-1 Meta-ABI!
  wb.register_guest_stage0_macro("guest_twice", R"(
    let arg = stx_arg(stx_call(), 0) in
    let t = stx_parse("expr", "let tmp = __x__ in tmp + tmp") in
    stx_splice(t, "__x__", arg)
  )");

  wb.register_guest_stage1_macro("guest_if_const", R"(
    let c = stx_call() in
    if attr_is_const_i64(stx_arg(c, 0)) != 0
    then stx_arg(c, 1)
    else stx_arg(c, 2)
  )");

  constexpr SmokeCase kCases[] = {
      // Phase 0: arithmetic + let
      {"1 + 2 * 3", 7, false, true},
      {"(1 + 2) * 3", 9, false, true},
      {"let x = 10 in let y = x * 4 + 2 in y - 5", 37, false, true},
      // Phase 1: custom primitive + transfer function + inline LLVM emitter
      {"clamp(15, 0, 10)", 10, false, true},
      {"clamp(0 - 5, 0, 10)", 0, false, true},
      {"let x = 7 in clamp(x * 2, 1, 10) + clamp(3, 1, 10)", 13, false, true},
      // Phase 2: hygienic, syntax-extension, and anaphoric macros
      {"let tmp = 100 in with_tmp(7, tmp + 1)", 108, false, true},
      {"unless 1 > 2 then 42 else 99", 42, false, false},
      {"unless 5 > 2 then 42 else 99", 99, false, false},
      {"aif 10 + 5 then it * 2 else 0", 30, false, false},
      {"aif 3 - 3 then it * 2 else 77", 77, false, false},
      // Minilang core: lambdas, polymorphic calls, records, field access
      {"let add = fn(a, b) => a + b in add(19, 23)", 42, false, true},
      {"let id = fn(x) => x in id(10) + id(32)", 42, false, false},
      {"let p = { x: 15, y: 27 } in p.x + p.y", 42, false, false},
      {"let id = fn(v) => v in let p = id({ a: 11, b: 31 }) in p.a + p.b", 42, false, false},
      // Phase 3: IFA-driven Stage-1 metaprogramming & record reflection
      {"let e = clamp(3, 0, 5) in let x = 4 in pow_opt(x, e)", 64, true, true},
      {"let k = 2 + 3 in if_const(k, 100, 200)", 100, true, true},
      {"let r = runtime_val(5) in if_const(r, 100, 200)", 200, true, true},
      {"let id = fn(v) => v in let p = id({ x: 10, y: 20, z: 12 }) in sum_fields(p)", 42, true,
       false},
      // Phase 4: in-language compile-time execution via IFA + LLVM LLJIT
      {"comptime(let sq = fn(x) => x * x in sq(6) + 6)", 42, false, true},
      // Comptime LLJIT feeding into IFA constant propagation -> Stage-1 pow_opt unroll!
      {"let n = comptime(let sq = fn(x) => x * x in sq(3) - 6) in pow_opt(2, n)", 8, true, true},
      // Step 1 (Zero-C++): hygienic Stage-0 macro written in minilang (runs in-process via LLJIT)
      {"let tmp = 100 in guest_twice(tmp + 1)", 202, false, true},
      // Step 1 (Zero-C++): IFA-attribute-driven Stage-1 macro written in minilang!
      {"let k = 2 + 3 in guest_if_const(k, 111, 222)", 111, true, true},
      {"let r = runtime_val(5) in guest_if_const(r, 111, 222)", 222, true, true},
      // Step 2 (Zero-C++): inline grammar extension + form_macro + macro + stage1_macro in one program!
      {R"(
        grammar expr += when_expr;
        grammar when_expr : 'when' expr 'do' expr 'otherwise' expr $right 0;
        form_macro when_expr =>
          let f = stx_call() in
          let t = stx_parse("expr", "if __c__ then __a__ else __b__") in
          let t1 = stx_splice(t, "__c__", stx_child(f, 1)) in
          let t2 = stx_splice(t1, "__a__", stx_child(f, 3)) in
          stx_splice(t2, "__b__", stx_child(f, 5));
        macro triple =>
          let a = stx_arg(stx_call(), 0) in
          let t = stx_parse("expr", "let tmp = __x__ in tmp + tmp + tmp") in
          stx_splice(t, "__x__", a);
        stage1_macro pick_const =>
          let c = stx_call() in
          if attr_is_const_i64(stx_arg(c, 0)) != 0
          then stx_arg(c, 1)
          else stx_arg(c, 2);
        let tmp = 10 in
        when 5 > 2 do pick_const(tmp + 1, triple(tmp + 4), 0) otherwise 999
      )",
       42, true, false},
      // Step 3 (Zero-C++): direct c_call FFI to libc `labs`
      {"c_call(\"labs\", 0 - 42)", 42, false, false},
      // Step 3 (Zero-C++): in-language `prim` + custom minilang IFA transfer function + Stage-1 unroll!
      {R"(
        prim my_abs c_call "labs" transfer =>
          if fa_arg_is_const_i64(0) != 0
          then
            let v = fa_arg_const_i64(0) in
            fa_emit_const_i64(if v < 0 then 0 - v else v)
          else
            fa_emit_i64();
        let e = my_abs(0 - 4) in
        pow_opt(2, e) + my_abs(runtime_val(0 - 7))
      )",
       23, true, false},
      // Step 4 (Zero-C++): `#lang mydsl` (grammar + macros + FFI/transfer + #%module-begin hook)
      {R"(#lang mydsl
        when my_abs(0 - 5) > 2 do
          pick_const(magic + 1, triple(magic + 4), 0)
        otherwise
          999
      )",
       42, true, false},
      // Step 4 (Zero-C++): `#lang scaled` (#%datum interposition hook scaling integer literals x10)
      {"#lang scaled\n2 + 3", 50, true, true},
      // All 6 Racket features: `for_syntax`, `syntax_rule`, `stx_local_expand` + syntax properties
      {R"(#lang racket_demo
        let tmp = 12 in
        when 3 > 1 do
          require_tagged(tagged(triple(bump(tmp))), 0) + require_tagged(999, 3)
        otherwise
          0
      )",
       42, true, false},
      // Core interposition: `hook field` (#%field) doubling every field read!
      {R"ml(
        hook field =>
          let f = stx_call() in
          stx_splice(stx_parse("expr", "(__f__ * 2)"), "__f__", f);
        let p = { x: 10, y: 11 } in p.x + p.y
      )ml",
       42, true, false},
  };

  for (const auto &tc : kCases) {
    if (trace) std::fprintf(stderr, "CASE: %.*s\n", static_cast<int>(tc.expr.size()), tc.expr.data());
    langwb::EvalOptions opts;
    opts.staged = tc.staged;
    int64_t got = 0;
    langwb::ifa_bridge::CompileInfo info;
    if (!wb.eval_i64(tc.expr, &got, opts, &info)) {
      std::fprintf(stderr, "smoke: failed '%.*s'\n", static_cast<int>(tc.expr.size()),
                   tc.expr.data());
      return 1;
    }
    if (got != tc.expected) {
      std::fprintf(stderr, "smoke: '%.*s' => %lld, expected %lld\n",
                   static_cast<int>(tc.expr.size()), tc.expr.data(),
                   static_cast<long long>(got), static_cast<long long>(tc.expected));
      return 1;
    }
    if (tc.expect_const_inferred &&
        (!info.has_inferred_const_i64 || info.inferred_const_i64 != tc.expected)) {
      std::fprintf(stderr, "smoke: '%.*s' IFA did not infer constant %lld\n",
                   static_cast<int>(tc.expr.size()), tc.expr.data(),
                   static_cast<long long>(tc.expected));
      return 1;
    }
  }

  const auto &st = wb.stats();
  if (st.comptime_evals != 2 || st.stage1_unrolls != 3 || st.stage1_reflections != 1 ||
      st.decls_applied != 23) {
    std::fprintf(stderr,
                 "smoke: unexpected stats comptime=%d (exp 2) unrolls=%d (exp 3) reflections=%d "
                 "(exp 1) decls=%d (exp 23)\n",
                 st.comptime_evals, st.stage1_unrolls, st.stage1_reflections, st.decls_applied);
    return 1;
  }

  // Result kinds come from the IFA type of the result, not from the printed text
  // (a string starting with `{` is still a String).
  {
    using K = langwb::ifa_bridge::EvalValue::Kind;
    struct KindCase {
      std::string_view expr;
      K kind;
    };
    static constexpr KindCase kKindCases[] = {
        {"41 + 1", K::Int64},
        {"1 == 1", K::Bool},
        {"\"{not a record}\"", K::String},
        {"[1, 2]", K::List},
        {"{x: 1}", K::Record},
        {"fn(x) => x", K::Function},
        {"type Option = None | Some(val); if runtime_val(1) == 1 then Some(1) else None",
         K::Record},
    };
    for (const auto &kc : kKindCases) {
      langwb::Workbench kwb;
      langwb::ifa_bridge::EvalValue val;
      langwb::EvalOptions opts;
      if (!kwb.eval_value(kc.expr, &val, opts) || val.kind != kc.kind) {
        std::fprintf(stderr, "smoke: '%.*s' has result kind %d, expected %d\n",
                     static_cast<int>(kc.expr.size()), kc.expr.data(), static_cast<int>(val.kind),
                     static_cast<int>(kc.kind));
        return 1;
      }
    }
  }

  std::printf(
      "langwb smoke test passed (all Phases 0-4 + Zero-C++ Steps 1-4 + syntax_rule, "
      "for_syntax, syntax properties, local_expand, & field hooks x31)\n");
  return 0;
}

int run_repl(langwb::Workbench &wb) {
  std::printf("langwb REPL (minilang + IFA + LLVM LLJIT)\n");
  std::printf("commands: :cst <expr>, :expand <expr>, :attrs <expr>, :quit\n");
  std::string line;
  while (true) {
    std::printf("langwb> ");
    std::fflush(stdout);
    if (!std::getline(std::cin, line)) break;

    std::string_view sv(line);
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r'))
      sv.remove_suffix(1);
    if (sv.empty()) continue;
    if (sv == ":quit" || sv == ":q") break;

    std::string lang_name;
    std::string_view lang_sv = sv;
    if (langwb::Workbench::extract_lang_header(lang_sv, &lang_name)) {
      if (wb.load_lang(lang_name)) {
        std::printf("ok (loaded #lang %s)\n", lang_name.c_str());
      } else {
        std::fprintf(stderr, "error: failed to load #lang %s\n", lang_name.c_str());
      }
      continue;
    }

    // `import` only takes effect within an evaluation (`apply_decl` does not
    // handle it), so `import "m"; expr` is evaluated as a program.
    bool is_import = sv.starts_with("import") &&
                     (sv.size() == 6 || !(std::isalnum(static_cast<unsigned char>(sv[6])) ||
                                          sv[6] == '_'));
    if (!is_import && langwb::Workbench::starts_with_decl(sv)) {
      if (wb.apply_decl(sv)) {
        std::printf("ok (declaration applied)\n");
      } else {
        std::fprintf(stderr, "error: invalid declaration\n");
      }
      continue;
    }

    langwb::EvalOptions opts;
    opts.staged = true;
    opts.diag_fp = stdout;
    if (sv.starts_with(":cst ")) {
      opts.dump_cst = true;
      sv.remove_prefix(5);
    } else if (sv.starts_with(":expand ")) {
      opts.dump_expanded = true;
      sv.remove_prefix(8);
    } else if (sv.starts_with(":attrs ")) {
      opts.dump_attrs = true;
      sv.remove_prefix(7);
    }

    langwb::ifa_bridge::EvalValue val;
    if (wb.eval_value(sv, &val, opts)) {
      if (val.stdout_dirty) std::putchar('\n');
      std::printf("=> %s\n", val.to_string().c_str());
    } else {
      std::fprintf(stderr, "error: evaluation failed\n");
    }
  }
  return 0;
}

void print_usage() {
  std::printf("langwb — programming language workbench (DParser + IFA + LLVM LLJIT/AOT)\n");
  std::printf("usage:\n");
  std::printf("  langwb --smoke\n");
  std::printf("  langwb --repl\n");
  std::printf("  langwb --lsp\n");
  std::printf("  langwb build <file.ml> [-o <out>] [--emit-llvm] [--bare]\n");
  std::printf("  langwb [--bare] [-e <expr> | <file.ml>] [--dump-cst] [--dump-expanded] [--dump-attrs]\n");
}

}  // namespace

int main(int argc, char **argv) {
  GC_INIT();
  if (argc <= 1) {
    print_usage();
    return 0;
  }

  langwb::EvalOptions opts;
  opts.staged = true;
  std::string eval_expr;
  std::string file_path;
  std::string out_path;
  bool want_repl = false;
  bool want_lsp = false;
  bool want_bare = false;
  bool want_build = false;
  bool emit_llvm = false;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--smoke") {
      return run_smoke_test();
    } else if (arg == "--repl") {
      want_repl = true;
    } else if (arg == "--lsp") {
      want_lsp = true;
    } else if (arg == "--bare") {
      want_bare = true;
    } else if (arg == "build" && i == 1) {
      want_build = true;
    } else if (arg == "--emit-llvm") {
      emit_llvm = true;
    } else if (arg == "-o" && i + 1 < argc) {
      out_path = argv[++i];
    } else if (arg == "--dump-cst") {
      opts.dump_cst = true;
    } else if (arg == "--dump-expanded") {
      opts.dump_expanded = true;
    } else if (arg == "--dump-attrs") {
      opts.dump_attrs = true;
    } else if (arg == "-e" && i + 1 < argc) {
      eval_expr = argv[++i];
    } else if (arg == "-h" || arg == "--help") {
      print_usage();
      return 0;
    } else if (!arg.starts_with("-")) {
      file_path = std::string(arg);
    } else {
      std::fprintf(stderr, "langwb: unknown option '%.*s'\n", static_cast<int>(arg.size()),
                   arg.data());
      return 1;
    }
  }

  if (want_lsp) {
    return langwb::run_lsp_server(langwb::LspOptions{.bare = want_bare});
  }
  langwb::Workbench wb = want_bare ? langwb::Workbench::create_bare() : langwb::Workbench();
  if (want_repl) {
    return run_repl(wb);
  }

  std::string source = eval_expr;
  if (source.empty() && !file_path.empty()) {
    std::ifstream ifs(file_path);
    if (!ifs) {
      std::fprintf(stderr, "langwb: cannot open '%s'\n", file_path.c_str());
      return 1;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    source = oss.str();
    opts.source_path = file_path;
  }

  if (source.empty()) {
    print_usage();
    return 1;
  }

  if (want_build) {
    bool used_default_out = out_path.empty();
    if (used_default_out) out_path = emit_llvm ? "a.ll" : "a.out";
    if (!wb.build_aot(source, out_path, emit_llvm, opts)) {
      std::fprintf(stderr, "langwb: build failed\n");
      return 1;
    }
    if (used_default_out) {
      std::fprintf(stderr, "langwb: built %s\n", out_path.c_str());
    }
    return 0;
  }

  langwb::ifa_bridge::EvalValue result;
  if (!wb.eval_value(source, &result, opts)) {
    std::fprintf(stderr, "langwb: evaluation failed\n");
    return 1;
  }
  if (result.stdout_dirty) std::putchar('\n');
  std::printf("%s\n", result.to_string().c_str());
  return 0;
}
