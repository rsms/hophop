struct Ctx {
    mem mut&__sl_MemAllocator
}

fn allocs(n uint) context Ctx {
    var a ?*[i32 4] = new(i32, 4)
    var b ?*[i32] = new(i32, n)
    assert a == null
    assert b == null
}

fn main() {
    var ma mut&__sl_MemAllocator = 0 as mut&__sl_MemAllocator
    allocs(4) with { mem = ma }
}
