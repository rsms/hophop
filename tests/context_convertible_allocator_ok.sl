// Verifies ambient allocator field is accepted.
fn a() {
	var p *i32 = new i32
	del p
}

fn main() {
	a()
}
