# hello.ml — core minilang with let bindings and custom inline LLVM primitive `clamp`
let x = 7 in
clamp(x * 2, 1, 10) + clamp(3, 1, 10)
