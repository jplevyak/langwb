#include "workbench/workbench.h"

#include "gc/gc.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <cerrno>
#include <iostream>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace langwb {

namespace {

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }
  out.push_back('"');
  return out;
}

struct JsonValue {
  enum class Kind { Null, Bool, Int, Float, String, Array, Object };
  Kind kind = Kind::Null;
  bool bool_val = false;
  int64_t int_val = 0;
  double float_val = 0.0;
  std::string str_val;
  std::vector<JsonValue> arr_val;
  std::unordered_map<std::string, JsonValue> obj_val;

  const JsonValue *get(std::string_view key) const {
    if (kind != Kind::Object) return nullptr;
    auto it = obj_val.find(std::string(key));
    return (it != obj_val.end()) ? &it->second : nullptr;
  }
  std::string as_string(std::string def = {}) const {
    return (kind == Kind::String) ? str_val : def;
  }
  int64_t as_int(int64_t def = 0) const {
    if (kind == Kind::Int) return int_val;
    if (kind == Kind::Float) return static_cast<int64_t>(float_val);
    return def;
  }
  std::string format_id() const {
    if (kind == Kind::Int) return std::to_string(int_val);
    if (kind == Kind::String) return json_escape(str_val);
    return "null";
  }
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view s) : s_(s) {}
  bool parse(JsonValue &out) {
    skip_ws();
    if (!parse_value(out)) return false;
    skip_ws();
    return pos_ == s_.size();
  }

 private:
  std::string_view s_;
  size_t pos_ = 0;

  void skip_ws() {
    while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
  }
  bool consume(char c) {
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }
  bool parse_string(std::string &out) {
    skip_ws();
    if (pos_ >= s_.size() || s_[pos_] != '"') return false;
    ++pos_;
    out.clear();
    while (pos_ < s_.size()) {
      char c = s_[pos_++];
      if (c == '"') return true;
      if (c == '\\') {
        if (pos_ >= s_.size()) return false;
        char e = s_[pos_++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            if (pos_ + 4 > s_.size()) return false;
            unsigned cp = 0;
            for (int k = 0; k < 4; ++k) {
              char h = s_[pos_++];
              cp <<= 4;
              if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
              else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
              else return false;
            }
            if (cp < 0x80) {
              out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
              out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
              out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
              out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
              out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
              out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            break;
          }
          default: out.push_back(e); break;
        }
      } else {
        out.push_back(c);
      }
    }
    return false;
  }
  bool parse_value(JsonValue &out) {
    skip_ws();
    if (pos_ >= s_.size()) return false;
    char c = s_[pos_];
    if (c == '"') {
      out.kind = JsonValue::Kind::String;
      return parse_string(out.str_val);
    }
    if (c == '{') {
      ++pos_;
      out.kind = JsonValue::Kind::Object;
      if (consume('}')) return true;
      while (true) {
        std::string key;
        if (!parse_string(key) || !consume(':')) return false;
        JsonValue val;
        if (!parse_value(val)) return false;
        out.obj_val.emplace(std::move(key), std::move(val));
        if (consume(',')) continue;
        return consume('}');
      }
    }
    if (c == '[') {
      ++pos_;
      out.kind = JsonValue::Kind::Array;
      if (consume(']')) return true;
      while (true) {
        JsonValue val;
        if (!parse_value(val)) return false;
        out.arr_val.push_back(std::move(val));
        if (consume(',')) continue;
        return consume(']');
      }
    }
    if (s_.substr(pos_).starts_with("true")) {
      pos_ += 4;
      out.kind = JsonValue::Kind::Bool;
      out.bool_val = true;
      return true;
    }
    if (s_.substr(pos_).starts_with("false")) {
      pos_ += 5;
      out.kind = JsonValue::Kind::Bool;
      out.bool_val = false;
      return true;
    }
    if (s_.substr(pos_).starts_with("null")) {
      pos_ += 4;
      out.kind = JsonValue::Kind::Null;
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      size_t start = pos_;
      if (s_[pos_] == '-') ++pos_;
      while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
      bool is_float = false;
      if (pos_ < s_.size() && s_[pos_] == '.') {
        is_float = true;
        ++pos_;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
      }
      if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
        is_float = true;
        ++pos_;
        if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
      }
      std::string num(s_.substr(start, pos_ - start));
      if (is_float) {
        out.kind = JsonValue::Kind::Float;
        out.float_val = std::strtod(num.c_str(), nullptr);
      } else {
        out.kind = JsonValue::Kind::Int;
        out.int_val = std::strtoll(num.c_str(), nullptr, 10);
      }
      return true;
    }
    return false;
  }
};

