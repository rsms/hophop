import "std/mem"

fn main() {
    var ma *Allocator = 0 as *Allocator
    var n uint = 4
    var a *[i32] = new(ma, i32, n)
    var b *[i32] = new(ma, i32, 4)
    assert len(a) == 4
    assert len(b) == 4
}
