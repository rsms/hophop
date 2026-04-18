// Verifies context missing field is rejected.
struct Allocator {
	impl fn(*Allocator, uint, uint, uint, *uint, u32) uint
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
