#include <gc/gc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  WB_TAG_INT = 0,
  WB_TAG_BOOL = 1,
  WB_TAG_STR = 2,
  WB_TAG_LIST = 3,
  WB_TAG_OBJ = 4,
};

typedef void (*WbEmitSinkFn)(const char *data, size_t len);
static WbEmitSinkFn g_wb_emit_sink = NULL;
static int g_wb_stdout_dirty = 0;

void _CG_wb_set_emit_sink(WbEmitSinkFn fn) { g_wb_emit_sink = fn; }

int _CG_wb_consume_stdout_dirty(void) {
  int d = g_wb_stdout_dirty;
  g_wb_stdout_dirty = 0;
  return d;
}

// Note: _CG_wb_print_begin/_end save and restore g_wb_emit_sink via a single
// slot, assuming value printers do not recursively invoke print().
void *_CG_wb_print_begin(void) {
  void *saved = (void *)(uintptr_t)g_wb_emit_sink;
  g_wb_emit_sink = NULL;
  return saved;
}

int64_t _CG_wb_print_end(void *saved, int64_t nl) {
  g_wb_emit_sink = (WbEmitSinkFn)(uintptr_t)saved;
  if (nl) {
    putchar('\n');
    g_wb_stdout_dirty = 0;
  } else {
    g_wb_stdout_dirty = 1;
  }
  fflush(stdout);
  return 0;
}

// Emits a 0x01 marker to the JIT result pipe (or a newline to stdout in AOT)
// when prior print() calls left stdout without a trailing newline.
void _CG_wb_emit_dirty_marker(void) {
  if (!g_wb_stdout_dirty) return;
  g_wb_stdout_dirty = 0;
  if (g_wb_emit_sink) {
    g_wb_emit_sink("\x01", 1);
  } else {
    putchar('\n');
    fflush(stdout);
  }
}

static void wb_emit_bytes(const char *data, size_t len) {
  if (!data || len == 0) return;
  if (g_wb_emit_sink) {
    g_wb_emit_sink(data, len);
  } else {
    fwrite(data, 1, len, stdout);
  }
}

void _CG_wb_emit_str_unquoted(const char *s) {
  if (s) wb_emit_bytes(s, strlen(s));
}

static void wb_emit_cstr(const char *s) {
  if (s) wb_emit_bytes(s, strlen(s));
}

void _CG_gc_init(void) { GC_INIT(); }
void _CG_set_argv(int argc, char **argv) {
  (void)argc;
  (void)argv;
}
void _CG_event_loop_run(void) {}

int64_t _CG_wb_pow_i64(int64_t base, int64_t exp) {
  if (exp < 0) return 0;
  uint64_t res = 1;
  uint64_t b = (uint64_t)base;
  uint64_t e = (uint64_t)exp;
  while (e > 0) {
    if (e & 1ULL) res *= b;
    e >>= 1;
    if (e > 0) b *= b;
  }
  return (int64_t)res;
}

typedef struct WbCons {
  int64_t elem_tag;
  int64_t head;
  struct WbCons *tail;
} WbCons;

int64_t _CG_wb_nil(void) { return 0; }

int64_t _CG_wb_cons_tagged(int64_t tag, int64_t h, int64_t t) {
  WbCons *c = (WbCons *)GC_MALLOC(sizeof(WbCons));
  c->elem_tag = tag;
  c->head = h;
  c->tail = (WbCons *)(uintptr_t)t;
  return (int64_t)(uintptr_t)c;
}

int64_t _CG_wb_cons(int64_t h, int64_t t) { return _CG_wb_cons_tagged(WB_TAG_INT, h, t); }

static const char *wb_tag_name(int64_t tag) {
  switch (tag) {
    case WB_TAG_INT: return "int64";
    case WB_TAG_BOOL: return "bool";
    case WB_TAG_STR: return "string";
    case WB_TAG_LIST: return "list";
    case WB_TAG_OBJ: return "object";
    default: return "unknown";
  }
}

