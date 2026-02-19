fn main() {
    var s str = "hello\n"
    var l u32 = len(s)

    assert l > 0
    assert l > 1, "len=%d", l

    cstr(s)
    assert l == 6
}
