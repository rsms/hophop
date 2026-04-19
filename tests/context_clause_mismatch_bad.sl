// Verifies context clause mismatch is rejected.
struct Allocator {
	impl fn(*Allocator, rawptr, uint, uint, *uint, u32) rawptr
}

struct CtxA {
	mem *Allocator
}

struct CtxB {
	mem *Allocator
}

fn f() context CtxA

fn f() context CtxB {}
