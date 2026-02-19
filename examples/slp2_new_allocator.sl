// allocator-driven `new` forms for value, fixed-array, and slice allocations
import "std/mem" as mem

struct Pair {
    x i32
    y i32
}

fn main() {
    var ma = mem.platformAllocator

    var one *Pair = new(ma, Pair)
    var many *[i32 8] = new(ma, i32, 8)
    one.x = 20
    one.y = 22
    many[0] = one.x + one.y
    assert many[0] == 42
}
