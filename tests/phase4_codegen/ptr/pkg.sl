package demo

pub {
    struct Foo {
        x i32
    }

    fn Set(f *Foo, v i32) void
}

fn Set(f *Foo, v i32) void {
    f.x = v
}
