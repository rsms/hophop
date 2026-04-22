// Verifies optional new returns null when the ambient allocator fails.
import "testing" { FailingAllocator }

fn main() {
	var ma = FailingAllocator{}
	context.allocator = ma
	var p ?*i32 = new i32
	assert p == null
}