void write_lsp_message(const std::string &body) {
  std::fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", body.size(), body.c_str());
  std::fflush(stdout);
}


// `file://` URI for an absolute path, percent-encoding every byte outside
// RFC 3986 unreserved characters and `/` (the inverse of the decoding done
// for incoming document URIs).
std::string path_to_uri(const std::string &path) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out = "file://";
  for (unsigned char c : path) {
    if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xF]);
    }
  }
  return out;
}

// The `import_alias` def whose bound identifier (child 0) has `lhs_id`.
const syntax::SyntaxObj *find_import_alias_by_lhs(const syntax::SyntaxObj *node, uint64_t lhs_id) {
  if (!node) return nullptr;
  if (node->symbol == "import_alias" && node->children.size() == 2 && node->children[0] &&
      node->children[0]->node_id == lhs_id && node->children[1]) {
    return node;
  }
  for (const syntax::SyntaxObj *c : node->children) {
    if (const syntax::SyntaxObj *found = find_import_alias_by_lhs(c, lhs_id)) return found;
  }
  return nullptr;
}

const syntax::SyntaxObj *find_node_by_id(const syntax::SyntaxObj *node, uint64_t target_id) {
  if (!node) return nullptr;
  if (node->node_id == target_id) return node;
  for (const syntax::SyntaxObj *c : node->children) {
    if (const syntax::SyntaxObj *found = find_node_by_id(c, target_id)) return found;
  }
  return nullptr;
}

// A `relatedInformation` entry for a located note (or a diagnostic in another file).
void write_related_info(std::ostringstream &oss, const std::string &uri, const diag::SourceRange &r,
                        const std::string &message) {
  std::string note_uri = uri;
  if (r.file) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(r.file, ec);
    note_uri = path_to_uri(ec ? std::string(r.file) : abs.lexically_normal().string());
  }
  int nl = r.line - 1, nc = r.col < 0 ? 0 : r.col;
  int nel = nl, nec = nc + 1;
  if (r.end_line > 0 && (r.end_line > r.line || (r.end_line == r.line && r.end_col > r.col))) {
    nel = r.end_line - 1;
    nec = r.end_col;
  }
  oss << "{\"location\":{\"uri\":" << json_escape(note_uri) << ",\"range\":{\"start\":{\"line\":"
      << nl << ",\"character\":" << nc << "},\"end\":{\"line\":" << nel << ",\"character\":" << nec
      << "}}},\"message\":" << json_escape(message) << "}";
}

std::string format_diagnostics_json(const std::string &uri,
                                    const std::vector<diag::Diagnostic> &diags) {
  std::ostringstream oss;
  oss << "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":"
      << json_escape(uri) << ",\"diagnostics\":[";
  bool first = true;
  for (const diag::Diagnostic &d : diags) {
    int line = 0, col = 0, end_line = 0, end_col = 0;
    diag::lsp_range(d, &line, &col, &end_line, &end_col);
    int severity = d.severity == diag::Severity::Error     ? 1
                   : d.severity == diag::Severity::Warning ? 2
                                                           : 3;
    // A diagnostic in another file (an imported module) is shown at the top of
    // this document, so its message says where it is.
    bool elsewhere = d.range.file && d.range.line > 0;
    std::string message = d.message;
    if (elsewhere) {
      message = diag::strf("%s:%d:%d: ", d.range.file, d.range.line, d.range.col) + message;
    }
    if (!first) oss << ",";
    first = false;
    oss << "{\"range\":{\"start\":{\"line\":" << line << ",\"character\":" << col
        << "},\"end\":{\"line\":" << end_line << ",\"character\":" << end_col
        << "}},\"severity\":" << severity << ",\"source\":\"langwb\",\"message\":"
        << json_escape(message);
    // The location itself (when elsewhere), then the notes (call chains, macro
    // provenance), as relatedInformation.
    bool first_note = true;
    if (elsewhere) {
      oss << ",\"relatedInformation\":[";
      first_note = false;
      write_related_info(oss, uri, d.range, d.message);
    }
    for (const diag::Diagnostic &n : d.notes) {
      if (n.range.line <= 0) continue;
      oss << (first_note ? ",\"relatedInformation\":[" : ",");
      first_note = false;
      write_related_info(oss, uri, n.range, n.message);
    }
    if (!first_note) oss << "]";
    oss << "}";
  }
  oss << "]}}";
  return oss.str();
}

