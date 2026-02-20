import "std/mem"

fn main() {
    var ma mut&mem.Allocator = 0 as mut&mem.Allocator
    var n uint = 4
    var a *[i32] = new(ma, i32, n)
    var b *[i32] = new(ma, i32, 4)
    assert len(a) == 4
    assert len(b) == 4
}
