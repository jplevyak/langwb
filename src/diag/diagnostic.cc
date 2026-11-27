#include "diag/diagnostic.h"

#include <cstdio>

namespace langwb::diag {

namespace {

Sink *g_current = nullptr;

StderrSink &default_sink() {
  static StderrSink sink;
  return sink;
}

}  // namespace

void StderrSink::emit(Diagnostic d) {
  std::string t = render_text(d);
  std::fputs(t.c_str(), stderr);
}

Sink &current_sink() { return g_current ? *g_current : default_sink(); }

ScopedSink::ScopedSink(Sink &sink) : prev_(g_current) { g_current = &sink; }

ScopedSink::~ScopedSink() { g_current = prev_; }

void emit(Diagnostic d) { current_sink().emit(std::move(d)); }

ScopedDefaultFile::ScopedDefaultFile(const char *file) : file_(file), outer_(g_current) {
  g_current = this;
}

ScopedDefaultFile::~ScopedDefaultFile() { g_current = outer_; }

void ScopedDefaultFile::emit(Diagnostic d) {
  if (file_) {
    if (!d.range.file && d.range.line > 0) d.range.file = file_;
    for (Diagnostic &n : d.notes) {
      if (!n.range.file && n.range.line > 0) n.range.file = file_;
    }
  }
  (outer_ ? *outer_ : default_sink()).emit(std::move(d));
}

namespace {
const char *g_main_file = nullptr;

void trim_newlines(std::string &s) {
  while (!s.empty() && s.back() == '\n') s.pop_back();
}
}  // namespace

const char *main_file_name() { return g_main_file ? g_main_file : "<input>"; }

ScopedMainFile::ScopedMainFile(const std::string &path) : prev_(g_main_file) {
  // Interned for the process: diagnostics may outlive the scope.
  static std::vector<std::string *> names;
  names.push_back(new std::string(path));
  g_main_file = path.empty() ? prev_ : names.back()->c_str();
}

ScopedMainFile::~ScopedMainFile() { g_main_file = prev_; }

void emit_message(Severity sev, const char *code, SourceRange range, std::string message,
                  uint64_t node_id) {
  Diagnostic d;
  d.severity = sev;
  d.code = code ? code : "";
  d.range = range;
  d.node_id = node_id;
  d.message = std::move(message);
  trim_newlines(d.message);
  emit(std::move(d));
}

std::string vstrf(const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int n = std::vsnprintf(nullptr, 0, fmt, ap2);
  va_end(ap2);
  if (n <= 0) return {};
  std::string out(static_cast<size_t>(n), '\0');
  std::vsnprintf(out.data(), out.size() + 1, fmt, ap);
  return out;
}

std::string strf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::string out = vstrf(fmt, ap);
  va_end(ap);
  return out;
}

void emitf(Severity sev, const char *code, SourceRange range, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::string message = vstrf(fmt, ap);
  va_end(ap);
  emit_message(sev, code, range, std::move(message));
}

void Builder::printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  message_ += vstrf(fmt, ap);
  va_end(ap);
}

void Builder::note(SourceRange range, std::string message) {
  Diagnostic n;
  n.severity = Severity::Note;
  n.code = code_ ? code_ : "";
  n.range = range;
  n.message = std::move(message);
  notes_.push_back(std::move(n));
}

void Builder::emit() {
  if (emitted_) return;
  emitted_ = true;
  trim_newlines(message_);
  if (message_.empty()) return;
  Diagnostic d;
  d.severity = sev_;
  d.code = code_ ? code_ : "";
  d.range = range_;
  d.node_id = node_id_;
  d.message = std::move(message_);
  d.notes = std::move(notes_);
  langwb::diag::emit(std::move(d));
}

Diagnostic expansion_note(const char *file, const std::string &macro, int line, int col) {
  Diagnostic n;
  n.severity = Severity::Note;
  n.code = "macro";
  n.range = SourceRange{file, line, col};
  n.message = "in expansion of '" + macro + "'";
  return n;
}

std::string render_text(const Diagnostic &d) {
  const char *sev = d.severity == Severity::Error     ? "error"
                    : d.severity == Severity::Warning ? "warning"
                                                      : "note";
  std::string out;
  if (d.range.line > 0) {
    out += strf("%s:%d:%d: ", d.range.file ? d.range.file : main_file_name(), d.range.line,
                d.range.col);
  } else {
    out += "langwb: ";
  }
  out += strf("%s: %s\n", sev, d.message.c_str());
  for (const Diagnostic &n : d.notes) out += render_text(n);
  return out;
}

void lsp_position(const Diagnostic &d, int *line, int *character) {
  if (d.range.line > 0 && !d.range.file) {
    *line = d.range.line - 1;
    *character = d.range.col < 0 ? 0 : d.range.col;
  } else {
    *line = 0;
    *character = 0;
  }
}

void lsp_range(const Diagnostic &d, int *line, int *character, int *end_line, int *end_character) {
  lsp_position(d, line, character);
  bool in_doc = d.range.line > 0 && !d.range.file;
  bool has_end = in_doc && d.range.end_line > 0 &&
                 (d.range.end_line > d.range.line ||
                  (d.range.end_line == d.range.line && d.range.end_col > d.range.col));
  if (has_end) {
    *end_line = d.range.end_line - 1;
    *end_character = d.range.end_col;
  } else {
    *end_line = *line;
    *end_character = *character + 1;
  }
}

}  // namespace langwb::diag