// --- Document index -------------------------------------------------------
//
// What hover and definition answer for one evaluated document, computed once
// per evaluation from the expanded tree and analysis attributes (and, with
// worker isolation, sent back from the worker), so requests never touch the
// worker's syntax tree.

// 1-based start line, 0-based columns; `size` is the node's text length (the
// narrowest node at a position wins).
struct IndexRange {
  int line = 0, col = 0, end_line = 0, end_col = 0;
  size_t size = 0;
  bool contains(int l, int c) const {
    if (l < line || l > end_line) return false;
    if (l == line && c < col) return false;
    if (l == end_line && c > end_col) return false;
    return true;
  }
};

struct HoverEntry {
  IndexRange r;
  std::string markdown;
};

struct DefEntry {
  IndexRange r;
  std::string uri;
  int line = 0, col = 0, end_col = 0;  // 0-based target line
};

struct DocIndex {
  std::string diagnostics_json;
  std::vector<HoverEntry> hovers;  // children before parents
  std::vector<DefEntry> defs;      // parents before children
  // Constructors and their declarations: definition on a constructor the
  // expanded tree no longer holds (a `case` pattern a macro consumed).
  std::vector<std::pair<std::string, DefEntry>> ctors;
};

IndexRange node_range(const syntax::SyntaxObj *n) {
  IndexRange r;
  r.line = n->line;
  r.col = n->col;
  r.size = n->text.size();
  if (n->end_line > n->line || (n->end_line == n->line && n->end_col > n->col)) {
    r.end_line = n->end_line;
    r.end_col = n->end_col;
  } else {
    r.end_line = n->line;
    r.end_col = n->col + static_cast<int>(n->text.size());
  }
  return r;
}

// A position in this document the user wrote (not an imported module's node,
// not a template node placed at a macro call).
bool indexable(const syntax::SyntaxObj *n) {
  return n && !n->file && !n->synthetic && n->line > 0 && !n->text.empty();
}

std::string hover_markdown(const syntax::SyntaxObj *node, const ifa_bridge::NodeAttr *a) {
  std::ostringstream md;
  md << "```langwb\n" << node->text << " : ";
  for (size_t i = 0; i < a->type_names.size(); ++i) {
    if (i > 0) md << " | ";
    md << ifa_bridge::display_type_name(a->type_names[i]);
  }
  if (a->has_const_i64) md << " = " << a->const_i64;
  if (!a->field_names.empty()) {
    md << " { ";
    for (size_t i = 0; i < a->field_names.size(); ++i) {
      if (i > 0) md << ", ";
      md << a->field_names[i];
    }
    md << " }";
  }
  md << "\n```";
  return md.str();
}

std::string file_uri(const std::string &doc_uri, const char *file) {
  if (!file) return doc_uri;
  std::error_code ec;
  std::filesystem::path abs = std::filesystem::absolute(file, ec);
  return path_to_uri(ec ? std::string(file) : abs.lexically_normal().string());
}

void index_hovers(const syntax::SyntaxObj *node, const ifa_bridge::AnalysisAttrs &attrs,
                  DocIndex &out) {
  if (!node) return;
  for (const syntax::SyntaxObj *c : node->children) index_hovers(c, attrs, out);
  if (!indexable(node)) return;
  if (const auto *a = attrs.get(node); a && a->analyzed) {
    out.hovers.push_back(HoverEntry{node_range(node), hover_markdown(node, a)});
  }
}

