type MyInt int

fn a_myint(x MyInt) int {
    return (x as int) + 1
}

fn a_int(x int) int {
    return x + 2
}

fn a{a_myint, a_int};

fn main() {
    var v MyInt = 3 as MyInt
    assert v.a() == 4
    assert a(v) == 4 // exact alias overload wins over alias->target conversion
}
