// Supports type function import named type with function import by providing the app entrypoint.
import "lib/mem" { ArenaAllocator, init }

fn main() {
	var arena ArenaAllocator
	init(&arena, block_size: 1024)
	assert arena.block_size == 1024
}