void index_defs(const syntax::SyntaxObj *node, const syntax::SyntaxObj *parent,
                const syntax::SyntaxObj *root, const InspectResult &res, const std::string &uri,
                DocIndex &out) {
  if (!node) return;
  if (node->is_terminal && indexable(node)) {
    uint64_t bid = res.attrs.binder_of(node->node_id);
    const syntax::SyntaxObj *binder = (bid != 0) ? find_node_by_id(root, bid) : nullptr;
    // An imported name is bound by a synthetic `import_alias` def (no
    // location) whose right-hand side refers to the module's own binder.
    for (int hops = 0; binder && binder->line <= 0 && hops < 8; ++hops) {
      const syntax::SyntaxObj *alias = find_import_alias_by_lhs(root, bid);
      if (!alias) break;
      bid = res.attrs.binder_of(alias->children[1]->node_id);
      binder = (bid != 0) ? find_node_by_id(root, bid) : nullptr;
    }
    if (binder && binder->line > 0) {
      int bcol = std::max(0, binder->col);
      out.defs.push_back(DefEntry{node_range(node), file_uri(uri, binder->file), binder->line - 1,
                                  bcol, bcol + static_cast<int>(binder->text.size())});
    } else if (bid == 0 && parent && parent->core.kind == parse::CoreFormKind::Ident) {
      // A constructor (an identifier reference, not a field name) goes to its
      // `type` declaration (B8).
      for (const ifa_bridge::AdtVariant &v : res.variants) {
        if (v.name == node->text && v.line > 0 && v.visible_to(node->scopes)) {
          int vcol = std::max(0, v.col);
          out.defs.push_back(DefEntry{node_range(node), file_uri(uri, v.file), v.line - 1, vcol,
                                      vcol + static_cast<int>(v.name.size())});
          break;
        }
      }
    }
  }
  for (const syntax::SyntaxObj *c : node->children) index_defs(c, node, root, res, uri, out);
}

DocIndex build_doc_index(const std::string &uri, const InspectResult &res) {
  DocIndex idx;
  // Warnings are published for a document that evaluates, too.
  idx.diagnostics_json = format_diagnostics_json(uri, res.diagnostics);
  if (res.ok && res.expanded) {
    index_hovers(res.expanded, res.attrs, idx);
    index_defs(res.expanded, nullptr, res.expanded, res, uri, idx);
    for (const ifa_bridge::AdtVariant &v : res.variants) {
      if (v.line <= 0) continue;
      int vcol = std::max(0, v.col);
      idx.ctors.emplace_back(v.name, DefEntry{{}, file_uri(uri, v.file), v.line - 1, vcol,
                                              vcol + static_cast<int>(v.name.size())});
    }
  }
  return idx;
}

std::string range_json(int line, int col, int end_line, int end_col) {
  std::ostringstream o;
  o << "{\"start\":{\"line\":" << line << ",\"character\":" << col << "},\"end\":{\"line\":"
    << end_line << ",\"character\":" << end_col << "}}";
  return o.str();
}

// The hover result for 1-based `line`, 0-based `col` (the narrowest node,
// first in tree order on ties), or "null".
std::string hover_result(const DocIndex &idx, int line, int col) {
  const HoverEntry *best = nullptr;
  for (const HoverEntry &h : idx.hovers) {
    if (h.r.contains(line, col) && (!best || h.r.size < best->r.size)) best = &h;
  }
  if (!best) return "null";
  return "{\"contents\":{\"kind\":\"markdown\",\"value\":" + json_escape(best->markdown) +
         "},\"range\":" +
         range_json(best->r.line - 1, best->r.col, best->r.end_line - 1, best->r.end_col) + "}";
}

// The identifier at 1-based `line`, 0-based `col` of `text`, unless it is
// inside a string literal or followed by `:` (a record field name).
std::string constructor_word_at(const std::string &text, int line, int col) {
  size_t pos = 0;
  for (int l = 1; l < line && pos != std::string::npos; ++l) {
    pos = text.find('\n', pos);
    if (pos != std::string::npos) ++pos;
  }
  if (pos == std::string::npos) return {};
  size_t eol = text.find('\n', pos);
  std::string ln = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
  auto is_word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
  if (col < 0 || static_cast<size_t>(col) > ln.size()) return {};
  size_t b = static_cast<size_t>(col), e = b;
  while (b > 0 && is_word(ln[b - 1])) --b;
  while (e < ln.size() && is_word(ln[e])) ++e;
  bool in_string = false;
  for (size_t i = 0; i < b; ++i) {
    if (ln[i] == '\\' && in_string) {
      ++i;
    } else if (ln[i] == '"') {
      in_string = !in_string;
    }
  }
  if (in_string) return {};
  size_t after = e;
  while (after < ln.size() && std::isspace(static_cast<unsigned char>(ln[after]))) ++after;
  if (after < ln.size() && ln[after] == ':' && (after + 1 >= ln.size() || ln[after + 1] != '=')) {
    return {};
  }
  return ln.substr(b, e - b);
}

