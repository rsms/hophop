// app package importing another local package from the same examples tree

import "math" as math

fn main() {
    var x = math.Add(20, 22)
    assert x > 40, "x=%d", x
    assert x == 42
}
