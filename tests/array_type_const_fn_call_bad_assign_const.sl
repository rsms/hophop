fn bad() i32 {
    const x i32 = 1
    x = 2
    return x
}

fn main() {
    var a [i32 bad()]
    _ = a
}
