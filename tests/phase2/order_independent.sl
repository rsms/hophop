package demo

struct Foo {
    parent *Bar
}

struct Bar {
    child Foo
}

fun a(x i32) i32 {
    if x == 0 {
        return 0
    }
    return b(x)
}

fun b(x i32) i32 {
    if x <= 0 {
        return a(x)
    }
    return x * 2
}
