type MyInt int

fn a(x int) int {
    return x + 1
}

fn main() {
    var v MyInt = 3 as MyInt
    assert a(v) == 4
    assert v.a() == 4
}
