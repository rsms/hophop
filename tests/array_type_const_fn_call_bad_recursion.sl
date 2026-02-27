fn rec(x i32) i32 {
    return rec(x)
}

fn main() {
    var a [i32 rec(1)]
    _ = a
}
