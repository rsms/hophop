// Verifies context clause mismatch is rejected.
struct MemAllocator {
	impl fn(*MemAllocator, rawptr, int, int, *int, u32) rawptr
}

struct CtxA {
	mem *MemAllocator
}

struct CtxB {
	mem *MemAllocator
}

fn f() context CtxA

fn f() context CtxB {}
