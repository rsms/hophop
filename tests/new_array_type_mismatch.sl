import "std/mem" as mem

fn foo(ma mut&mem.Allocator) {
    var many *i32 = new(ma, i32, 8)
    _ = many
}
