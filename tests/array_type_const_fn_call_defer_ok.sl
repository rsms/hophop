fn next(x i32) i32 {
    var y i32 = 0
    defer y += x
    y = x
    return 2
}

fn main() {
    var a [i32 next(2)]
    assert a[0] == 0
}