static void wb_tag_fail(const char *fn, int64_t expected, int64_t got) {
  fprintf(stderr, "langwb: %s expected %s element, got %s\n", fn, wb_tag_name(expected),
          wb_tag_name(got));
  exit(1);
}

int64_t _CG_wb_head(int64_t xs) {
  WbCons *c = (WbCons *)(uintptr_t)xs;
  if (!c) return 0;
  if (c->elem_tag != WB_TAG_INT) wb_tag_fail("head", WB_TAG_INT, c->elem_tag);
  return c->head;
}

int64_t _CG_wb_head_tag(int64_t xs) {
  WbCons *c = (WbCons *)(uintptr_t)xs;
  return c ? c->elem_tag : -1;
}

const char *_CG_wb_head_str(int64_t xs) {
  WbCons *c = (WbCons *)(uintptr_t)xs;
  if (!c) return "";
  if (c->elem_tag != WB_TAG_STR) wb_tag_fail("head_str", WB_TAG_STR, c->elem_tag);
  return c->head ? (const char *)(uintptr_t)c->head : "";
}

int8_t _CG_wb_head_bool(int64_t xs) {
  WbCons *c = (WbCons *)(uintptr_t)xs;
  if (!c) return 0;
  if (c->elem_tag != WB_TAG_BOOL) wb_tag_fail("head_bool", WB_TAG_BOOL, c->elem_tag);
  return c->head != 0 ? 1 : 0;
}

int64_t _CG_wb_head_list(int64_t xs) {
  WbCons *c = (WbCons *)(uintptr_t)xs;
  if (!c) return 0;
  if (c->elem_tag != WB_TAG_LIST) wb_tag_fail("head_list", WB_TAG_LIST, c->elem_tag);
  return c->head;
}

int64_t _CG_wb_tail(int64_t xs) {
  WbCons *c = (WbCons *)(uintptr_t)xs;
  return c ? (int64_t)(uintptr_t)c->tail : 0;
}

int64_t _CG_wb_is_nil(int64_t xs) { return xs == 0 ? 1 : 0; }

int64_t _CG_wb_list_len(int64_t xs) {
  int64_t n = 0;
  for (WbCons *c = (WbCons *)(uintptr_t)xs; c; c = c->tail) ++n;
  return n;
}

int64_t _CG_wb_match_fail(void) {
  fprintf(stderr, "langwb: no pattern matched the value\n");
  exit(1);
}

void _CG_wb_emit_i64(int64_t v) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
  if (n > 0) wb_emit_bytes(buf, (size_t)n);
}

void _CG_wb_emit_bool(int8_t v) { wb_emit_cstr(v ? "true" : "false"); }

void _CG_wb_emit_raw(const char *s) { wb_emit_cstr(s); }

// Objects whose printer is currently running (innermost last), so a value that
// reaches itself prints `<cycle>` instead of recursing forever, and absurdly
// deep (but acyclic) structures stop with `...`.
#define WB_PRINT_MAX_DEPTH 1000
static const void *g_wb_print_stack[WB_PRINT_MAX_DEPTH];
static int g_wb_print_depth = 0;

// 0: print it (pushed; call _CG_wb_print_exit after), 1: cycle, 2: too deep.
int32_t _CG_wb_print_enter(const void *obj) {
  for (int i = 0; i < g_wb_print_depth; ++i) {
    if (g_wb_print_stack[i] == obj) return 1;
  }
  if (g_wb_print_depth >= WB_PRINT_MAX_DEPTH) return 2;
  g_wb_print_stack[g_wb_print_depth++] = obj;
  return 0;
}

void _CG_wb_print_exit(void) {
  if (g_wb_print_depth > 0) --g_wb_print_depth;
}

