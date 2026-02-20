struct TmpCtx {
    tmpmem mut&__sl_MemAllocator
}

struct MemCtx {
    mem mut&__sl_MemAllocator
}

fn log_event(msg str) context TmpCtx {
}

fn example() context MemCtx {
    log_event("hello")
}
