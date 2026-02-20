struct CtxA {
    mem mut&__sl_MemAllocator
}

struct CtxB {
    mem mut&__sl_MemAllocator
}

fn f() context CtxA;
fn f() context CtxB {}
