#include "workbench/workbench.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace langwb {

namespace {

struct CaptureStderr {
  int saved_fd = -1;
  int pipe_fds[2] = {-1, -1};
  std::string captured;

  CaptureStderr() {
    std::fflush(stderr);
    if (::pipe(pipe_fds) == 0) {
      int flags = ::fcntl(pipe_fds[0], F_GETFL, 0);
      ::fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
      saved_fd = ::dup(STDERR_FILENO);
      if (saved_fd >= 0) ::dup2(pipe_fds[1], STDERR_FILENO);
    }
  }

  std::string finish() {
    if (saved_fd >= 0) {
      std::fflush(stderr);
      ::dup2(saved_fd, STDERR_FILENO);
      ::close(saved_fd);
      saved_fd = -1;
    }
    if (pipe_fds[1] >= 0) {
      ::close(pipe_fds[1]);
      pipe_fds[1] = -1;
    }
    if (pipe_fds[0] >= 0) {
      char buf[512];
      ssize_t n = 0;
      while ((n = ::read(pipe_fds[0], buf, sizeof(buf))) > 0) {
        captured.append(buf, static_cast<size_t>(n));
      }
      ::close(pipe_fds[0]);
      pipe_fds[0] = -1;
    }
    return captured;
  }

  ~CaptureStderr() { (void)finish(); }
};

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

bool extract_json_string(std::string_view json, std::string_view key, std::string &out,
                         size_t start_pos = 0) {
  std::string needle = "\"" + std::string(key) + "\"";
  size_t pos = json.find(needle, start_pos);
  if (pos == std::string_view::npos) return false;
  pos = json.find(':', pos + needle.size());
  if (pos == std::string_view::npos) return false;
  ++pos;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
  if (pos >= json.size() || json[pos] != '"') return false;
  ++pos;
  out.clear();
  while (pos < json.size()) {
    char c = json[pos++];
    if (c == '\\' && pos < json.size()) {
      char e = json[pos++];
      if (e == 'n') out.push_back('\n');
      else if (e == 'r') out.push_back('\r');
      else if (e == 't') out.push_back('\t');
      else out.push_back(e);
    } else if (c == '"') {
      return true;
    } else {
      out.push_back(c);
    }
  }
  return false;
}

bool extract_json_int(std::string_view json, std::string_view key, int64_t &out,
                      size_t start_pos = 0) {
  std::string needle = "\"" + std::string(key) + "\"";
  size_t pos = json.find(needle, start_pos);
  if (pos == std::string_view::npos) return false;
  pos = json.find(':', pos + needle.size());
  if (pos == std::string_view::npos) return false;
  ++pos;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
  if (pos >= json.size()) return false;
  char *endp = nullptr;
  std::string tmp(json.substr(pos, std::min<size_t>(32, json.size() - pos)));
  long long v = std::strtoll(tmp.c_str(), &endp, 10);
  if (endp == tmp.c_str()) return false;
  out = static_cast<int64_t>(v);
  return true;
}

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
const syntax::SyntaxObj *find_node_at(const syntax::SyntaxObj *node, int line, int col,
                                      bool prefer_terminal) {
  if (!node) return nullptr;
  const syntax::SyntaxObj *best = nullptr;
  for (const syntax::SyntaxObj *c : node->children) {
    if (const syntax::SyntaxObj *found = find_node_at(c, line, col, prefer_terminal)) {
      best = found;
    }
  }
  if (best) return best;
  if (node->line == line && !node->text.empty()) {
    int start_col = node->col;
    int end_col = start_col + static_cast<int>(node->text.size());
    if (col >= start_col && col <= end_col) {
      if (!prefer_terminal || node->is_terminal) return node;
    }
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

const syntax::SyntaxObj *find_annotated_at(const syntax::SyntaxObj *node,
                                           const ifa_bridge::AnalysisAttrs &attrs, int line,
                                           int col) {
  if (!node) return nullptr;
  for (const syntax::SyntaxObj *c : node->children) {
    if (const syntax::SyntaxObj *found = find_annotated_at(c, attrs, line, col)) return found;
  }
  if (node->line == line && !node->text.empty()) {
    int start_col = node->col;
    int end_col = start_col + static_cast<int>(node->text.size());
    if (col >= start_col && col <= end_col) {
      if (const auto *a = attrs.get(node); a && a->analyzed) return node;
    }
  }
  return nullptr;
}

std::string format_diagnostics_json(const std::string &uri, const std::string &diag_text) {
  std::ostringstream oss;
  oss << "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":"
      << json_escape(uri) << ",\"diagnostics\":[";
  bool first = true;
  std::istringstream iss(diag_text);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.empty()) continue;
    int dline = 0;
    int dcol = 0;
    std::string msg = line;
    // Parse `<input>:L:C: error: ...` or `langwb: L:C: ...`.
    size_t c1 = line.find(':');
    if (c1 != std::string::npos) {
      size_t c2 = line.find(':', c1 + 1);
      if (c2 != std::string::npos) {
        int l = std::atoi(line.c_str() + c1 + 1);
        int c = std::atoi(line.c_str() + c2 + 1);
        if (l > 0) {
          dline = l - 1;
          dcol = std::max(0, c);
        }
      }
    }
    if (!first) oss << ",";
    first = false;
    oss << "{\"range\":{\"start\":{\"line\":" << dline << ",\"character\":" << dcol
        << "},\"end\":{\"line\":" << dline << ",\"character\":" << (dcol + 1)
        << "}},\"severity\":1,\"source\":\"langwb\",\"message\":" << json_escape(msg) << "}";
  }
  oss << "]}}";
  return oss.str();
}

}  // namespace

