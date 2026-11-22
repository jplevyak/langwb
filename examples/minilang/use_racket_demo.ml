#lang racket_demo
# Exercises:
#   - `when..do..otherwise` (declarative `syntax_rule form` with `:expr` classes)
#   - `triple` (declarative `syntax_rule` hygienic call macro)
#   - `bump` (uses shared `for_syntax` Phase-1 helper `wrap_add1`)
#   - `require_tagged(tagged(...), 0)` (`stx_local_expand` + `stx_set_prop`/`stx_get_prop`)
#   - `require_tagged(999, 1)` (untagged -> falls back to `1`)
# Computation: triple(bump(12)) + 3 = triple(13) + 3 = 39 + 3 = 42
let tmp = 12 in
when 3 > 1 do
  require_tagged(tagged(triple(bump(tmp))), 0) + require_tagged(999, 3)
otherwise
  0