// The definition result (the narrowest token, last in tree order on ties).
std::string definition_result(const DocIndex &idx, const std::string &text, int line, int col) {
  const DefEntry *best = nullptr;
  for (const DefEntry &d : idx.defs) {
    if (d.r.contains(line, col) && (!best || d.r.size <= best->r.size)) best = &d;
  }
  if (!best) {
    // Not inside a comment (removed here), a string, or as a record field
    // name (`{ Some: 1 }`).
    std::string w = constructor_word_at(Workbench::strip_comments(text), line, col);
    for (const auto &[name, d] : idx.ctors) {
      if (!w.empty() && name == w) best = &d;
    }
  }
  if (!best) return "null";
  return "{\"uri\":" + json_escape(best->uri) +
         ",\"range\":" + range_json(best->line, best->col, best->line, best->end_col) + "}";
}

}  // namespace

bool Workbench::inspect_document(std::string_view src, InspectResult *out,
                                 const std::string &source_path) {
  if (!out) return false;
  *out = InspectResult{};
  diag::CollectingSink sink;
  bool ok = false;
  {
    diag::ScopedSink scoped(sink);
    EvalOptions opts;
    opts.source_path = source_path;
    ok = eval_impl(src, nullptr, nullptr, opts, nullptr, nullptr, out);
  }
  out->diagnostics = std::move(sink.diags);
  out->ok = ok;
  return ok;
}

namespace {

std::string uri_to_path(const std::string &uri) {
  std::string doc_path;
  if (uri.rfind("file://", 0) != 0) return doc_path;
  std::string_view raw = std::string_view(uri).substr(7);
  doc_path.reserve(raw.size());
  auto hex_val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '%' && i + 2 < raw.size()) {
      int hi = hex_val(raw[i + 1]);
      int lo = hex_val(raw[i + 2]);
      if (hi >= 0 && lo >= 0) {
        doc_path.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    doc_path.push_back(raw[i]);
  }
  return doc_path;
}

// --- Index serialization (worker -> server) -------------------------------
//
// Records are `<tag> <ints...> <len>\n<len bytes>`; the stream ends with `E\n`
// (a stream without it is from a worker that died).

void put_record(std::string &out, char tag, std::initializer_list<long> ints,
                const std::string &bytes) {
  out += tag;
  for (long v : ints) out += " " + std::to_string(v);
  out += " " + std::to_string(bytes.size()) + "\n";
  out += bytes;
}

std::string serialize_index(const DocIndex &idx) {
  std::string out;
  put_record(out, 'D', {}, idx.diagnostics_json);
  for (const HoverEntry &h : idx.hovers) {
    put_record(out, 'H', {h.r.line, h.r.col, h.r.end_line, h.r.end_col, static_cast<long>(h.r.size)},
               h.markdown);
  }
  for (const DefEntry &d : idx.defs) {
    put_record(out, 'F',
               {d.r.line, d.r.col, d.r.end_line, d.r.end_col, static_cast<long>(d.r.size), d.line,
                d.col, d.end_col},
               d.uri);
  }
  for (const auto &[name, d] : idx.ctors) {
    put_record(out, 'C', {d.line, d.col, d.end_col, static_cast<long>(name.size())}, name + d.uri);
  }
  out += "E\n";
  return out;
}

bool parse_index(const std::string &in, DocIndex *idx) {
  size_t pos = 0;
  while (pos < in.size()) {
    size_t nl = in.find('\n', pos);
    if (nl == std::string::npos) return false;
    std::istringstream hdr(in.substr(pos, nl - pos));
    char tag = 0;
    hdr >> tag;
    if (tag == 'E') return true;
    std::vector<long> ints;
    long v;
    while (hdr >> v) ints.push_back(v);
    if (ints.empty()) return false;
    size_t len = static_cast<size_t>(ints.back());
    ints.pop_back();
    if (nl + 1 + len > in.size()) return false;
    std::string bytes = in.substr(nl + 1, len);
    pos = nl + 1 + len;
    auto rng = [&](size_t i) {
      IndexRange r;
      r.line = static_cast<int>(ints[i]);
      r.col = static_cast<int>(ints[i + 1]);
      r.end_line = static_cast<int>(ints[i + 2]);
      r.end_col = static_cast<int>(ints[i + 3]);
      r.size = static_cast<size_t>(ints[i + 4]);
      return r;
    };
    if (tag == 'D' && ints.empty()) {
      idx->diagnostics_json = std::move(bytes);
    } else if (tag == 'H' && ints.size() == 5) {
      idx->hovers.push_back(HoverEntry{rng(0), std::move(bytes)});
    } else if (tag == 'F' && ints.size() == 8) {
      idx->defs.push_back(DefEntry{rng(0), std::move(bytes), static_cast<int>(ints[5]),
                                   static_cast<int>(ints[6]), static_cast<int>(ints[7])});
    } else if (tag == 'C' && ints.size() == 4) {
      size_t nlen = static_cast<size_t>(ints[3]);
      if (nlen > bytes.size()) return false;
      idx->ctors.emplace_back(bytes.substr(0, nlen),
                              DefEntry{{}, bytes.substr(nlen), static_cast<int>(ints[0]),
                                       static_cast<int>(ints[1]), static_cast<int>(ints[2])});
    } else {
      return false;
    }
  }
  return false;
}

// A diagnostic at the top of the document, for an evaluation that did not
// finish.
std::string failure_diagnostics_json(const std::string &uri, const std::string &message) {
  diag::Diagnostic d;
  d.severity = diag::Severity::Error;
  d.code = "lsp";
  d.message = message;
  return format_diagnostics_json(uri, {d});
}

// --- Worker ----------------------------------------------------------------

long lsp_timeout_ms() {
  if (const char *e = std::getenv("LANGWB_LSP_TIMEOUT_MS")) {
    long v = std::strtol(e, nullptr, 10);
    if (v > 0) return v;
  }
  // An MLite document takes ~2.5 s to evaluate on its own (mostly loading the
  // `.lang`), so the default leaves room for a loaded machine.
  return 10000;
}

long now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

#ifdef DEBUG
// `fork` is safe only while this process is single-threaded (no Boehm marker
// threads or LLVM threads to lose in the child; docs/STEP12_DIAGNOSTICS.md
// §2.5).
void assert_single_threaded() {
#ifdef __APPLE__
  thread_act_array_t threads = nullptr;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return;
  for (mach_msg_type_number_t i = 0; i < count; ++i) mach_port_deallocate(mach_task_self(), threads[i]);
  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                count * sizeof(thread_act_t));
  if (count != 1) {
    std::fprintf(stderr, "langwb --lsp: %u threads before fork\n", count);
    std::abort();
  }
  return;
#endif
  FILE *f = std::fopen("/proc/self/status", "r");
  if (!f) return;
  char line[256];
  while (std::fgets(line, sizeof line, f)) {
    int n = 0;
    if (std::sscanf(line, "Threads: %d", &n) == 1 && n != 1) {
      std::fprintf(stderr, "langwb --lsp: %d threads before fork\n", n);
      std::abort();
    }
  }
  std::fclose(f);
}
#endif

