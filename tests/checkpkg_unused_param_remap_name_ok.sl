// Verifies checkpkg unused parameter remap name is accepted.
import "mem" { ArenaAllocator }

fn cleanup(self *ArenaAllocator) {}

fn main() {
	var arena ArenaAllocator
	cleanup(&arena)
}
