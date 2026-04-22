// Verifies import builtin is accepted.
import "builtin"

fn main() {
	var mem *builtin.MemAllocator = (null as rawptr) as *builtin.MemAllocator
	_ = mem
}
