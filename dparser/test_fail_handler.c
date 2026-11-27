/* Tests d_set_fail_handler / d_set_warn_handler. */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "d.h"
#include "gram.h"
#include "mkdparse.h"

static jmp_buf jb;
static char last[1024];
static int warns = 0;

static void on_fail(const char *msg) {
  snprintf(last, sizeof(last), "%s", msg);
  longjmp(jb, 1);
}
static void on_warn(const char *msg) {
  snprintf(last, sizeof(last), "%s", msg);
  ++warns;
}

static int failures = 0;
#define CHECK(c, what)                                   \
  do {                                                   \
    if (!(c)) {                                          \
      printf("FAILED: %s\n", what);                      \
      ++failures;                                        \
    }                                                    \
  } while (0)

int main(void) {
  CHECK(d_set_fail_handler(on_fail) == NULL, "no previous fail handler");
  if (!setjmp(jb)) {
    d_fail("bad thing %d", 42);
    CHECK(0, "d_fail returned");
  }
  CHECK(strcmp(last, "bad thing 42") == 0, "fail message formatted without prefix");

  /* A grammar error inside the grammar compiler reaches the handler. */
  last[0] = 0;
  if (!setjmp(jb)) {
    Grammar *g = new_D_Grammar((char *)"test_fail_handler");
    mkdparse_from_string(g, (char *)"S : 'a' undefined_nonterminal;\n");
    CHECK(0, "mkdparse_from_string returned after an unresolved identifier");
  }
  CHECK(strstr(last, "undefined_nonterminal") != NULL, "unresolved identifier reported to the handler");

  CHECK(d_set_warn_handler(on_warn) == NULL, "no previous warn handler");
  d_warn("careful %s", "now");
  CHECK(warns == 1 && strcmp(last, "careful now") == 0, "warn message reaches the handler");

  CHECK(d_set_fail_handler(NULL) == on_fail, "setter returns the previous fail handler");
  CHECK(d_set_warn_handler(NULL) == on_warn, "setter returns the previous warn handler");
  if (failures) return 1;
  printf("fail/warn handler tests passed\n");
  return 0;
}
