# Contour-aware Stage-1 metaprogramming & demand-driven constant splitting:
#   - `sum_rec` is called with two different record shapes ({x,y} and {a,b});
#     IFA splits it into 2 type contours, and Stage-1 specializes each contour
#     with decision-keyed syntactic cloning.
#   - `pow2` is called with three different constant exponents (3, 4, and 1);
#     `pow_opt`'s constant query triggers demand-driven splitting so each call
#     site gets its own unrolled clone.
let sum_rec = fn(p) => sum_fields(p) in
let pow2 = fn(n) => pow_opt(2, n) in
sum_rec({ x: 10, y: 2 }) + sum_rec({ a: 3, b: 1 }) + pow2(3) + pow2(4) + pow2(1)
