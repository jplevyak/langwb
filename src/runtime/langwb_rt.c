#include <gc/gc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*WbEmitSinkFn)(const char *data, size_t len);
static WbEmitSinkFn g_wb_emit_sink = NULL;

void _CG_wb_set_emit_sink(WbEmitSinkFn fn) { g_wb_emit_sink = fn; }

static void wb_emit_bytes(const char *data, size_t len) {
  if (!data || len == 0) return;
  if (g_wb_emit_sink) {
    g_wb_emit_sink(data, len);
  } else {
    fwrite(data, 1, len, stdout);
  }
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
  int64_t head;
  struct WbCons *tail;
} WbCons;

int64_t _CG_wb_nil(void) { return 0; }

int64_t _CG_wb_cons(int64_t h, int64_t t) {
  WbCons *c = (WbCons *)GC_MALLOC(sizeof(WbCons));
  c->head = h;
  c->tail = (WbCons *)(uintptr_t)t;
  return (int64_t)(uintptr_t)c;
}

int64_t _CG_wb_head(int64_t xs) {
  WbCons *c = (WbCons *)(uintptr_t)xs;
  return c ? c->head : 0;
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
  fprintf(stderr, "langwb: non-exhaustive match\n");
  exit(1);
}

void _CG_wb_emit_i64(int64_t v) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
  if (n > 0) wb_emit_bytes(buf, (size_t)n);
}

void _CG_wb_emit_bool(int8_t v) { wb_emit_cstr(v ? "true" : "false"); }

void _CG_wb_emit_raw(const char *s) { wb_emit_cstr(s); }

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
    _CG_wb_emit_i64(c->head);
  }
  wb_emit_bytes("]", 1);
}
