struct CtxA {
    mem *__sl_MemAllocator
}

struct CtxB {
    mem *__sl_MemAllocator
}

fn f() context CtxA;
fn f() context CtxB {}
