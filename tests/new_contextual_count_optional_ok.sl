struct Ctx {
    mem *__sl_MemAllocator
}

fn allocs(n uint) context Ctx {
    var a ?*[i32 4] = new(i32, 4)
    var b ?*[i32] = new(i32, n)
    assert a == null
    assert b == null
}

fn main() {
    var ma *__sl_MemAllocator = 0 as *__sl_MemAllocator
    allocs(4) with { mem = ma }
}
