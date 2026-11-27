#include "workbench/workbench.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
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

bool read_lsp_message(std::string &out_body) {
  size_t content_length = 0;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    if (line.starts_with("Content-Length:")) {
      content_length = static_cast<size_t>(std::strtoull(line.c_str() + 15, nullptr, 10));
    }
  }
  if (content_length == 0 || !std::cin) return false;
  out_body.resize(content_length);
  std::cin.read(out_body.data(), static_cast<std::streamsize>(content_length));
  return static_cast<size_t>(std::cin.gcount()) == content_length;
}

// Finds the innermost SyntaxObj covering 1-indexed `line` and 0-indexed `col`.
// The narrowest node covering (line, col) (terminals only if
// `prefer_terminal`); ties go to the deeper / later node in tree order. As in
// `find_annotated_at`, a macro may relocate a template node onto a user
// node's position, so the narrowest match is the user's token.
void find_node_at_impl(const syntax::SyntaxObj *node, int line, int col, bool prefer_terminal,
                       const syntax::SyntaxObj *&best, size_t &best_len) {
  if (!node) return;
  // Nodes from imported modules (`file` set) are spliced into the analyzed
  // program but are not positions in this document; their children are still
  // searched (an imported macro's template can hold spliced program nodes).
  if (!node->file && !node->synthetic && node->line == line && !node->text.empty() &&
      (!prefer_terminal || node->is_terminal)) {
    int start_col = node->col;
    int end_col = start_col + static_cast<int>(node->text.size());
    if (col >= start_col && col <= end_col && node->text.size() <= best_len) {
      best = node;
      best_len = node->text.size();
    }
  }
  for (const syntax::SyntaxObj *c : node->children) {
    find_node_at_impl(c, line, col, prefer_terminal, best, best_len);
  }
}

