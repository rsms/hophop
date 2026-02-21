import "std/mem"

struct NeedCtx {
    mem *__sl_MemAllocator
}

fn a() context NeedCtx {
    var p *i32 = new(i32)
}

fn main() {
    a() with { mem = mem.platformAllocator }
}