struct Worker {
  pid_t pid = -1;
  int fd = -1;
  std::string uri;
  std::string out;
  long deadline_ms = 0;
};

// Evaluates `text` in a forked child, which builds its own Workbench and
// sends back the document index.
bool start_worker(Worker *w, const LspOptions &opts, const std::string &uri,
                  const std::string &text) {
  int fds[2];
  if (pipe(fds) != 0) return false;
#ifdef DEBUG
  assert_single_threaded();
#endif
  GC_atfork_prepare();
  pid_t pid = fork();
  if (pid == 0) {
    GC_atfork_child();
    close(fds[0]);
    // The child must not write to the LSP stream.
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, STDOUT_FILENO);
    close(STDIN_FILENO);
    std::string data;
    {
      Workbench wb = opts.bare ? Workbench::create_bare() : Workbench();
      InspectResult res;
      wb.inspect_document(text, &res, uri_to_path(uri));
      data = serialize_index(build_doc_index(uri, res));
    }
    size_t off = 0;
    while (off < data.size()) {
      ssize_t n = write(fds[1], data.data() + off, data.size() - off);
      if (n <= 0) break;
      off += static_cast<size_t>(n);
    }
    _exit(0);
  }
  GC_atfork_parent();
  close(fds[1]);
  if (pid < 0) {
    close(fds[0]);
    return false;
  }
  *w = Worker{pid, fds[0], uri, {}, now_ms() + lsp_timeout_ms()};
  return true;
}

void stop_worker(Worker *w) {
  if (w->pid > 0) {
    kill(w->pid, SIGKILL);
    waitpid(w->pid, nullptr, 0);
  }
  if (w->fd >= 0) close(w->fd);
  *w = Worker{};
}

// Reads LSP messages from fd 0 without blocking the event loop.
struct StdinReader {
  std::string buf;
  bool eof = false;

  void fill() {
    char tmp[65536];
    ssize_t n = read(STDIN_FILENO, tmp, sizeof tmp);
    if (n <= 0) {
      eof = true;
      return;
    }
    buf.append(tmp, static_cast<size_t>(n));
  }

