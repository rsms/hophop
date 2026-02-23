import "std/mem"

fn foo(ma *Allocator) {
    var many *i32 = new(ma, i32, 8)
    _ = many
}
