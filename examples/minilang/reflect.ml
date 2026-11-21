# reflect.ml — polymorphic function + record + IFA-driven Stage-1 reflection (`sum_fields`)
let id = fn(v) => v in
let p = id({ x: 10, y: 20, z: 12 }) in
sum_fields(p)
