// Verifies context duplicate with field is rejected.
struct Allocator {
	impl fn(*Allocator, rawptr, uint, uint, *uint, u32) rawptr
}

struct Ctx {
	mem *Allocator
}

fn f() context Ctx {}

fn g() context Ctx {
	f() with { mem, mem }
}
