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

void emit_text(Severity sev, const char *code, SourceRange range, std::string text,
               uint64_t node_id) {
  Diagnostic d;
  d.severity = sev;
  d.code = code ? code : "";
  d.range = range;
  d.node_id = node_id;
  d.message = text;
  while (!d.message.empty() && d.message.back() == '\n') d.message.pop_back();
  d.text = std::move(text);
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
  std::string text = vstrf(fmt, ap);
  va_end(ap);
  emit_text(sev, code, range, std::move(text));
}

void Builder::printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  text_ += vstrf(fmt, ap);
  va_end(ap);
}

void Builder::emit() {
  if (emitted_) return;
  emitted_ = true;
  if (text_.empty()) return;
  emit_text(sev_, code_, range_, std::move(text_), node_id_);
}

std::string render_text(const Diagnostic &d) {
  if (!d.text.empty()) return d.text;
  const char *sev = d.severity == Severity::Error     ? "error"
                    : d.severity == Severity::Warning ? "warning"
                                                      : "note";
  std::string out;
  if (d.range.line > 0) {
    out += strf("%s:%d:%d: ", d.range.file ? d.range.file : "<input>", d.range.line, d.range.col);
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
