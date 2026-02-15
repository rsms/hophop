package app

import math "math"

pub {
    fn Main() i32
}

fn Main() i32 {
    var x i32 = math.Add(20, 22)
    assert x > 40, "x=%d", x
    return x
}
