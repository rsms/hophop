// Verifies unknown context field is rejected.
struct MemAllocator {
	impl fn(*MemAllocator, rawptr, int, int, *int, u32) rawptr
}

struct Ctx {
	mem *MemAllocator
}

fn f() context Ctx {}

fn g() context Ctx {
	f() context { nope: context.allocator }
}
