// Verifies new builtin allocator is accepted.
fn main() {
	var ma      = context.mem
	var _p *i32 = new i32 with ma
}
