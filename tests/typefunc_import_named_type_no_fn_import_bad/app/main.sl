// Supports type function import named type no function import by providing the app entrypoint.
import "lib/mem" { ArenaAllocator }

fn main() {
	var arena ArenaAllocator
	init(&arena, 1024)
}
