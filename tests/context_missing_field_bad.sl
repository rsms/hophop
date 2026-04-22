// Verifies context missing field is rejected.
struct MemAllocator {
	impl fn(*MemAllocator, rawptr, int, int, *int, u32) rawptr
}

struct TmpCtx {
	tmpmem *MemAllocator
}

struct MemCtx {
	mem *MemAllocator
}

fn log_event(msg &str) context TmpCtx {}

fn example() context MemCtx {
	log_event("hello")
}
