// Verifies context clause mismatch is rejected.
struct Allocator {
	impl fn(*Allocator, rawptr, int, int, *int, u32) rawptr
}

struct CtxA {
	mem *Allocator
}

struct CtxB {
	mem *Allocator
}

fn f() context CtxA

fn f() context CtxB {}
