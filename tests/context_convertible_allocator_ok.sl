import "std/mem"

struct NeedCtx {
    mem mut&__sl_MemAllocator
}

fn a() context NeedCtx {
    var p *i32 = new(i32)
}

struct MyMem {
    mem.Allocator
}

struct WrapCtx {
    mem mut&MyMem
}

fn caller() context WrapCtx {
    a()
}

fn main() {
    var wrapped MyMem

    a() with { mem = mem.platformAllocator }
    a() with { mem = &wrapped }
    caller() with { mem = &wrapped }
}
