// Verifies mem arena allocator API is accepted.
import "mem"

fn main() {
	var arena mem.ArenaAllocator
	arena.init(&context.allocator, block_size: 64)

	var p *i32 = new i32
	*p = 7
	assert *p == 7
	del p

	arena.free_all()
}
