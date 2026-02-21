struct Ctx {
    mem *__sl_MemAllocator
}

fn f() context Ctx {}

fn g() context Ctx {
    f() with { nope = context.mem }
}
