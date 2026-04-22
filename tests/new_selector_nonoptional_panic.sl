// Verifies new selector non-optional panics.
import "testing" { FailingAllocator }

fn main() {
	var ma = FailingAllocator{}
	context.allocator = ma
	var _p *i32 = new i32
}
