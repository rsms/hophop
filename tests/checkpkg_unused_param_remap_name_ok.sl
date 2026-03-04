import "mem" { ArenaAllocator }

fn cleanup(self *ArenaAllocator) {}

fn main() {
	var arena ArenaAllocator
	cleanup(&arena)
}
