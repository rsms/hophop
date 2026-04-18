// Verifies context unknown with field is rejected.
struct Allocator {
	impl fn(*Allocator, uint, uint, uint, *uint, u32) uint
}

struct Ctx {
	mem *Allocator
}

fn f() context Ctx {}

fn g() context Ctx {
	f() with { nope: context.mem }
}
