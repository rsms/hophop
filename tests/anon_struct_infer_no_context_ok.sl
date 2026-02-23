fn use(v { a int, b &str }) int {
    return v.a
}

fn main() {
    var x = { a = 1, b = "hello" }
    var y int = use(x)
    assert(y == 1)
}