const syntax::SyntaxObj *find_node_at(const syntax::SyntaxObj *node, int line, int col,
                                      bool prefer_terminal) {
  const syntax::SyntaxObj *best = nullptr;
  size_t best_len = static_cast<size_t>(-1);
  find_node_at_impl(node, line, col, prefer_terminal, best, best_len);
  return best;
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

// The narrowest analyzed node covering (line, col). Spans are measured by a
// node's text, and a macro may give a template node (whose text is the
// template's) the location of a user node, so "first match in tree order"
// can pick a node spanning far past the cursor; the narrowest is the user's.
void find_annotated_at_impl(const syntax::SyntaxObj *node, const ifa_bridge::AnalysisAttrs &attrs,
                            int line, int col, const syntax::SyntaxObj *&best, size_t &best_len) {
  if (!node) return;
  for (const syntax::SyntaxObj *c : node->children) {
    find_annotated_at_impl(c, attrs, line, col, best, best_len);
  }
  if (!node->file && !node->synthetic && node->line == line &&
      !node->text.empty()) {  // see find_node_at
    int start_col = node->col;
    int end_col = start_col + static_cast<int>(node->text.size());
    if (col >= start_col && col <= end_col && node->text.size() < best_len) {
      if (const auto *a = attrs.get(node); a && a->analyzed) {
        best = node;
        best_len = node->text.size();
      }
    }
  }
}

const syntax::SyntaxObj *find_annotated_at(const syntax::SyntaxObj *node,
                                           const ifa_bridge::AnalysisAttrs &attrs, int line,
                                           int col) {
  const syntax::SyntaxObj *best = nullptr;
  size_t best_len = static_cast<size_t>(-1);
  find_annotated_at_impl(node, attrs, line, col, best, best_len);
  return best;
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

int run_lsp_server(Workbench &wb) {
  std::unordered_map<std::string, std::string> docs;
  std::unordered_map<std::string, InspectResult> inspected;

  std::string body;
  bool shutdown_requested = false;
  while (read_lsp_message(body)) {
    JsonValue msg;
    if (!JsonParser(body).parse(msg) || msg.kind != JsonValue::Kind::Object) continue;
    std::string method = msg.get("method") ? msg.get("method")->as_string() : "";
    const JsonValue *id_val = msg.get("id");
    bool has_id = (id_val != nullptr);
    std::string id_json = has_id ? id_val->format_id() : "null";
    const JsonValue *params = msg.get("params");

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
      if (has_id) {
        write_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + id_json + ",\"result\":null}");
      }
    } else if (method == "exit") {
      return shutdown_requested ? 0 : 1;
    } else if (method == "textDocument/didOpen" || method == "textDocument/didChange") {
      const JsonValue *td = params ? params->get("textDocument") : nullptr;
      std::string uri = (td && td->get("uri")) ? td->get("uri")->as_string() : "";
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
      std::string doc_path;
      if (uri.rfind("file://", 0) == 0) {
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
      }
      InspectResult res;
      wb.inspect_document(text, &res, doc_path);
      write_lsp_message(format_diagnostics_json(uri, res.ok ? std::vector<diag::Diagnostic>{} : res.diagnostics));
      inspected[uri] = std::move(res);
    } else if (method == "textDocument/didClose") {
      const JsonValue *td = params ? params->get("textDocument") : nullptr;
      std::string uri = (td && td->get("uri")) ? td->get("uri")->as_string() : "";
      docs.erase(uri);
      inspected.erase(uri);
    } else if (method == "textDocument/hover" && has_id) {
      const JsonValue *td = params ? params->get("textDocument") : nullptr;
      const JsonValue *pos = params ? params->get("position") : nullptr;
      std::string uri = (td && td->get("uri")) ? td->get("uri")->as_string() : "";
      int64_t line = (pos && pos->get("line")) ? pos->get("line")->as_int() : 0;
      int64_t ch = (pos && pos->get("character")) ? pos->get("character")->as_int() : 0;

      auto it = inspected.find(uri);
      if (it != inspected.end() && it->second.ok && it->second.expanded) {
        const syntax::SyntaxObj *node = find_annotated_at(
            it->second.expanded, it->second.attrs, static_cast<int>(line) + 1, static_cast<int>(ch));
        if (node) {
          const auto *a = it->second.attrs.get(node);
          std::ostringstream md;
          md << "```langwb\n" << node->text << " : ";
          for (size_t i = 0; i < a->type_names.size(); ++i) {
            if (i > 0) md << " | ";
            md << a->type_names[i];
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
          write_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + id_json +
                            ",\"result\":{\"contents\":{\"kind\":\"markdown\",\"value\":" +
                            json_escape(md.str()) + "}}}");
          continue;
        }
      }
      write_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + id_json + ",\"result\":null}");
    } else if (method == "textDocument/definition" && has_id) {
      const JsonValue *td = params ? params->get("textDocument") : nullptr;
      const JsonValue *pos = params ? params->get("position") : nullptr;
      std::string uri = (td && td->get("uri")) ? td->get("uri")->as_string() : "";
      int64_t line = (pos && pos->get("line")) ? pos->get("line")->as_int() : 0;
      int64_t ch = (pos && pos->get("character")) ? pos->get("character")->as_int() : 0;

      auto it = inspected.find(uri);
      if (it != inspected.end() && it->second.ok && it->second.expanded) {
        const syntax::SyntaxObj *node = find_node_at(
            it->second.expanded, static_cast<int>(line) + 1, static_cast<int>(ch), true);
        uint64_t bid = node ? it->second.attrs.binder_of(node->node_id) : 0;
        const syntax::SyntaxObj *binder =
            (bid != 0) ? find_node_by_id(it->second.expanded, bid) : nullptr;
        // An imported name is bound by a synthetic `import_alias` def (no
        // location) whose right-hand side refers to the module's own binder.
        for (int hops = 0; binder && binder->line <= 0 && hops < 8; ++hops) {
          const syntax::SyntaxObj *alias = find_import_alias_by_lhs(it->second.expanded, bid);
          if (!alias) break;
          bid = it->second.attrs.binder_of(alias->children[1]->node_id);
          binder = (bid != 0) ? find_node_by_id(it->second.expanded, bid) : nullptr;
        }
        if (binder && binder->line > 0) {
          int bline = binder->line - 1;
          int bcol = std::max(0, binder->col);
          int bend = bcol + static_cast<int>(binder->text.size());
          std::string target_uri = uri;
          if (binder->file) {
            std::error_code ec;
            std::filesystem::path abs = std::filesystem::absolute(binder->file, ec);
            target_uri = path_to_uri(ec ? std::string(binder->file) : abs.lexically_normal().string());
          }
          std::ostringstream resp;
          resp << "{\"jsonrpc\":\"2.0\",\"id\":" << id_json << ",\"result\":{\"uri\":"
               << json_escape(target_uri) << ",\"range\":{\"start\":{\"line\":" << bline
               << ",\"character\":" << bcol << "},\"end\":{\"line\":" << bline
               << ",\"character\":" << bend << "}}}}";
          write_lsp_message(resp.str());
          continue;
        }
      }
      write_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + id_json + ",\"result\":null}");
    } else if (has_id) {
      write_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + id_json + ",\"result\":null}");
    }
  }
  return 0;
}

}  // namespace langwb
