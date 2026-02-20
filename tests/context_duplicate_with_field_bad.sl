struct Ctx {
    mem mut&__sl_MemAllocator
}

fn f() context Ctx {}

fn g() context Ctx {
    f() with { mem = context.mem, mem = context.mem }
}
