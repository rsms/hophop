fn count(n i32) i32 {
    assert n > 10, "n too small"
    return n + 1
}

fn main() {
    var a [i32 count(2)]
    assert a[0] == 0
}
