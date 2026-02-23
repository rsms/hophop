struct Allocator {
    impl fn(self *Allocator, addr, align, curSize uint, newSizeInOut *uint, flags u32) uint
}

struct TmpCtx {
    tmpmem *Allocator
}

struct MemCtx {
    mem *Allocator
}

fn log_event(msg str) context TmpCtx {
}

fn example() context MemCtx {
    log_event("hello")
}
