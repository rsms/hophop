struct Allocator {
	impl fn(*Allocator, uint, uint, uint, *uint, u32) uint
}

struct Ctx {
	mem *Allocator
}

fn f() context Ctx {}

fn g() context Ctx {
	f() with { mem: context.mem, mem: context.mem }
}
