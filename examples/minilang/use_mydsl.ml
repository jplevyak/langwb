#lang mydsl
# Uses `magic` (from #%module-begin), `when..do..otherwise` (grammar + form_macro),
# `my_abs` (FFI + custom IFA transfer fn), `pick_const` (Stage-1), and `triple` (hygienic macro).
when my_abs(0 - 5) > 2 do
  pick_const(magic + 1, triple(magic + 4), 0)
otherwise
  999
