# comptime.ml — in-language CTFE via IFA+LLJIT feeding into IFA constant prop -> Stage-1 unrolling
let n = comptime(let sq = fn(x) => x * x in sq(3) - 6) in
pow_opt(2, n)
