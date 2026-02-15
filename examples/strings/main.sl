package strings

pub {
    fn Main() i32
}

fn Main() i32 {
    var s str = "hello\n"
    var l u32 = len(s)

    assert l > 0
    assert l > 1, "len=%d", l

    cstr(s)
    return l as i32
}
