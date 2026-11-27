#pragma once

// Structured diagnostics (Step 12, docs/STEP12_DIAGNOSTICS.md §2.1).
//
// Every diagnostic the workbench reports (syntax errors, resolution errors,
// macro and check errors, IFA type violations, grammar compilation errors) is
// emitted as a `Diagnostic` into the current `Sink`. The default sink prints
// each diagnostic's text to stderr as it is emitted, so command-line output
// and its ordering relative to other stderr output are unchanged; the LSP
// installs a collecting sink and builds its diagnostics from the structured
// fields instead of parsing text.
//
// Milestone M0 keeps today's exact wording: `text` carries the legacy
// rendering (newline-terminated, possibly several lines), and `range` is the
// position that rendering refers to. Later milestones derive the text from the
// structured fields.

#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace langwb::diag {

enum class Severity { Error, Warning, Note };

// 1-based line, 0-based column (as in `SyntaxObj`). `line == 0` means the
// diagnostic has no source position. `file` is null for the program being
// evaluated and an interned module path for imported modules.
struct SourceRange {
  const char *file = nullptr;
  int line = 0;
  int col = 0;
  int end_line = 0;
  int end_col = 0;
};

struct Diagnostic {
  Severity severity = Severity::Error;
  // Stable-ish category, e.g. "syntax", "resolve", "type", "check", "grammar".
  std::string code;
  SourceRange range;
  std::string message;
  std::vector<Diagnostic> notes;
  uint64_t node_id = 0;
  // Exact command-line rendering (M0: today's text).
  std::string text;
};

class Sink {
 public:
  virtual ~Sink() = default;
  virtual void emit(Diagnostic d) = 0;
};

// Prints each diagnostic's text to stderr immediately.
class StderrSink : public Sink {
 public:
  void emit(Diagnostic d) override;
};

class CollectingSink : public Sink {
 public:
  void emit(Diagnostic d) override { diags.push_back(std::move(d)); }
  std::vector<Diagnostic> diags;
};

class DiscardingSink : public Sink {
 public:
  void emit(Diagnostic) override {}
};

// The sink diagnostics go to: the innermost `ScopedSink`, else a process-wide
// `StderrSink`.
Sink &current_sink();

// Installs `sink` as the current sink for its lifetime (nests).
class ScopedSink {
 public:
  explicit ScopedSink(Sink &sink);
  ~ScopedSink();
  ScopedSink(const ScopedSink &) = delete;
  ScopedSink &operator=(const ScopedSink &) = delete;

 private:
  Sink *prev_;
};

void emit(Diagnostic d);

// Emits a diagnostic whose text is exactly `text` (which should end in a
// newline) and whose message is `text` without its trailing newline.
void emit_text(Severity sev, const char *code, SourceRange range, std::string text,
               uint64_t node_id = 0);

// `printf`-style formatting into a std::string.
std::string strf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string vstrf(const char *fmt, va_list ap);

// `emit_text` with a `printf`-formatted text.
void emitf(Severity sev, const char *code, SourceRange range, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

// Accumulates one diagnostic's text from several `printf` calls (as the
// pre-Step-12 code wrote it with several `fprintf(stderr, ...)` calls) and
// emits it once, when `emit()` is called or the builder is destroyed.
class Builder {
 public:
  Builder(Severity sev, const char *code, SourceRange range, uint64_t node_id = 0)
      : sev_(sev), code_(code), range_(range), node_id_(node_id) {}
  ~Builder() { emit(); }
  Builder(const Builder &) = delete;
  Builder &operator=(const Builder &) = delete;

  void printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void append(const std::string &s) { text_ += s; }
  void emit();

 private:
  Severity sev_;
  const char *code_;
  SourceRange range_;
  uint64_t node_id_;
  std::string text_;
  bool emitted_ = false;
};

// The command-line text of a diagnostic.
std::string render_text(const Diagnostic &d);

// The LSP placement of a diagnostic in the evaluated document: 0-based
// (line, character). Diagnostics without a position, or located in another
// file (an imported module), are placed at the top of the document.
void lsp_position(const Diagnostic &d, int *line, int *character);

}  // namespace langwb::diag