void _CG_wb_emit_str(const char *s) {
  wb_emit_bytes("\"", 1);
  if (s) {
    for (const char *p = s; *p; ++p) {
      switch (*p) {
        case '"': wb_emit_bytes("\\\"", 2); break;
        case '\\': wb_emit_bytes("\\\\", 2); break;
        case '\n': wb_emit_bytes("\\n", 2); break;
        case '\r': wb_emit_bytes("\\r", 2); break;
        case '\t': wb_emit_bytes("\\t", 2); break;
        default: wb_emit_bytes(p, 1); break;
      }
    }
  }
  wb_emit_bytes("\"", 1);
}

void _CG_wb_emit_list(int64_t xs) {
  wb_emit_bytes("[", 1);
  int first = 1;
  for (WbCons *c = (WbCons *)(uintptr_t)xs; c; c = c->tail) {
    if (!first) wb_emit_bytes(", ", 2);
    first = 0;
    switch (c->elem_tag) {
      case WB_TAG_INT: _CG_wb_emit_i64(c->head); break;
      case WB_TAG_BOOL: _CG_wb_emit_bool((int8_t)(c->head != 0)); break;
      case WB_TAG_STR: _CG_wb_emit_str((const char *)(uintptr_t)c->head); break;
      case WB_TAG_LIST: _CG_wb_emit_list(c->head); break;
      default: _CG_wb_emit_raw("<val>"); break;
    }
  }
  wb_emit_bytes("]", 1);
}

// --- I/O primitives (always write to stdout) ---------------------------------

int64_t _CG_wb_print_i64(int64_t v, int64_t nl) {
  printf("%lld", (long long)v);
  if (nl) putchar('\n');
  fflush(stdout);
  return 0;
}

int64_t _CG_wb_print_bool(int8_t v, int64_t nl) {
  fputs(v ? "true" : "false", stdout);
  if (nl) putchar('\n');
  fflush(stdout);
  return 0;
}

int64_t _CG_wb_print_str(const char *s, int64_t nl) {
  if (s) fputs(s, stdout);
  if (nl) putchar('\n');
  fflush(stdout);
  return 0;
}

int64_t _CG_wb_print_list(int64_t xs, int64_t nl) {
  WbEmitSinkFn saved = g_wb_emit_sink;
  g_wb_emit_sink = NULL;
  _CG_wb_emit_list(xs);
  g_wb_emit_sink = saved;
  if (nl) putchar('\n');
  fflush(stdout);
  return 0;
}

// --- String primitives (GC-allocated) ----------------------------------------

const char *_CG_wb_str_concat(const char *a, const char *b) {
  size_t la = a ? strlen(a) : 0;
  size_t lb = b ? strlen(b) : 0;
  char *out = (char *)GC_MALLOC_ATOMIC(la + lb + 1);
  if (la > 0) memcpy(out, a, la);
  if (lb > 0) memcpy(out + la, b, lb);
  out[la + lb] = '\0';
  return out;
}

int64_t _CG_wb_str_len(const char *s) { return s ? (int64_t)strlen(s) : 0; }

int8_t _CG_wb_str_eq(const char *a, const char *b) {
  if (!a) a = "";
  if (!b) b = "";
  return strcmp(a, b) == 0 ? 1 : 0;
}

const char *_CG_wb_str_slice(const char *s, int64_t start, int64_t end) {
  size_t len = s ? strlen(s) : 0;
  if (start < 0) start = 0;
  if (end < 0) end = 0;
  if ((size_t)start > len) start = (int64_t)len;
  if ((size_t)end > len) end = (int64_t)len;
  if (end < start) end = start;
  size_t n = (size_t)(end - start);
  char *out = (char *)GC_MALLOC_ATOMIC(n + 1);
  if (n > 0) memcpy(out, s + start, n);
  out[n] = '\0';
  return out;
}

const char *_CG_wb_int_to_str(int64_t v) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
  if (n < 0) n = 0;
  char *out = (char *)GC_MALLOC_ATOMIC((size_t)n + 1);
  memcpy(out, buf, (size_t)n);
  out[n] = '\0';
  return out;
}

int64_t _CG_wb_str_to_int(const char *s) {
  if (!s) return 0;
  return (int64_t)strtoll(s, NULL, 10);
}
