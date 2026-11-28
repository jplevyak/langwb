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
// The command-line text is rendered from the structured fields (M2):
// `<file>:L:C: error: message`, then one `<file>:L:C: note: ...` line per note.
// Diagnostics without a position render as `langwb: error: message`.

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
// Errors emitted so far (to any sink): a caller compares two readings to tell
// whether a step it ran already reported its failure.
std::size_t error_count();

// For its lifetime, diagnostics (and notes) without a file are attributed to
// `file` (e.g. while a module or `.lang` is read: syntax errors from the parser
// carry only a position) and passed on to the sink that was current before.
class ScopedDefaultFile : public Sink {
 public:
  explicit ScopedDefaultFile(const char *file);
  ~ScopedDefaultFile() override;
  ScopedDefaultFile(const ScopedDefaultFile &) = delete;
  ScopedDefaultFile &operator=(const ScopedDefaultFile &) = delete;
  void emit(Diagnostic d) override;

 private:
  const char *file_;
  Sink *outer_;
};

// Emits a diagnostic with `message` (a trailing newline is dropped).
void emit_message(Severity sev, const char *code, SourceRange range, std::string message,
                  uint64_t node_id = 0);

// The name diagnostics use for the program being evaluated (`SourceRange::file`
// null): its path when it was read from a file, else `<input>` (`-e`, the
// REPL, embedder strings). `ScopedMainFile` sets it for its lifetime.
const char *main_file_name();
class ScopedMainFile {
 public:
  explicit ScopedMainFile(const std::string &path);
  ~ScopedMainFile();
  ScopedMainFile(const ScopedMainFile &) = delete;
  ScopedMainFile &operator=(const ScopedMainFile &) = delete;

 private:
  const char *prev_;
};

// `printf`-style formatting into a std::string.
std::string strf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string vstrf(const char *fmt, va_list ap);

// `emit_message` with a `printf`-formatted message.
void emitf(Severity sev, const char *code, SourceRange range, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

// Accumulates one diagnostic's message from several `printf` calls, plus its
// notes, and emits it once, when `emit()` is called or the builder is destroyed.
class Builder {
 public:
  Builder(Severity sev, const char *code, SourceRange range, uint64_t node_id = 0)
      : sev_(sev), code_(code), range_(range), node_id_(node_id) {}
  ~Builder() { emit(); }
  Builder(const Builder &) = delete;
  Builder &operator=(const Builder &) = delete;

  void printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void append(const std::string &s) { message_ += s; }
  // Adds a note (rendered on its own line, and as LSP `relatedInformation`).
  void note(SourceRange range, std::string message);
  void note(Diagnostic n) { notes_.push_back(std::move(n)); }
  void emit();

 private:
  Severity sev_;
  const char *code_;
  SourceRange range_;
  uint64_t node_id_;
  std::string message_;
  std::vector<Diagnostic> notes_;
  bool emitted_ = false;
};

// A note "in expansion of '<macro>'" at the macro's call site.
Diagnostic expansion_note(const char *file, const std::string &macro, int line, int col);

// The command-line text of a diagnostic.
std::string render_text(const Diagnostic &d);

// The LSP placement of a diagnostic in the evaluated document: 0-based
// (line, character). Diagnostics without a position, or located in another
// file (an imported module), are placed at the top of the document.
void lsp_position(const Diagnostic &d, int *line, int *character);
// The LSP range (0-based start and end): the diagnostic's end when it is
// known and in the document, else one character past the start.
void lsp_range(const Diagnostic &d, int *line, int *character, int *end_line, int *end_character);

}  // namespace langwb::diag