  bool next(std::string *body) {
    size_t hdr_end = buf.find("\r\n\r\n");
    size_t sep = 4;
    if (hdr_end == std::string::npos) {
      hdr_end = buf.find("\n\n");
      sep = 2;
      if (hdr_end == std::string::npos) return false;
    }
    size_t len = 0;
    size_t cl = buf.find("Content-Length:");
    if (cl == std::string::npos || cl > hdr_end) {
      buf.erase(0, hdr_end + sep);
      return false;
    }
    len = static_cast<size_t>(std::strtoull(buf.c_str() + cl + 15, nullptr, 10));
    if (buf.size() < hdr_end + sep + len) return false;
    *body = buf.substr(hdr_end + sep, len);
    buf.erase(0, hdr_end + sep + len);
    return true;
  }
};

}  // namespace

int run_lsp_server(const LspOptions &opts) {
  // Single-threaded router: documents are evaluated in forked workers (one at
  // a time, with a timeout), so a guest macro that loops or crashes cannot take
  // the server down; requests are answered from each document's last index.
  std::unordered_map<std::string, std::string> docs;
  std::unordered_map<std::string, DocIndex> inspected;
  std::vector<std::string> pending;  // documents to (re)evaluate, oldest first
  std::vector<std::pair<std::string, std::string>> deferred;  // (uri, request body)
  Worker worker;
  StdinReader in;
  bool shutdown_requested = false;
  signal(SIGPIPE, SIG_IGN);

  auto busy = [&](const std::string &uri) {
    return (worker.pid > 0 && worker.uri == uri) ||
           std::find(pending.begin(), pending.end(), uri) != pending.end();
  };

  auto answer = [&](const std::string &body) {
    JsonValue msg;
    if (!JsonParser(body).parse(msg) || msg.kind != JsonValue::Kind::Object) return;
    std::string method = msg.get("method") ? msg.get("method")->as_string() : "";
    const JsonValue *id_val = msg.get("id");
    std::string id_json = id_val ? id_val->format_id() : "null";
    const JsonValue *params = msg.get("params");
    const JsonValue *td = params ? params->get("textDocument") : nullptr;
    const JsonValue *pos = params ? params->get("position") : nullptr;
    std::string uri = (td && td->get("uri")) ? td->get("uri")->as_string() : "";
    int line = (pos && pos->get("line")) ? static_cast<int>(pos->get("line")->as_int()) : 0;
    int ch = (pos && pos->get("character")) ? static_cast<int>(pos->get("character")->as_int()) : 0;
    std::string result = "null";
    if (auto it = inspected.find(uri); it != inspected.end()) {
      result = method == "textDocument/hover" ? hover_result(it->second, line + 1, ch)
                                              : definition_result(it->second, docs[uri], line + 1, ch);
    }
    write_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + id_json + ",\"result\":" + result + "}");
  };

  auto flush_deferred = [&]() {
    std::vector<std::pair<std::string, std::string>> keep;
    for (auto &[uri, body] : deferred) {
      if (busy(uri)) {
        keep.emplace_back(uri, std::move(body));
      } else {
        answer(body);
      }
    }
    deferred = std::move(keep);
  };

  auto finish_worker = [&](bool timed_out) {
    std::string uri = worker.uri;
    DocIndex idx;
    std::string failure;
    if (timed_out) {
      stop_worker(&worker);
      failure = "evaluation of this document timed out after " +
                std::to_string(lsp_timeout_ms()) + " ms";
    } else {
      int status = 0;
      waitpid(worker.pid, &status, 0);
      worker.pid = -1;
      close(worker.fd);
      std::string out = std::move(worker.out);
      worker = Worker{};
      if (!parse_index(out, &idx)) {
        failure = WIFSIGNALED(status)
                      ? "evaluation of this document crashed (signal " +
                            std::to_string(WTERMSIG(status)) + ")"
                      : "evaluation of this document crashed";
      }
    }
    if (!failure.empty()) {
      idx = DocIndex{};
      idx.diagnostics_json = failure_diagnostics_json(uri, failure);
    }
    if (docs.count(uri)) {
      write_lsp_message(idx.diagnostics_json);
      inspected[uri] = std::move(idx);
    }
    flush_deferred();
  };

  auto handle = [&](const std::string &body) -> int {  // >= 0: exit status
    JsonValue msg;
    if (!JsonParser(body).parse(msg) || msg.kind != JsonValue::Kind::Object) return -1;
    std::string method = msg.get("method") ? msg.get("method")->as_string() : "";
    const JsonValue *id_val = msg.get("id");
    bool has_id = (id_val != nullptr);
    std::string id_json = has_id ? id_val->format_id() : "null";
    const JsonValue *params = msg.get("params");
    const JsonValue *td = params ? params->get("textDocument") : nullptr;
    std::string uri = (td && td->get("uri")) ? td->get("uri")->as_string() : "";

    if (method == "initialize") {
      std::ostringstream resp;
      resp << "{\"jsonrpc\":\"2.0\",\"id\":" << id_json
           << ",\"result\":{\"capabilities\":{\"positionEncoding\":\"utf-8\","
              "\"textDocumentSync\":1,\"hoverProvider\":true,\"definitionProvider\":true},"
              "\"serverInfo\":{\"name\":\"langwb-lsp\",\"version\":\"0.1\"}}}";
      write_lsp_message(resp.str());
    } else if (method == "initialized") {
      // No-op notification.
    } else if (method == "shutdown") {
      shutdown_requested = true;
      if (has_id) write_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + id_json + ",\"result\":null}");
    } else if (method == "exit") {
      return shutdown_requested ? 0 : 1;
    } else if (method == "textDocument/didOpen" || method == "textDocument/didChange") {
      std::string text;
      if (method == "textDocument/didOpen") {
        if (td && td->get("text")) text = td->get("text")->as_string();
      } else {
        const JsonValue *cc = params ? params->get("contentChanges") : nullptr;
        if (cc && cc->kind == JsonValue::Kind::Array && !cc->arr_val.empty()) {
          if (const JsonValue *t = cc->arr_val.back().get("text")) text = t->as_string();
        }
      }
      docs[uri] = text;
      // A newer version cancels the evaluation of an older one.
      if (worker.pid > 0 && worker.uri == uri) stop_worker(&worker);
      if (std::find(pending.begin(), pending.end(), uri) == pending.end()) pending.push_back(uri);
    } else if (method == "textDocument/didClose") {
      docs.erase(uri);
      inspected.erase(uri);
      pending.erase(std::remove(pending.begin(), pending.end(), uri), pending.end());
      if (worker.pid > 0 && worker.uri == uri) stop_worker(&worker);
      flush_deferred();
    } else if ((method == "textDocument/hover" || method == "textDocument/definition") && has_id) {
      // Answered once the document's latest version has been evaluated.
      if (busy(uri)) {
        deferred.emplace_back(uri, body);
      } else {
        answer(body);
      }
    } else if (has_id) {
      write_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + id_json + ",\"result\":null}");
    }
    return -1;
  };

  for (;;) {
    std::string body;
    while (in.next(&body)) {
      int rc = handle(body);
      if (rc >= 0) {
        stop_worker(&worker);
        return rc;
      }
    }
    if (in.eof) break;
    // Coalesce queued edits: take any input already waiting before starting
    // an evaluation.
    struct pollfd pfd0 = {STDIN_FILENO, POLLIN, 0};
    if (worker.pid <= 0 && !pending.empty() && poll(&pfd0, 1, 0) > 0) {
      in.fill();
      continue;
    }
    if (worker.pid <= 0 && !pending.empty()) {
      std::string uri = pending.front();
      pending.erase(pending.begin());
      if (!start_worker(&worker, opts, uri, docs[uri])) {
        DocIndex idx;
        idx.diagnostics_json = failure_diagnostics_json(uri, "cannot start an evaluation worker");
        write_lsp_message(idx.diagnostics_json);
        inspected[uri] = std::move(idx);
        flush_deferred();
      }
      continue;
    }
    struct pollfd pfds[2] = {{STDIN_FILENO, POLLIN, 0}, {worker.fd, POLLIN, 0}};
    int nfds = worker.pid > 0 ? 2 : 1;
    int timeout = -1;
    if (worker.pid > 0) timeout = static_cast<int>(std::max(0L, worker.deadline_ms - now_ms()));
    int r = poll(pfds, nfds, timeout);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0 && worker.pid > 0) {
      finish_worker(/*timed_out=*/true);
      continue;
    }
    if (nfds == 2 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
      char tmp[65536];
      ssize_t n = read(worker.fd, tmp, sizeof tmp);
      if (n > 0) {
        worker.out.append(tmp, static_cast<size_t>(n));
      } else {
        finish_worker(/*timed_out=*/false);
      }
    }
    if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) in.fill();
  }
  stop_worker(&worker);
  return 0;
}

}  // namespace langwb
