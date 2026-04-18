// Verifies new non-optional panics.
import "std/testing"

fn main() {
	var ma *Allocator = null as *Allocator
	var _p *i32       = new i32 with ma
}
