fn add1(x i32) i32 {
    return x + 1
}

fn mul2(x i32) i32 {
    return x * 2
}

const N = mul2(add1(2))

fn main() {
    var a [i32 N]
    assert N == 6
    assert a[0] == 0
}
