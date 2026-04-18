// Verifies context clause mismatch is rejected.
struct Allocator {
	impl fn(*Allocator, uint, uint, uint, *uint, u32) uint
}

struct CtxA {
	mem *Allocator
}

struct CtxB {
	mem *Allocator
}

fn f() context CtxA

fn f() context CtxB {}
