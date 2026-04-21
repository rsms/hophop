// Verifies the Wasm backend accepts custom allocator.
struct MyAllocator {
	Allocator
	mem *Allocator
}

fn custom_alloc_impl(self *Allocator, addr rawptr, align, curSize uint, newSizeInOut *uint, flags u32) rawptr {
	var ma = self as *MyAllocator
	return ma.mem.impl(ma.mem, addr, align, curSize, newSizeInOut, flags)
}

fn main() {
	var ma MyAllocator
	ma.mem = context.mem

	var a *Allocator = &ma
	a.impl = custom_alloc_impl
	var p *i32 = new i32 context a
	*p = 7
	assert *p == 7

	var n  uint   = 3
	var xs *[i32] = new [i32 n] context a
	xs[0] = 1
	xs[2] = 9
	assert len(xs) == 3
	assert xs[0] == 1
	assert xs[2] == 9
}
