// Verifies Wasm new non-optional panics on allocation failure.
import "testing" { FailingAllocator }

fn main() {
	var ma            = FailingAllocator{}
	var a  *Allocator = &ma
	var _p *i32       = new i32 context a
}