bool Workbench::inspect_document(std::string_view src, InspectResult *out) {
  if (!out) return false;
  *out = InspectResult{};
  CaptureStderr cap;
  EvalOptions opts;
  bool ok = eval_impl(src, nullptr, nullptr, opts, nullptr, nullptr, out);
  out->diagnostics = cap.finish();
  out->ok = ok;
  return ok;
}

int run_lsp_server(Workbench &wb) {
  std::unordered_map<std::string, std::string> docs;
  std::unordered_map<std::string, InspectResult> inspected;

  std::string body;
  bool shutdown_requested = false;
  while (read_lsp_message(body)) {
    std::string method;
    extract_json_string(body, "method", method);
    int64_t req_id = -1;
    bool has_id = extract_json_int(body, "id", req_id);

    if (method == "initialize") {
      std::ostringstream resp;
      resp << "{\"jsonrpc\":\"2.0\",\"id\":" << req_id
           << ",\"result\":{\"capabilities\":{\"textDocumentSync\":1,\"hoverProvider\":true,"
              "\"definitionProvider\":true},\"serverInfo\":{\"name\":\"langwb-lsp\",\"version\":"
              "\"0.1\"}}}";
      write_lsp_message(resp.str());
    } else if (method == "initialized") {
      // No-op notification.
    } else if (method == "shutdown") {
      shutdown_requested = true;
      if (has_id) {
        write_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(req_id) +
                          ",\"result\":null}");
      }
    } else if (method == "exit") {
      return shutdown_requested ? 0 : 1;
    } else if (method == "textDocument/didOpen" || method == "textDocument/didChange") {
      std::string uri, text;
      extract_json_string(body, "uri", uri);
      if (method == "textDocument/didOpen") {
        extract_json_string(body, "text", text);
      } else {
        size_t cc = body.find("\"contentChanges\"");
        if (cc != std::string::npos) extract_json_string(body, "text", text, cc);
      }
      docs[uri] = text;
      InspectResult res;
      wb.inspect_document(text, &res);
      write_lsp_message(format_diagnostics_json(uri, res.ok ? "" : res.diagnostics));
      inspected[uri] = std::move(res);
    } else if (method == "textDocument/didClose") {
      std::string uri;
      extract_json_string(body, "uri", uri);
      docs.erase(uri);
      inspected.erase(uri);
    } else if (method == "textDocument/hover" && has_id) {
      std::string uri;
      int64_t line = 0, ch = 0;
      extract_json_string(body, "uri", uri);
      extract_json_int(body, "line", line);
      extract_json_int(body, "character", ch);

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
          write_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(req_id) +
                            ",\"result\":{\"contents\":{\"kind\":\"markdown\",\"value\":" +
                            json_escape(md.str()) + "}}}");
          continue;
        }
      }
      write_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(req_id) +
                        ",\"result\":null}");
    } else if (method == "textDocument/definition" && has_id) {
      std::string uri;
      int64_t line = 0, ch = 0;
      extract_json_string(body, "uri", uri);
      extract_json_int(body, "line", line);
      extract_json_int(body, "character", ch);

      auto it = inspected.find(uri);
      if (it != inspected.end() && it->second.ok && it->second.expanded) {
        const syntax::SyntaxObj *node = find_node_at(
            it->second.expanded, static_cast<int>(line) + 1, static_cast<int>(ch), true);
        uint64_t bid = node ? it->second.attrs.binder_of(node->node_id) : 0;
        const syntax::SyntaxObj *binder =
            (bid != 0) ? find_node_by_id(it->second.expanded, bid) : nullptr;
        if (binder && binder->line > 0) {
          int bline = binder->line - 1;
          int bcol = std::max(0, binder->col);
          int bend = bcol + static_cast<int>(binder->text.size());
          std::ostringstream resp;
          resp << "{\"jsonrpc\":\"2.0\",\"id\":" << req_id << ",\"result\":{\"uri\":"
               << json_escape(uri) << ",\"range\":{\"start\":{\"line\":" << bline
               << ",\"character\":" << bcol << "},\"end\":{\"line\":" << bline
               << ",\"character\":" << bend << "}}}}";
          write_lsp_message(resp.str());
          continue;
        }
      }
      write_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(req_id) +
                        ",\"result\":null}");
    } else if (has_id) {
      write_lsp_message("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(req_id) +
                        ",\"result\":null}");
    }
  }
  return 0;
}

}  // namespace langwb
