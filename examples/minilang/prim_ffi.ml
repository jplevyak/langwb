# prim_ffi.ml — Zero-C++ custom IFA transfer function + libc FFI (`labs`)!
#
# Declare `my_abs` backed by libc `labs`, with a custom IFA transfer function
# written purely in minilang (pre-compiled once to native LLJIT code):
prim my_abs c_call "labs" transfer =>
  if fa_arg_is_const_i64(0) != 0
  then
    let v = fa_arg_const_i64(0) in
    fa_emit_const_i64(if v < 0 then 0 - v else v)
  else
    fa_emit_i64();

# IFA folds `my_abs(0 - 4)` -> constant 4 via our guest transfer function,
# triggering Stage-1 `pow_opt(2, e)` unrolling to `2*2*2*2 = 16`,
# while `my_abs(runtime_val(0 - 7))` calls libc `labs(-7)` -> 7 at runtime!
let e = my_abs(0 - 4) in
pow_opt(2, e) + my_abs(runtime_val(0 - 7))
