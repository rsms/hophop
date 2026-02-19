import "std/mem" as mem

fn main() {
    var ma = mem.platformAllocator
    var p *i32 = ma.new(i32)
    var q *[i32 4] = ma.new(i32, 4)

    *p = 7
    q[0] = *p
    assert q[0] == 7
}
