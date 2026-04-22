import "mem" { ArenaAllocator }

fn arena_init(self *ArenaAllocator, source *Allocator, block_size int) {
	self.mem = source
	self.block_size = block_size
	if self.block_size == 0 {
		self.block_size = 4096
	}
}

fn arena_free_all(self *ArenaAllocator) {}

fn fill_values(arena *ArenaAllocator) i32 {
	var p *i32 = new i32 context arena.mem
	*p = 10

	var n  uint   = 4
	var xs *[i32] = new [i32 n] context arena.mem
	xs[0] = 1
	xs[1] = 2
	xs[2] = 3
	xs[3] = 4
	return *p + xs[0] + xs[1] + xs[2] + xs[3]
}

fn main() {
	var arena ArenaAllocator
	arena_init(&arena, source: context.mem, block_size: 1024)

	var sum = fill_values(&arena)
	assert sum == 20

	arena_free_all(&arena)
}
