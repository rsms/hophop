// Verifies unknown context field is rejected.
struct Allocator {
	impl fn(*Allocator, rawptr, int, int, *int, u32) rawptr
}

struct Ctx {
	mem *Allocator
}

fn f() context Ctx {}

fn g() context Ctx {
	f() context { nope: context.mem }
}
