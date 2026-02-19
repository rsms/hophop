import "b"

fn example1(v b.B) int {
    return v.a.x
}

fn example2() int {
    var result = b.make_a()
    return result.x
}

fn example3() int {
    var v = b.make_b()
    return v.a.x
}
