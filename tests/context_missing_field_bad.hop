// Verifies context missing field is rejected.
struct Allocator {
	impl fn(*Allocator, rawptr, int, int, *int, u32) rawptr
}

struct TmpCtx {
	tmpmem *Allocator
}

struct MemCtx {
	mem *Allocator
}

fn log_event(msg &str) context TmpCtx {}

fn example() context MemCtx {
	log_event("hello")
}
