// Verifies new builtin allocator is accepted.
fn main() {
	var ma *Allocator = null as *Allocator
	var _p *i32       = new i32 with ma
}
