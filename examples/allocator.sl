// Demonstrates every `new` call style:
// - explicit allocator argument: `new(ma, T[, N])`
// - selector sugar: `ma.new(T[, N])`
// - contextual allocator: `new(T[, N])` with `mem` in context
import "std/mem"

struct Pair {
    x i32
    y i32
}

fn explicit_forms(ma *mem.Allocator, n u32) {
    var p *Pair = new(ma, Pair)
    var fixed *[i32 4] = new(ma, i32, 4)
    var dyn *[i32] = new(ma, i32, n)

    p.x = 42
    p.y = 0
    fixed[0] = p.x
    dyn[0] = fixed[0]
    assert fixed[0] == 42
    assert dyn[0] == 42
    assert len(dyn) == n
}

fn selector_forms(ma *mem.Allocator, n u32) {
    var p *Pair = ma.new(Pair)
    var fixed *[i32 4] = ma.new(i32, 4)
    var dyn *[i32] = ma.new(i32, n)

    p.x = 42
    p.y = 0
    fixed[0] = p.x
    dyn[0] = fixed[0]
    assert fixed[0] == 42
    assert dyn[0] == 42
    assert len(dyn) == n
}

fn contextual_forms(n u32) context { mem *mem.Allocator } {
    var p *Pair = new(Pair)
    var fixed *[i32 4] = new(i32, 4)
    var dyn *[i32] = new(i32, n)

    p.x = 42
    p.y = 0
    fixed[0] = p.x
    dyn[0] = fixed[0]
    assert fixed[0] == 42
    assert dyn[0] == 42
    assert len(dyn) == n
}

fn main() {
    var ma = mem.platformAllocator
    var n u32 = 6

    explicit_forms(ma, n)
    selector_forms(ma, n)
    contextual_forms(n) with { mem = ma }
}
