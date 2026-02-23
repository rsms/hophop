struct Allocator {
    impl fn(self *Allocator, addr, align, curSize uint, newSizeInOut *uint, flags u32) uint
}

struct Ctx {
    mem *Allocator
}

fn f() context Ctx {}

fn g() context Ctx {
    f() with { mem = context.mem, mem = context.mem }
}
