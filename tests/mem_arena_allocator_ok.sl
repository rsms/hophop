// Verifies mem arena allocator is accepted.
import "mem"

fn init(self *mem.ArenaAllocator, source *Allocator, block_size uint) {
	mem.init(self, source, block_size)
}

fn free_all(self *mem.ArenaAllocator) {
	mem.free_all(self)
}

fn takes_allocator(ma *Allocator) *i32 {
	var p *i32 = new i32 with ma
	*p = 7
	return p
}

fn contextual_alloc() *[i32] context struct {
	mem *Allocator
} {
	var n  uint   = 4
	var xs *[i32] = new [i32 n]
	xs[0] = 1
	xs[3] = 9
	return xs
}

fn main() {
	var arena mem.ArenaAllocator
	arena.init(context.mem, block_size: 64)

	var p *i32 = takes_allocator(&arena)
	assert *p == 7

	var xs *[i32] = contextual_alloc() with { mem: &arena }
	assert len(xs) == 4
	assert xs[0] == 1
	assert xs[3] == 9

	arena.free_all()

	var q *i32 = takes_allocator(&arena)
	assert *q == 7
	arena.free_all()
}
