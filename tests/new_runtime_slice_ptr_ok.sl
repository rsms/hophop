
fn main() {
    var ma *Allocator = 0 as *Allocator
    var n uint = 4
    var a *[i32] = new [i32 n] with ma
    var b *[i32] = new [i32 4] with ma
    assert len(a) == 4
    assert len(b) == 4
}
