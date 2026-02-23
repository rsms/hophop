pub fn Main(x i32) i32

fn Main(x i32) i32 {
    var a &str = "hello"
    var b &str = "hello"
    var n u32 = len(a)

    cstr(b)
    assert x >= 0
    assert x > 1, "x=%d", x
    assert n > 0

    return x
}
