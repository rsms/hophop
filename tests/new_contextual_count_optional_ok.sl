struct Ctx {
    mem *Allocator
}

fn allocs(n uint) context Ctx {
    var a ?*[i32 4] = new(i32, 4)
    var b ?*[i32] = new(i32, n)
    assert a == null
    assert b == null
}

fn main() {
    var ma *Allocator = 0 as *Allocator
    allocs(4) with { mem = ma }
}
