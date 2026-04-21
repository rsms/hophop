// Verifies new non-optional panics.
import "std/testing" { FailingAllocator }

fn main() {
	var ma            = FailingAllocator{}
	var a  *Allocator = &ma
	var _p *i32       = new i32 with a
}
