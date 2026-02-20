type MyInt int

fn a_int(x int) int {
    return x + 1
}

fn a{a_int};

fn main() {
    var v MyInt = 3 as MyInt
    assert v.a() == 4
    assert a(v) == 4
}
